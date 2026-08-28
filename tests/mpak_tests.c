/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

  * Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the following
  disclaimer in the documentation and/or other materials provided
  with the distribution.

  * Neither the name of the MusicPack Development Team nor the names of
  its contributors may be used to endorse or promote products derived
  from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
 * C tests for the MPAK v1 single-file container: format framing, DATA
 * round-trips, INDX, MANF, TAIL, recovery, determinism, directory <->
 * MPAK equivalence, and the byte-exact Musepack handoff.
 *
 * Usage: mpak_tests <musicpack-album.mpack> <flac-album.mpack> <fixture.mpc>
 * Wired into CTest as the "mpak" suite.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
# include <windows.h>
# include <direct.h>
# define mkdir_one(p) _mkdir(p)
#else
# include <sys/stat.h>
# include <unistd.h>
# define mkdir_one(p) mkdir(p, 0777)
#endif

#include <musicpack/musicpack.h>
#include <musepack/musepack.h>

static int failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);    \
            failures++;                                                      \
        }                                                                    \
    } while (0)

#define MPAK_HDR 16u
#define MPAK_BHDR 14u

static const char *g_ref_album;
static const char *g_ref_flac;
static const char *g_fixture_mpc;

/* ------------------------------------------------------------------ */
/* scratch helpers                                                     */
/* ------------------------------------------------------------------ */

static int
make_temp_dir(char *buf, size_t cap)
{
#if defined(_WIN32)
    const char *base = getenv("TEMP");
    if (base == 0) base = ".";
    if (snprintf(buf, cap, "%s\\mpak_test_%lu", base,
                 (unsigned long) GetCurrentProcessId()) >= (int) cap)
        return -1;
    if (_mkdir(buf) != 0 && errno != EEXIST)
        return -1;
    return 0;
#else
    if (snprintf(buf, cap, "/tmp/mpak_test_XXXXXX") >= (int) cap)
        return -1;
    return mkdtemp(buf) != 0 ? 0 : -1;
#endif
}

static int
mkdir_p(const char *path)
{
    char tmp[4200];
    size_t len, i;

    len = strlen(path);
    if (len == 0 || len >= sizeof tmp)
        return -1;
    memcpy(tmp, path, len + 1);
    for (i = 1; tmp[i] != '\0'; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            tmp[i] = '\0';
            if (mkdir_one(tmp) != 0 && errno != EEXIST)
                return -1;
            tmp[i] = c;
        }
    }
    if (mkdir_one(tmp) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int
write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (f == 0)
        return -1;
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

static unsigned char *
read_file_all(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long len;

    if (f == 0)
        return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    buf = (unsigned char *) malloc(len > 0 ? (size_t) len : 1);
    if (buf == 0) {
        fclose(f);
        return 0;
    }
    if (len > 0 && fread(buf, 1, (size_t) len, f) != (size_t) len) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    *len_out = (size_t) len;
    return buf;
}

static void
join_path(char *out, size_t cap, const char *a, const char *b)
{
    snprintf(out, cap, "%s/%s", a, b);
}

/* Minimal one-track directory package with the fixture .mpc as audio and
   a same-length extras path (both 12 chars) for index tampering tests. */
static int
build_min_package_from(const char *root, const char *audio_rel,
                       const char *extra_rel, const char *audio_src)
{
    char path[4200], hash[MUSICPACK_SHA256_HEX_SIZE];
    char manifest[2048];
    unsigned char *mpc;
    size_t mpc_len;

    if (mkdir_p(root) != 0)
        return -1;
    join_path(path, sizeof path, root, "audio");
    if (mkdir_p(path) != 0)
        return -1;
    join_path(path, sizeof path, root, audio_rel);
    mpc = read_file_all(audio_src, &mpc_len);
    if (mpc == 0)
        return -1;
    if (write_file(path, mpc, mpc_len) != 0) {
        free(mpc);
        return -1;
    }
    free(mpc);
    if (musicpack_sha256_file(path, hash, sizeof hash) != MUSICPACK_OK)
        return -1;

    if (extra_rel != 0) {
        static const char note[] = "note\n";
        char ehash[MUSICPACK_SHA256_HEX_SIZE];
        join_path(path, sizeof path, root, extra_rel);
        /* parent dir of extra_rel must exist */
        {
            char dir[4200];
            char *slash = strrchr(path, '/');
            if (slash != 0) {
                size_t n = (size_t) (slash - path);
                memcpy(dir, path, n);
                dir[n] = '\0';
                if (mkdir_p(dir) != 0)
                    return -1;
            }
        }
        if (write_file(path, note, sizeof note - 1) != 0)
            return -1;
        if (musicpack_sha256_file(path, ehash, sizeof ehash) != MUSICPACK_OK)
            return -1;
        snprintf(manifest, sizeof manifest,
                 "{\n"
                 "  \"album\": { \"artists\": [ { \"name\": \"T\" } ], \"title\": \"M\" },\n"
                 "  \"format\": \"musicpack\",\n"
                 "  \"media\": [ { \"disc\": 1, \"tracks\": [ { \"audio\": { \"path\": \"%s\", \"sha256\": \"%s\" }, \"title\": \"t\", \"track\": 1 } ] } ],\n"
                 "  \"version\": 1,\n"
                 "  \"extras\": [ { \"path\": \"%s\", \"sha256\": \"%s\" } ]\n"
                 "}\n",
                 audio_rel, hash, extra_rel, ehash);
    } else {
        snprintf(manifest, sizeof manifest,
                 "{\n"
                 "  \"album\": { \"artists\": [ { \"name\": \"T\" } ], \"title\": \"M\" },\n"
                 "  \"format\": \"musicpack\",\n"
                 "  \"media\": [ { \"disc\": 1, \"tracks\": [ { \"audio\": { \"path\": \"%s\", \"sha256\": \"%s\" }, \"title\": \"t\", \"track\": 1 } ] } ],\n"
                 "  \"version\": 1\n"
                 "}\n",
                 audio_rel, hash);
    }
    join_path(path, sizeof path, root, "manifest.json");
    return write_file(path, manifest, strlen(manifest));
}

static int
build_min_package(const char *root, const char *audio_rel,
                  const char *extra_rel)
{
    return build_min_package_from(root, audio_rel, extra_rel, g_fixture_mpc);
}

/* packs a directory package into out (must succeed) */
static int
pack_ok(const char *dir, const char *out)
{
    musicpack_status s = MUSICPACK_OK;
    if (musicpack_mpak_pack_dir(dir, out, &s) != MUSICPACK_OK) {
        CHECK(0, "pack_dir failed");
        return -1;
    }
    return 0;
}

/* message-capturing verification */
typedef struct msg_bag {
    char msgs[64][160];
    int is_error[64];
    int count;
} msg_bag;

static void
bag_report(void *ctx, const char *message, int is_error)
{
    msg_bag *b = (msg_bag *) ctx;
    if (b->count < 64) {
        snprintf(b->msgs[b->count], sizeof b->msgs[0], "%s", message);
        b->is_error[b->count] = is_error;
        b->count++;
    }
}

static int
bag_has(const msg_bag *b, const char *needle)
{
    int i;
    for (i = 0; i < b->count; i++)
        if (strstr(b->msgs[i], needle) != 0)
            return 1;
    return 0;
}

static musicpack_status
open_and_verify(const char *path, msg_bag *bag)
{
    musicpack_package *pkg = musicpack_package_open(path, 0);
    musicpack_report rep = { 0, 0 };
    musicpack_status s;

    memset(bag, 0, sizeof *bag);
    if (pkg == 0)
        return (musicpack_status) -100; /* sentinel: open failed */
    s = musicpack_package_verify(pkg, &rep, bag_report, bag);
    musicpack_package_close(pkg);
    return s;
}

/* ------------------------------------------------------------------ */
/* container byte helpers                                              */
/* ------------------------------------------------------------------ */

static uint16_t
crc16_buypass(const unsigned char *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    size_t i;
    int bit;

    for (i = 0; i < len; i++) {
        crc ^= (uint16_t) (((uint16_t) data[i]) << 8);
        for (bit = 0; bit < 8; bit++)
            crc = (uint16_t) ((crc & 0x8000) ? (crc << 1) ^ 0x8005 : crc << 1);
    }
    return crc;
}

static uint64_t
rd64(const unsigned char *p)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

/* Walks the block stream; returns the payload offset of the nth (0-based)
   block with the given type, its payload length, and the block offset. */
static int
find_block(const unsigned char *buf, size_t len, const char *type,
           uint64_t *block_off, uint64_t *payload_off, uint64_t *payload_len)
{
    uint64_t pos = MPAK_HDR;

    while (pos + MPAK_BHDR <= (uint64_t) len) {
        uint64_t plen = rd64(buf + pos + 4);
        if (memcmp(buf + pos, type, 4) == 0) {
            *block_off = pos;
            *payload_off = pos + MPAK_BHDR;
            *payload_len = plen;
            return 0;
        }
        pos += MPAK_BHDR + plen;
    }
    return -1;
}

/* Writes a fresh 16-byte container header into buf. */
static void
write_header(unsigned char *buf, unsigned major, unsigned minor,
             unsigned flags)
{
    memset(buf, 0, MPAK_HDR);
    memcpy(buf, "MPAK", 4);
    buf[4] = (unsigned char) major;
    buf[5] = (unsigned char) minor;
    buf[6] = (unsigned char) (flags >> 8);
    buf[7] = (unsigned char) (flags & 0xFF);
}

typedef struct hb {
    unsigned char *buf;
    size_t len, cap;
} hb;

static void
hb_put(hb *h, const void *data, size_t len)
{
    if (h->len + len > h->cap) {
        size_t ncap = h->cap == 0 ? 256 : h->cap * 2;
        unsigned char *nb;
        while (ncap < h->len + len)
            ncap *= 2;
        nb = (unsigned char *) realloc(h->buf, ncap);
        if (nb == 0)
            return;
        h->buf = nb;
        h->cap = ncap;
    }
    memcpy(h->buf + h->len, data, len);
    h->len += len;
}

static void
hb_header(hb *h, unsigned major, unsigned minor, unsigned flags)
{
    unsigned char hdr[MPAK_HDR];
    write_header(hdr, major, minor, flags);
    hb_put(h, hdr, sizeof hdr);
}

static void
hb_block(hb *h, const char *type, const unsigned char *payload, size_t len)
{
    unsigned char bhdr[MPAK_BHDR];
    int i;

    memcpy(bhdr, type, 4);
    for (i = 0; i < 8; i++)
        bhdr[4 + i] = (unsigned char) ((uint64_t) len >> (56 - i * 8));
    {
        uint16_t crc = crc16_buypass(bhdr, 12);
        bhdr[12] = (unsigned char) (crc >> 8);
        bhdr[13] = (unsigned char) crc;
    }
    hb_put(h, bhdr, sizeof bhdr);
    if (len > 0)
        hb_put(h, payload, len);
}

static void
hb_free(hb *h)
{
    free(h->buf);
    h->buf = 0;
    h->len = h->cap = 0;
}

/* splices [cut_start, cut_end) out of the buffer */
static unsigned char *
splice_out(const unsigned char *src, size_t len, size_t cut_start,
           size_t cut_end, size_t *new_len)
{
    unsigned char *out = (unsigned char *) malloc(len - (cut_end - cut_start));
    if (out == 0)
        return 0;
    memcpy(out, src, cut_start);
    memcpy(out + cut_start, src + cut_end, len - cut_end);
    *new_len = len - (cut_end - cut_start);
    return out;
}

/* inserts bytes at pos */
static unsigned char *
splice_in(const unsigned char *src, size_t len, size_t pos,
          const unsigned char *ins, size_t ins_len, size_t *new_len)
{
    unsigned char *out = (unsigned char *) malloc(len + ins_len);
    if (out == 0)
        return 0;
    memcpy(out, src, pos);
    memcpy(out + pos, ins, ins_len);
    memcpy(out + pos + ins_len, src + pos, len - pos);
    *new_len = len + ins_len;
    return out;
}


/* Removes the TAIL block and the INDX block (clearing the flag bit):
   a scan-friendly variant whose only expected warnings are
   "missing INDX" and "completeness unproven". Returns a new buffer. */
static unsigned char *
strip_indx_and_tail(const unsigned char *buf, size_t len, size_t *new_len)
{
    uint64_t io, ipo, ipl, to, tpo, tpl;
    unsigned char *m1, *m2;
    size_t n1;

    if (find_block(buf, len, "TAIL", &to, &tpo, &tpl) != 0)
        return 0;
    m1 = splice_out(buf, len, (size_t) to,
                    (size_t) (to + MPAK_BHDR + tpl), &n1);
    if (m1 == 0)
        return 0;
    if (find_block(m1, n1, "INDX", &io, &ipo, &ipl) != 0) {
        free(m1);
        return 0;
    }
    m2 = splice_out(m1, n1, (size_t) io, (size_t) (io + MPAK_BHDR + ipl),
                    new_len);
    free(m1);
    if (m2 != 0)
        m2[7] &= (unsigned char) ~1u; /* clear INDX_PRESENT */
    return m2;
}

/* every asset path in the manifest (for equivalence comparisons) */
typedef struct asset_paths {
    char paths[4200][64]; /* 4096 max assets */
    size_t count;
} asset_paths;

static void
collect_paths(const musicpack_manifest *m, asset_paths *ap)
{
    size_t d, t, r, i;

    ap->count = 0;
    for (d = 0; d < m->disc_count; d++)
        for (t = 0; t < m->discs[d].track_count; t++) {
            const musicpack_track *tr = &m->discs[d].tracks[t];
            snprintf(ap->paths[ap->count++], 64, "%s", tr->audio.path);
            for (r = 0; r < tr->representation_count; r++)
                snprintf(ap->paths[ap->count++], 64, "%s",
                         tr->representations[r].path);
            if (tr->waveform.present)
                snprintf(ap->paths[ap->count++], 64, "%s", tr->waveform.path);
        }
    for (i = 0; i < m->artwork_count; i++)
        snprintf(ap->paths[ap->count++], 64, "%s", m->artwork[i].asset.path);
    for (i = 0; i < m->booklet_count; i++)
        snprintf(ap->paths[ap->count++], 64, "%s", m->booklet[i].path);
    for (i = 0; i < m->lyrics_count; i++)
        snprintf(ap->paths[ap->count++], 64, "%s", m->lyrics[i].path);
    for (i = 0; i < m->extras_count; i++)
        snprintf(ap->paths[ap->count++], 64, "%s", m->extras[i].path);
    for (i = 0; i < m->analysis_count; i++)
        snprintf(ap->paths[ap->count++], 64, "%s", m->analysis[i].asset.path);
}

/* ------------------------------------------------------------------ */
/* tests                                                               */
/* ------------------------------------------------------------------ */

static void
test_format_basics(const char *tmp)
{
    char root[4200], mpak[4200];
    unsigned char *buf;
    size_t len;
    musicpack_status s;
    msg_bag bag;
    musicpack_package *pkg;

    join_path(root, sizeof root, tmp, "fmt_pkg");
    join_path(mpak, sizeof mpak, tmp, "fmt.mpak");
    CHECK(build_min_package(root, "audio/01.mpc", 0) == 0, "build package");
    CHECK(pack_ok(root, mpak) == 0, "pack");

    buf = read_file_all(mpak, &len);
    CHECK(buf != 0, "read packed file");
    if (buf == 0)
        return;

    /* header */
    CHECK(memcmp(buf, "MPAK", 4) == 0, "magic");
    CHECK(buf[4] == 1, "major 1");
    CHECK(buf[5] == 0, "minor 0");
    CHECK(buf[6] == 0 && buf[7] == 1, "flags INDX_PRESENT");
    {
        int i, zero = 1;
        for (i = 8; i < 16; i++)
            if (buf[i] != 0)
                zero = 0;
        CHECK(zero, "reserved zero");
    }

    /* canonical block order: INDX, MANF, DATA..., TAIL */
    CHECK(memcmp(buf + 16, "INDX", 4) == 0, "INDX first");
    {
        uint64_t bo, po, pl;
        CHECK(find_block(buf, len, "MANF", &bo, &po, &pl) == 0, "MANF found");
        CHECK(bo > 16, "MANF after INDX");
        CHECK(find_block(buf, len, "TAIL", &bo, &po, &pl) == 0, "TAIL found");
        CHECK(pl == 52, "TAIL payload 52");
    }

    /* bad magic */
    buf[0] = 'X';
    write_file(mpak, buf, len);
    pkg = musicpack_package_open(mpak, &s);
    CHECK(pkg == 0 && s == MUSICPACK_ERR_INVALID, "bad magic rejected");
    if (pkg)
        musicpack_package_close(pkg);

    /* unsupported major */
    write_header(buf, 2, 0, 1);
    write_file(mpak, buf, len);
    pkg = musicpack_package_open(mpak, &s);
    CHECK(pkg == 0 && s == MUSICPACK_ERR_VERSION, "major 2 rejected");
    if (pkg)
        musicpack_package_close(pkg);

    /* unknown minor: tolerated with a verify warning. The header is
       covered by the TAIL digest, so these variants are built from the
       stripped (no TAIL, no INDX) form to isolate the semantics. */
    {
        unsigned char *stripped;
        size_t slen;
        stripped = strip_indx_and_tail(buf, len, &slen);
        CHECK(stripped != 0, "strip");
        if (stripped != 0) {
            write_header(stripped, 1, 9, 0);
            write_file(mpak, stripped, slen);
            s = open_and_verify(mpak, &bag);
            CHECK(s == MUSICPACK_OK, "unknown minor tolerated");
            CHECK(bag_has(&bag, "minor version 9"), "minor warning");
            CHECK(bag.count == 3, "minor + missing-index/completeness only");
            free(stripped);
        }
    }

    /* nonzero reserved: tolerated with a warning */
    {
        unsigned char *stripped;
        size_t slen;
        stripped = strip_indx_and_tail(buf, len, &slen);
        CHECK(stripped != 0, "strip");
        if (stripped != 0) {
            write_header(stripped, 1, 0, 0);
            stripped[15] = 1;
            write_file(mpak, stripped, slen);
            s = open_and_verify(mpak, &bag);
            CHECK(s == MUSICPACK_OK, "nonzero reserved tolerated");
            CHECK(bag_has(&bag, "reserved"), "reserved warning");
            CHECK(bag.count == 3, "reserved + missing-index/completeness only");
            free(stripped);
        }
    }

    free(buf);
}

static void
test_block_skipping(const char *tmp)
{
    char root[4200], mpak[4200];
    unsigned char *buf, *mod;
    size_t len, nlen;
    uint64_t bo, po, pl;
    musicpack_status s;
    msg_bag bag;

    join_path(root, sizeof root, tmp, "skip_pkg");
    join_path(mpak, sizeof mpak, tmp, "skip.mpak");
    CHECK(build_min_package(root, "audio/01.mpc", 0) == 0, "build");
    CHECK(pack_ok(root, mpak) == 0, "pack");
    buf = read_file_all(mpak, &len);
    CHECK(buf != 0, "read");
    if (buf == 0)
        return;

    /* Inserted blocks shift every later offset, which would stale INDX
       and TAIL; the skipping semantics are isolated by inserting into
       the stripped (no INDX/TAIL) form. The remaining warnings are the
       expected missing-index/completeness pair and nothing else. */
    {
        unsigned char *stripped;
        size_t slen;
        stripped = strip_indx_and_tail(buf, len, &slen);
        CHECK(stripped != 0, "strip");
        if (stripped != 0) {
            uint64_t sbo, spo, spl;
            CHECK(find_block(stripped, slen, "MANF", &sbo, &spo, &spl) == 0,
                  "find MANF in stripped");
            {
                unsigned char payload[5] = { 1, 2, 3, 4, 5 };
                hb h;
                memset(&h, 0, sizeof h);
                hb_block(&h, "XXXX", payload, sizeof payload);
                mod = splice_in(stripped, slen, (size_t) (spo + spl), h.buf,
                                h.len, &nlen);
                hb_free(&h);
                CHECK(mod != 0, "splice");
                write_file(mpak, mod, nlen);
                s = open_and_verify(mpak, &bag);
                CHECK(s == MUSICPACK_OK, "unknown uppercase block skipped");
                CHECK(bag.count == 2, "only missing-index/completeness");
                free(mod);
            }
            {
                unsigned char payload[3] = { 9, 9, 9 };
                hb h;
                memset(&h, 0, sizeof h);
                hb_block(&h, "xx1!", payload, sizeof payload);
                mod = splice_in(stripped, slen, (size_t) (spo + spl), h.buf,
                                h.len, &nlen);
                hb_free(&h);
                CHECK(mod != 0, "splice");
                write_file(mpak, mod, nlen);
                s = open_and_verify(mpak, &bag);
                CHECK(s == MUSICPACK_OK, "private block skipped");
                CHECK(bag.count == 2, "only missing-index/completeness");
                free(mod);
            }
            free(stripped);
        }
    }
    {
        uint64_t dbo, dpo, dpl;
        CHECK(find_block(buf, len, "MANF", &dbo, &dpo, &dpl) == 0,
              "find MANF (full)");
        bo = dbo;
        po = dpo;
        pl = dpl;
    }

    /* malformed length (u64 max) in the DATA block header */
    CHECK(find_block(buf, len, "DATA", &bo, &po, &pl) == 0, "find DATA");
    {
        int i;
        mod = splice_out(buf, len, 0, 0, &nlen); /* copy */
        free(mod);
        mod = (unsigned char *) malloc(len);
        memcpy(mod, buf, len);
        nlen = len;
        for (i = 0; i < 8; i++)
            mod[(size_t) bo + 4 + i] = 0xFF; /* length = 2^64-1 */
        write_file(mpak, mod, nlen);
        /* open survives (resync); the member is gone -> verify error */
        s = open_and_verify(mpak, &bag);
        CHECK(s == (musicpack_status) -100 || s != MUSICPACK_OK,
              "malformed length does not verify");
        CHECK(bag_has(&bag, "missing file") || s == (musicpack_status) -100,
              "missing member reported");
        free(mod);
    }

    /* INDX entry offset overflow: index discarded, scan used */
    {
        uint64_t io, ipo, ipl;
        CHECK(find_block(buf, len, "INDX", &io, &ipo, &ipl) == 0, "find INDX");
        mod = (unsigned char *) malloc(len);
        memcpy(mod, buf, len);
        /* first entry: 4 (count) + 2 (path_len) + path + 8 offset... */
        {
            uint64_t entry_off = ipo + 4 + 2 + strlen("audio/01.mpc");
            mod[(size_t) entry_off + 7] = 0xFF; /* offset near UINT64_MAX */
        }
        write_file(mpak, mod, len);
        s = open_and_verify(mpak, &bag);
        CHECK(s != MUSICPACK_OK, "overflowed index is an error (tail/size)");
        CHECK(bag_has(&bag, "corrupt INDX discarded"), "index discarded");
        free(mod);
    }

    free(buf);
}

static void
test_data_members(const char *tmp)
{
    char root[4200], mpak[4200], path[4200];
    unsigned char *orig, *got;
    size_t orig_len, got_len;
    musicpack_package *pkg;
    musicpack_status s;

    join_path(root, sizeof root, tmp, "data_pkg");
    join_path(mpak, sizeof mpak, tmp, "data.mpak");
    CHECK(build_min_package(root, "audio/01.mpc", "lyric/ab.txt") == 0,
          "build");
    CHECK(pack_ok(root, mpak) == 0, "pack");

    pkg = musicpack_package_open(mpak, &s);
    CHECK(pkg != 0, "open");
    if (pkg == 0)
        return;

    /* byte-exact member reads for every asset */
    orig = read_file_all(g_fixture_mpc, &orig_len);
    CHECK(orig != 0, "read fixture");
    CHECK(musicpack_package_read_member(pkg, "audio/01.mpc", 16u << 20, &got,
                                        &got_len) == MUSICPACK_OK,
          "read audio member");
    CHECK(got_len == orig_len && memcmp(got, orig, got_len) == 0,
          "audio member byte-exact");
    free(got);
    free(orig);
    CHECK(musicpack_package_read_member(pkg, "lyric/ab.txt", 1024, &got,
                                        &got_len) == MUSICPACK_OK,
          "read extras member");
    CHECK(got_len == 5 && memcmp(got, "note\n", 5) == 0, "extras byte-exact");
    free(got);

    /* missing member read fails cleanly */
    CHECK(musicpack_package_read_member(pkg, "audio/nope.mpc", 1024, &got,
                                        &got_len) == MUSICPACK_ERR_MISSING,
          "missing member read");

    musicpack_package_close(pkg);

    /* verify: clean */
    {
        msg_bag bag;
        s = open_and_verify(mpak, &bag);
        CHECK(s == MUSICPACK_OK, "verify clean");
        CHECK(bag.count == 0, "no warnings");
    }
    (void) path;
}

static void
test_manf_preservation(const char *tmp)
{
    char root[4200], mpak[4200], mpath[4200];
    unsigned char *buf, *mjson;
    size_t len, mlen;
    uint64_t bo, po, pl;
    musicpack_status s;
    msg_bag bag;

    join_path(root, sizeof root, tmp, "manf_pkg");
    join_path(mpak, sizeof mpak, tmp, "manf.mpak");
    CHECK(build_min_package(root, "audio/01.mpc", 0) == 0, "build");
    CHECK(pack_ok(root, mpak) == 0, "pack");

    join_path(mpath, sizeof mpath, root, "manifest.json");
    mjson = read_file_all(mpath, &mlen);
    buf = read_file_all(mpak, &len);
    CHECK(mjson != 0 && buf != 0, "read");
    if (mjson == 0 || buf == 0) {
        free(mjson);
        free(buf);
        return;
    }

    /* MANF payload is the exact manifest.json byte sequence */
    CHECK(find_block(buf, len, "MANF", &bo, &po, &pl) == 0, "find MANF");
    CHECK(pl == (uint64_t) mlen, "MANF length matches manifest.json");
    CHECK(memcmp(buf + po, mjson, mlen) == 0, "MANF bytes unchanged");

    /* a manifest referencing a missing member is an error */
    {
        unsigned char *mod = (unsigned char *) malloc(len);
        memcpy(mod, buf, len);
        /* corrupt the manifest JSON so the package cannot even open */
        mod[po] = '[';
        write_file(mpak, mod, len);
        CHECK(musicpack_package_open(mpak, &s) == 0,
              "malformed manifest cannot open");
        free(mod);
    }

    /* clean again: verify OK */
    write_file(mpak, buf, len);
    s = open_and_verify(mpak, &bag);
    CHECK(s == MUSICPACK_OK, "verify after restore");
    CHECK(bag.count == 0, "no warnings");
    (void) bo;

    free(mjson);
    free(buf);
}

static void
test_indx(const char *tmp)
{
    char root[4200], mpak[4200];
    unsigned char *buf;
    size_t len;
    uint64_t io, ipo, ipl;
    uint32_t count;
    musicpack_status s;
    msg_bag bag;

    join_path(root, sizeof root, tmp, "indx_pkg");
    join_path(mpak, sizeof mpak, tmp, "indx.mpak");
    CHECK(build_min_package(root, "audio/01.mpc", "lyric/ab.txt") == 0,
          "build");
    CHECK(pack_ok(root, mpak) == 0, "pack");
    buf = read_file_all(mpak, &len);
    CHECK(buf != 0, "read");
    if (buf == 0)
        return;
    CHECK(find_block(buf, len, "INDX", &io, &ipo, &ipl) == 0, "find INDX");

    /* entry count and strict lexicographic order */
    count = ((uint32_t) buf[ipo] << 24) | ((uint32_t) buf[ipo + 1] << 16)
          | ((uint32_t) buf[ipo + 2] << 8) | (uint32_t) buf[ipo + 3];
    CHECK(count == 2, "two index entries");

    /* walk entries: path/offset/length, offsets point at member bytes */
    {
        uint64_t p = ipo + 4;
        char prev[256];
        prev[0] = '\0';
        uint32_t i;
        for (i = 0; i < count; i++) {
            uint16_t plen = (uint16_t) ((buf[p] << 8) | buf[p + 1]);
            char path[256];
            uint64_t off, mlen;
            memcpy(path, buf + p + 2, plen);
            path[plen] = '\0';
            if (i > 0)
                CHECK(strcmp(prev, path) < 0, "lexicographic order");
            snprintf(prev, sizeof prev, "%s", path);
            p += 2 + plen;
            off = rd64(buf + p);
            mlen = rd64(buf + p + 8);
            p += 48;
            /* member bytes: verify offset+length against the DATA block */
            {
                uint64_t dbo, dpo, dpl;
                CHECK(find_block(buf, len, "DATA", &dbo, &dpo, &dpl) == 0,
                      "find DATA");
                (void) dbo;
                if (strcmp(path, "audio/01.mpc") == 0) {
                    CHECK(memcmp(buf + off, "MPCK", 4) == 0,
                          "offset points at member bytes (MPCK)");
                    CHECK(dpl == 2 + plen + mlen, "member length consistent");
                }
            }
        }
        CHECK(p == ipo + ipl, "index payload exactly consumed");
    }

    /* corrupt INDX payload (path bytes scrambled): discarded, scan used */
    {
        unsigned char *mod = (unsigned char *) malloc(len);
        memcpy(mod, buf, len);
        mod[ipo + 4 + 2] = 'z'; /* first entry path first byte */
        write_file(mpak, mod, len);
        s = open_and_verify(mpak, &bag);
        CHECK(s != MUSICPACK_OK, "scrambled index cannot verify (tail)");
        CHECK(bag_has(&bag, "corrupt INDX discarded"), "corrupt index warning");
        /* members still readable through the scan fallback */
        {
            musicpack_package *pkg = musicpack_package_open(mpak, 0);
            unsigned char *got = 0;
            size_t got_len = 0;
            CHECK(pkg != 0, "open with corrupt index");
            if (pkg != 0) {
                CHECK(musicpack_package_read_member(pkg, "audio/01.mpc",
                                                    16u << 20, &got, &got_len)
                          == MUSICPACK_OK, "scan fallback read");
                free(got);
                musicpack_package_close(pkg);
            }
        }
        free(mod);
    }

    /* restore and tamper an INDX hash byte: manifest/index mismatch */
    write_file(mpak, buf, len);
    {
        unsigned char *mod = (unsigned char *) malloc(len);
        uint64_t first_sha = ipo + 4 + 2 + strlen("audio/01.mpc") + 16;
        memcpy(mod, buf, len);
        mod[(size_t) first_sha] ^= 0xFF;
        write_file(mpak, mod, len);
        s = open_and_verify(mpak, &bag);
        CHECK(s != MUSICPACK_OK, "index hash mismatch is an error");
        CHECK(bag_has(&bag, "index: checksum mismatch"), "index hash message");
        free(mod);
    }

    /* duplicate INDX path: equal-length paths, entry0 renamed to entry1 */
    write_file(mpak, buf, len);
    {
        unsigned char *mod = (unsigned char *) malloc(len);
        uint64_t e0_path = ipo + 4 + 2;
        uint64_t e1_head = ipo + 4 + 2 + strlen("audio/01.mpc") + 48 + 2;
        memcpy(mod, buf, len);
        memcpy(mod + e0_path, mod + e1_head, strlen("lyric/ab.txt"));
        write_file(mpak, mod, len);
        s = open_and_verify(mpak, &bag);
        CHECK(s != MUSICPACK_OK, "duplicate INDX path is an error");
        CHECK(bag_has(&bag, "duplicate INDX path"), "duplicate index message");
        free(mod);
    }

    /* missing INDX (and TAIL removed, flag cleared): warning only */
    {
        size_t slen;
        unsigned char *m1 = strip_indx_and_tail(buf, len, &slen);
        CHECK(m1 != 0, "strip");
        if (m1 != 0) {
            write_file(mpak, m1, slen);
            s = open_and_verify(mpak, &bag);
            CHECK(s == MUSICPACK_OK, "missing INDX is not an error");
            CHECK(bag_has(&bag, "missing INDX"), "missing index warning");
            CHECK(bag_has(&bag, "completeness unproven"), "missing tail warning");
            CHECK(bag.count == 2, "exactly the two expected warnings");
            /* member still byte-exact */
            {
                musicpack_package *pkg = musicpack_package_open(mpak, 0);
                unsigned char *got = 0;
                size_t got_len = 0;
                CHECK(pkg != 0, "open without index");
                if (pkg != 0) {
                    CHECK(musicpack_package_read_member(pkg, "audio/01.mpc",
                                                        16u << 20, &got, &got_len)
                              == MUSICPACK_OK, "read without index");
                    free(got);
                    musicpack_package_close(pkg);
                }
            }
            free(m1);
        }
    }

    free(buf);
}

static void
test_tail(const char *tmp)
{
    char root[4200], mpak[4200];
    unsigned char *buf;
    size_t len;
    uint64_t to, tpo, tpl;
    musicpack_status s;
    msg_bag bag;

    join_path(root, sizeof root, tmp, "tail_pkg");
    join_path(mpak, sizeof mpak, tmp, "tail.mpak");
    CHECK(build_min_package(root, "audio/01.mpc", "lyric/ab.txt") == 0,
          "build");
    CHECK(pack_ok(root, mpak) == 0, "pack");
    buf = read_file_all(mpak, &len);
    CHECK(buf != 0, "read");
    if (buf == 0)
        return;
    CHECK(find_block(buf, len, "TAIL", &to, &tpo, &tpl) == 0, "find TAIL");

    /* fields */
    CHECK(rd64(buf + tpo) == (uint64_t) len, "total_file_size");
    {
        uint32_t objects = ((uint32_t) buf[tpo + 8] << 24)
                         | ((uint32_t) buf[tpo + 9] << 16)
                         | ((uint32_t) buf[tpo + 10] << 8)
                         | (uint32_t) buf[tpo + 11];
        CHECK(objects == 2, "object_count");
    }
    CHECK(rd64(buf + tpo + 12) == 16, "indx_offset points at INDX block");

    /* digest over all preceding bytes */
    {
        char hex[MUSICPACK_SHA256_HEX_SIZE];
        int i, match = 1;
        CHECK(musicpack_sha256_file_range(mpak, 0, to, hex, sizeof hex)
                  == MUSICPACK_OK, "hash range");
        CHECK(strlen(hex) == 64, "hex len");
        for (i = 0; i < 32; i++) {
            int v = (hex[i * 2] < 'a' ? hex[i * 2] - '0' : hex[i * 2] - 'a' + 10);
            int w = (hex[i * 2 + 1] < 'a' ? hex[i * 2 + 1] - '0'
                                          : hex[i * 2 + 1] - 'a' + 10);
            if ((unsigned char) ((v << 4) | w) != buf[tpo + 20 + i])
                match = 0;
        }
        CHECK(match, "TAIL digest matches SHA-256 of preceding bytes");
    }

    /* missing TAIL (truncated exactly at TAIL): warning only */
    write_file(mpak, buf, (size_t) to);
    s = open_and_verify(mpak, &bag);
    CHECK(s == MUSICPACK_OK, "missing TAIL is not an error");
    CHECK(bag_has(&bag, "completeness unproven"), "completeness warning");

    /* corrupted TAIL digest: error */
    write_file(mpak, buf, len);
    buf[tpo + 25] ^= 0xFF;
    write_file(mpak, buf, len);
    s = open_and_verify(mpak, &bag);
    CHECK(s != MUSICPACK_OK, "bad digest is an error");
    CHECK(bag_has(&bag, "package digest mismatch"), "digest message");

    /* corrupted TAIL total size: error */
    buf[tpo + 25] ^= 0xFF; /* restore */
    buf[tpo] ^= 0x01;
    write_file(mpak, buf, len);
    s = open_and_verify(mpak, &bag);
    CHECK(bag_has(&bag, "total size mismatch"), "total size message");
    buf[tpo] ^= 0x01; /* restore */

    /* truncated mid-DATA: open survives; the member before the cut stays
       byte-exact, the cut member is absent, and verify fails */
    {
        uint64_t dbo, dpo, dpl, cut;
        musicpack_package *pkg;
        CHECK(find_block(buf, len, "DATA", &dbo, &dpo, &dpl) == 0, "DATA");
        /* the second DATA block carries the lyric member */
        cut = dpo + dpl + MPAK_BHDR + 5;
        CHECK(cut < len, "second DATA block in bounds");
        CHECK(memcmp(buf + dpo + dpl, "DATA", 4) == 0,
              "second block is DATA");
        write_file(mpak, buf, (size_t) cut);
        pkg = musicpack_package_open(mpak, 0);
        CHECK(pkg != 0, "open truncated");
        if (pkg != 0) {
            unsigned char *got = 0;
            size_t got_len = 0;
            CHECK(musicpack_package_read_member(pkg, "audio/01.mpc",
                                                16u << 20, &got, &got_len)
                      == MUSICPACK_OK, "member before cut intact");
            CHECK(got != 0 && got_len > 0 && got[0] == 'M',
                  "intact member bytes");
            free(got);
            got = 0;
            CHECK(musicpack_package_read_member(pkg, "lyric/ab.txt", 1024,
                                                &got, &got_len)
                      == MUSICPACK_ERR_MISSING, "cut member missing");
            free(got);
            musicpack_package_close(pkg);
        }
        s = open_and_verify(mpak, &bag);
        CHECK(s != MUSICPACK_OK, "truncated package does not verify");
        CHECK(bag_has(&bag, "missing file"), "truncation reported");
    }


    free(buf);
}

static void
test_recovery(const char *tmp)
{
    char root[4200], mpak[4200];
    unsigned char *buf;
    size_t len;
    uint64_t bo, po, pl;
    musicpack_status s;
    msg_bag bag;

    join_path(root, sizeof root, tmp, "rec_pkg");
    join_path(mpak, sizeof mpak, tmp, "rec.mpak");
    CHECK(build_min_package(root, "audio/01.mpc", "lyric/ab.txt") == 0,
          "build");
    CHECK(pack_ok(root, mpak) == 0, "pack");
    buf = read_file_all(mpak, &len);
    CHECK(buf != 0, "read");
    if (buf == 0)
        return;

    /* trailing garbage after TAIL: the resynchronization scan runs over
       the garbage without crashing, emits a finding, and the framed
       members remain readable */
    {
        static const unsigned char junk[4096];
        FILE *f = fopen(mpak, "ab");
        CHECK(f != 0, "open for append");
        if (f != 0) {
            CHECK(fwrite(junk, 1, sizeof junk, f) == sizeof junk, "append");
            fclose(f);
        }
        s = open_and_verify(mpak, &bag);
        CHECK(s != MUSICPACK_OK, "garbage tail does not verify (tail)");
        CHECK(bag_has(&bag, "resynchronization"), "resync warning emitted");
        {
            musicpack_package *pkg = musicpack_package_open(mpak, 0);
            unsigned char *got = 0;
            size_t got_len = 0;
            CHECK(pkg != 0, "open with garbage tail");
            if (pkg != 0) {
                CHECK(musicpack_package_read_member(pkg, "lyric/ab.txt",
                                                    1024, &got, &got_len)
                          == MUSICPACK_OK, "member after garbage intact");
                CHECK(got != 0 && got_len == 5 &&
                      memcmp(got, "note\n", 5) == 0, "member bytes exact");
                free(got);
                musicpack_package_close(pkg);
            }
        }
        write_file(mpak, buf, len); /* restore for the cases below */
    }

    /* corrupted member bytes: localized checksum failure, other member OK */
    CHECK(find_block(buf, len, "DATA", &bo, &po, &pl) == 0, "DATA");
    {
        unsigned char *mod = (unsigned char *) malloc(len);
        memcpy(mod, buf, len);
        mod[(size_t) (po + 2 + strlen("audio/01.mpc"))] ^= 0xFF;
        write_file(mpak, mod, len);
        s = open_and_verify(mpak, &bag);
        CHECK(s != MUSICPACK_OK, "corrupted member fails");
        CHECK(bag_has(&bag, "checksum mismatch 'audio/01.mpc'"),
              "localized checksum message");
        {
            musicpack_package *pkg = musicpack_package_open(mpak, 0);
            unsigned char *got = 0;
            size_t got_len = 0;
            CHECK(pkg != 0, "open with corrupted member");
            if (pkg != 0) {
                CHECK(musicpack_package_read_member(pkg, "lyric/ab.txt", 1024,
                                                    &got, &got_len)
                          == MUSICPACK_OK, "other member intact");
                CHECK(got_len == 5, "other member size");
                free(got);
                musicpack_package_close(pkg);
            }
        }
        free(mod);
    }

    /* duplicate DATA path: error, first occurrence kept */
    write_file(mpak, buf, len);
    {
        size_t dstart = (size_t) bo;
        size_t dlen = (size_t) (MPAK_BHDR + pl);
        size_t nlen = 0;
        unsigned char *mod = splice_in(buf, len, dstart, buf + dstart, dlen,
                                       &nlen);
        CHECK(mod != 0, "splice duplicate DATA");
        write_file(mpak, mod, nlen);
        s = open_and_verify(mpak, &bag);
        CHECK(s != MUSICPACK_OK, "duplicate DATA path is an error");
        CHECK(bag_has(&bag, "duplicate object path"), "duplicate message");
        free(mod);
    }

    /* damaged block header: best-effort resynchronization finds later
       blocks; recovery never verifies semantics by itself */
    write_file(mpak, buf, len);
    buf[(size_t) bo + 13] ^= 0xFF; /* break DATA block header CRC */
    write_file(mpak, buf, len);
    {
        musicpack_package *pkg = musicpack_package_open(mpak, 0);
        CHECK(pkg != 0, "open with damaged header");
        if (pkg != 0) {
            unsigned char *got = 0;
            size_t got_len = 0;
            /* the lyric member lives after the damaged header; resync
               must have found it (best-effort, SHA verified at verify) */
            CHECK(musicpack_package_read_member(pkg, "lyric/ab.txt", 1024,
                                                &got, &got_len)
                      == MUSICPACK_OK, "resync recovered later member");
            free(got);
            musicpack_package_close(pkg);
        }
        s = open_and_verify(mpak, &bag);
        CHECK(bag_has(&bag, "resynchronization"), "resync warning");
    }

    /* corrupt MANF: package cannot open, but extraction recovers members */
    {
        char out_dir[4200], member_path[4200];
        unsigned char *extracted;
        size_t extracted_len;
        musicpack_report rep = { 0, 0 };
        CHECK(find_block(buf, len, "MANF", &bo, &po, &pl) == 0, "MANF");
        buf[(size_t) po] = '{'; /* break JSON immediately */
        buf[(size_t) po + 1] = '[';
        write_file(mpak, buf, len);
        CHECK(musicpack_package_open(mpak, 0) == 0,
              "corrupt manifest cannot open");
        join_path(out_dir, sizeof out_dir, tmp, "rec_out");
        /* physical extraction succeeds even though the package cannot be
           opened as a MusicPack (extractable-but-unverifiable) */
        CHECK(musicpack_mpak_unpack(mpak, out_dir, &rep, 0, 0) == MUSICPACK_OK,
              "physical extraction succeeds with damaged manifest");
        join_path(member_path, sizeof member_path, out_dir, "lyric/ab.txt");
        extracted = read_file_all(member_path, &extracted_len);
        CHECK(extracted != 0 && extracted_len == 5 &&
              memcmp(extracted, "note\n", 5) == 0,
              "extracted-but-unverifiable members recovered");
        free(extracted);
    }

    free(buf);
}

static void
test_indx_hint_lie(const char *tmp)
{
    char root[4200], mpak[4200];
    unsigned char *buf, *stripped;
    size_t len, slen;
    musicpack_status s;
    msg_bag bag;

    join_path(root, sizeof root, tmp, "lie_pkg");
    join_path(mpak, sizeof mpak, tmp, "lie.mpak");
    CHECK(build_min_package(root, "audio/01.mpc", "lyric/ab.txt") == 0,
          "build");
    CHECK(pack_ok(root, mpak) == 0, "pack");
    buf = read_file_all(mpak, &len);
    CHECK(buf != 0, "read");
    if (buf == 0)
        return;

    /* INDX_PRESENT is advisory: a set flag with no INDX block must stay
       warning-only and never gate reading */
    stripped = strip_indx_and_tail(buf, len, &slen);
    CHECK(stripped != 0, "strip");
    if (stripped != 0) {
        stripped[7] |= (unsigned char) 1u; /* force INDX_PRESENT on (lie) */
        write_file(mpak, stripped, slen);
        s = open_and_verify(mpak, &bag);
        CHECK(s == MUSICPACK_OK, "hint lie is not an error");
        CHECK(bag_has(&bag, "missing INDX"), "missing index warning");
        CHECK(bag_has(&bag, "completeness unproven"), "completeness warning");
        CHECK(bag.count == 2, "exactly the two expected warnings");
        {
            musicpack_package *pkg = musicpack_package_open(mpak, 0);
            unsigned char *got = 0;
            size_t got_len = 0;
            CHECK(pkg != 0, "open with lying hint");
            if (pkg != 0) {
                CHECK(musicpack_package_read_member(pkg, "audio/01.mpc",
                                                    16u << 20, &got, &got_len)
                          == MUSICPACK_OK, "scan fallback member read");
                free(got);
                musicpack_package_close(pkg);
            }
        }
        free(stripped);
    }
    free(buf);
}

static void
test_recovery_invalid_preamble(const char *tmp)
{
    char root[4200], mpak[4200], out[4200], member[4200];
    unsigned char *buf;
    size_t len;
    uint64_t bo, po, pl;
    musicpack_status s;
    msg_bag bag;
    musicpack_report rep = { 0, 0 };

    join_path(root, sizeof root, tmp, "pre_pkg");
    join_path(mpak, sizeof mpak, tmp, "pre.mpak");
    CHECK(build_min_package(root, "audio/01.mpc", "lyric/ab.txt") == 0,
          "build");
    CHECK(pack_ok(root, mpak) == 0, "pack");
    buf = read_file_all(mpak, &len);
    CHECK(buf != 0, "read");
    if (buf == 0)
        return;
    CHECK(find_block(buf, len, "DATA", &bo, &po, &pl) == 0, "first DATA");

    /* invalid path byte inside the first member's preamble; the block
       header CRC remains valid */
    buf[po + 2] = 0x01;
    write_file(mpak, buf, len);
    /* normal reader: hard error, unchanged */
    s = MUSICPACK_OK;
    CHECK(musicpack_package_open(mpak, &s) == 0, "normal open hard-fails");
    CHECK(s == MUSICPACK_ERR_PATH, "invalid preamble is a path error");

    /* recovery: damaged member skipped, later members still extracted */
    join_path(out, sizeof out, tmp, "pre_out");
    memset(&bag, 0, sizeof bag);
    s = musicpack_mpak_unpack(mpak, out, &rep, bag_report, &bag);
    CHECK(s == MUSICPACK_ERR_CHECKSUM, "recovery reports findings");
    CHECK(bag_has(&bag, "skipped"), "skip finding emitted");
    join_path(member, sizeof member, out, "lyric/ab.txt");
    {
        unsigned char *got = read_file_all(member, &len);
        CHECK(got != 0 && len == 5 && memcmp(got, "note\n", 5) == 0,
              "later member extracted byte-exact");
        free(got);
    }
    join_path(member, sizeof member, out, "audio/01.mpc");
    {
        unsigned char *got = read_file_all(member, &len);
        CHECK(got == 0, "damaged member not extracted");
        free(got);
    }
    join_path(member, sizeof member, out, "manifest.json");
    {
        unsigned char *got = read_file_all(member, &len);
        CHECK(got != 0 && len > 0, "manifest bytes recovered");
        free(got);
    }
    free(buf);
}

static void
test_extra_blocks(const char *tmp)
{
    char root[4200], mpak[4200];
    unsigned char *buf, *mod;
    size_t len, nlen;
    uint64_t io, ipo, ipl, to, tpo, tpl;
    musicpack_status s;
    msg_bag bag;

    join_path(root, sizeof root, tmp, "xtra_pkg");
    join_path(mpak, sizeof mpak, tmp, "xtra.mpak");
    CHECK(build_min_package(root, "audio/01.mpc", "lyric/ab.txt") == 0,
          "build");
    CHECK(pack_ok(root, mpak) == 0, "pack");
    buf = read_file_all(mpak, &len);
    CHECK(buf != 0, "read");
    if (buf == 0)
        return;
    CHECK(find_block(buf, len, "INDX", &io, &ipo, &ipl) == 0, "find INDX");
    CHECK(find_block(buf, len, "TAIL", &to, &tpo, &tpl) == 0, "find TAIL");

    /* append copies of the INDX and TAIL blocks after the TAIL: first
       block wins, extras warn (implementation-defined behavior) */
    {
        size_t indx_bytes = MPAK_BHDR + (size_t) ipl;
        size_t tail_bytes = MPAK_BHDR + 52; /* TAIL payload is fixed */
        mod = (unsigned char *) malloc(len + indx_bytes + tail_bytes);
        CHECK(mod != 0, "alloc");
        if (mod == 0) {
            free(buf);
            return;
        }
        memcpy(mod, buf, len);
        memcpy(mod + len, buf + (size_t) io, indx_bytes);
        memcpy(mod + len + indx_bytes, buf + (size_t) to, tail_bytes);
        nlen = len + indx_bytes + tail_bytes;
        write_file(mpak, mod, nlen);
        free(mod);
    }

    s = open_and_verify(mpak, &bag);
    CHECK(s != MUSICPACK_OK, "appended blocks stale the tail (expected)");
    CHECK(bag_has(&bag, "extra INDX block ignored"), "extra INDX warning");
    CHECK(bag_has(&bag, "extra TAIL block ignored"), "extra TAIL warning");
    {
        musicpack_package *pkg = musicpack_package_open(mpak, 0);
        unsigned char *got = 0;
        size_t got_len = 0;
        CHECK(pkg != 0, "open with extra blocks");
        if (pkg != 0) {
            CHECK(musicpack_package_read_member(pkg, "lyric/ab.txt", 1024,
                                                &got, &got_len)
                      == MUSICPACK_OK, "members still readable");
            CHECK(got != 0 && got_len == 5, "member bytes intact");
            free(got);
            musicpack_package_close(pkg);
        }
    }
    free(buf);
}

static void
test_path_rules(const char *tmp)
{
    char mpak[4200];
    hb h;
    unsigned char payload[64];
    musicpack_status s;
    musicpack_package *pkg;

    /* hand-built container with an unsafe DATA preamble path */
    join_path(mpak, sizeof mpak, tmp, "paths.mpak");
    memset(&h, 0, sizeof h);
    hb_header(&h, 1, 0, 0);
    {
        const char *bad = "../evil";
        size_t plen = strlen(bad);
        payload[0] = 0;
        payload[1] = (unsigned char) plen;
        memcpy(payload + 2, bad, plen);
        hb_block(&h, "DATA", payload, 2 + plen);
    }
    write_file(mpak, h.buf, h.len);
    hb_free(&h);

    pkg = musicpack_package_open(mpak, &s);
    CHECK(pkg == 0 && s == MUSICPACK_ERR_PATH, "unsafe DATA path rejected");
    if (pkg)
        musicpack_package_close(pkg);
}

static void
test_second_manf(const char *tmp)
{
    char root[4200], mpak[4200];
    unsigned char *buf, *mod;
    size_t len, nlen;
    uint64_t bo, po, pl;
    musicpack_status s;
    musicpack_package *pkg;

    join_path(root, sizeof root, tmp, "dupmanf_pkg");
    join_path(mpak, sizeof mpak, tmp, "dupmanf.mpak");
    CHECK(build_min_package(root, "audio/01.mpc", 0) == 0, "build");
    CHECK(pack_ok(root, mpak) == 0, "pack");
    buf = read_file_all(mpak, &len);
    CHECK(buf != 0, "read");
    if (buf == 0)
        return;
    CHECK(find_block(buf, len, "MANF", &bo, &po, &pl) == 0, "MANF");
    /* duplicate the MANF block bytes right after the original */
    mod = splice_in(buf, len, (size_t) (po + pl), buf + (size_t) bo,
                    (size_t) (MPAK_BHDR + pl), &nlen);
    CHECK(mod != 0, "splice");
    write_file(mpak, mod, nlen);
    pkg = musicpack_package_open(mpak, &s);
    CHECK(pkg == 0 && s == MUSICPACK_ERR_INVALID, "duplicate MANF rejected");
    if (pkg)
        musicpack_package_close(pkg);
    free(mod);
    free(buf);
}

static void
test_determinism(const char *tmp)
{
    char mpak_a[4200], mpak_b[4200];
    unsigned char *a, *b;
    size_t alen, blen;

    join_path(mpak_a, sizeof mpak_a, tmp, "det_a.mpak");
    join_path(mpak_b, sizeof mpak_b, tmp, "det_b.mpak");
    CHECK(pack_ok(g_ref_album, mpak_a) == 0, "pack a");
    CHECK(pack_ok(g_ref_album, mpak_b) == 0, "pack b");
    a = read_file_all(mpak_a, &alen);
    b = read_file_all(mpak_b, &blen);
    CHECK(a != 0 && b != 0, "read both");
    if (a != 0 && b != 0) {
        CHECK(alen == blen, "same length");
        CHECK(memcmp(a, b, alen) == 0, "byte-identical builds");
    }
    free(a);
    free(b);
}

static void
test_roundtrip_album(const char *tmp, const char *dir, const char *name)
{
    char mpak[4200], out[4200], path_a[4200], path_b[4200];
    unsigned char *ma, *mb, *fa, *fb;
    size_t malen, mblen, falen, fblen;
    musicpack_package *pkg;
    musicpack_report rep = { 0, 0 };
    asset_paths ap;
    size_t i;

    snprintf(mpak, sizeof mpak, "%s/rt_%s.mpak", tmp, name);
    snprintf(out, sizeof out, "%s/rt_%s_out", tmp, name);
    remove(mpak);
    {
        char stage[4300];
        snprintf(stage, sizeof stage, "%s.unpack-%ld", out, (long) getpid());
        remove(stage);
    }
    CHECK(pack_ok(dir, mpak) == 0, "pack album");

    /* verify the container */
    pkg = musicpack_package_open(mpak, 0);
    CHECK(pkg != 0, "open container");
    if (pkg != 0) {
        CHECK(musicpack_package_verify(pkg, &rep, 0, 0) == MUSICPACK_OK,
              "container verifies clean");
        collect_paths(musicpack_package_manifest(pkg), &ap);
        musicpack_package_close(pkg);
    } else {
        return;
    }

    /* unpack and compare */
    CHECK(musicpack_mpak_unpack(mpak, out, &rep, 0, 0) == MUSICPACK_OK,
          "unpack album");

    join_path(path_a, sizeof path_a, dir, "manifest.json");
    join_path(path_b, sizeof path_b, out, "manifest.json");
    ma = read_file_all(path_a, &malen);
    mb = read_file_all(path_b, &mblen);
    CHECK(ma != 0 && mb != 0, "read manifests");
    if (ma != 0 && mb != 0)
        CHECK(malen == mblen && memcmp(ma, mb, malen) == 0,
              "manifest bytes unchanged");
    free(ma);
    free(mb);

    for (i = 0; i < ap.count; i++) {
        join_path(path_a, sizeof path_a, dir, ap.paths[i]);
        join_path(path_b, sizeof path_b, out, ap.paths[i]);
        fa = read_file_all(path_a, &falen);
        fb = read_file_all(path_b, &fblen);
        CHECK(fa != 0 && fb != 0, "read member pair");
        if (fa != 0 && fb != 0)
            CHECK(falen == fblen && memcmp(fa, fb, falen) == 0,
                  "member bytes unchanged");
        free(fa);
        free(fb);
    }

    /* logical verification of the unpacked directory is equivalent */
    pkg = musicpack_package_open_dir(out, 0);
    CHECK(pkg != 0, "open unpacked dir");
    if (pkg != 0) {
        memset(&rep, 0, sizeof rep);
        CHECK(musicpack_package_verify(pkg, &rep, 0, 0) == MUSICPACK_OK,
              "unpacked directory verifies");
        musicpack_package_close(pkg);
    }
}

static void
test_mpc_integration(const char *tmp)
{
    char root[4200], mpak[4200];
    musicpack_package *pkg;
    musicpack_status s;
    mpc_reader reader;
    unsigned char *orig, *stream;
    size_t orig_len;

    join_path(root, sizeof root, tmp, "mpc_pkg");
    join_path(mpak, sizeof mpak, tmp, "mpc.mpak");
    CHECK(build_min_package(root, "audio/01.mpc", 0) == 0, "build");
    CHECK(pack_ok(root, mpak) == 0, "pack");

    orig = read_file_all(g_fixture_mpc, &orig_len);
    CHECK(orig != 0, "read fixture");
    if (orig == 0)
        return;

    pkg = musicpack_package_open(mpak, &s);
    CHECK(pkg != 0, "open container");
    if (pkg == 0) {
        free(orig);
        return;
    }

    /* read the complete member through the mpc_reader: the bytes reaching
       libmusepack are exactly the original SV8 stream */
    memset(&reader, 0, sizeof reader);
    CHECK(musicpack_package_track_open_reader(pkg, 0, 0, &reader)
              == MUSICPACK_OK, "open track reader");
    CHECK(reader.data != 0, "reader initialized");
    CHECK(reader.canseek(&reader) == MPC_TRUE, "container reader seekable");
    CHECK(reader.get_size(&reader) == (mpc_seek_t) orig_len,
          "reader size == member size");
    stream = (unsigned char *) malloc(orig_len);
    CHECK(stream != 0, "alloc");
    if (stream != 0) {
        mpc_int32_t n, total = 0;
        while ((n = reader.read(&reader, stream + total,
                                (mpc_int32_t) (orig_len - (size_t) total))) > 0)
            total += n;
        CHECK((size_t) total == orig_len, "read all bytes via mpc_reader");
        CHECK(memcmp(stream, orig, orig_len) == 0,
              "bytes reaching libmusepack are byte-exact");
        free(stream);
    }

    /* seek within the member works through the container reader */
    CHECK(reader.seek(&reader, (mpc_seek_t) (orig_len / 2)) == MPC_TRUE,
          "seek to member midpoint");
    CHECK(reader.tell(&reader) == (mpc_seek_t) (orig_len / 2), "tell");
    {
        unsigned char probe[4];
        mpc_int32_t n = reader.read(&reader, probe, 4);
        CHECK(n == 4 && memcmp(probe, orig + orig_len / 2, 4) == 0,
              "post-seek bytes match");
    }
    musicpack_package_track_close_reader(&reader);

    /* the existing Musepack decoder consumes the container-backed stream */
    CHECK(musicpack_package_track_open_reader(pkg, 0, 0, &reader)
              == MUSICPACK_OK, "reopen track reader");
    {
        musepack_decoder *dec = musepack_decoder_open(&reader, 0);
        float pcm[1152 * 2];
        uint64_t frames = 0, total = 0;
        CHECK(dec != 0, "decoder over container reader");
        if (dec != 0) {
            while (musepack_decoder_read(dec, pcm, 1152, &frames)
                       == MUSEPACK_OK)
                total += frames;
            CHECK(total > 0, "decoded frames via container reader");
            musepack_decoder_close(dec);
        }
    }
    musicpack_package_track_close_reader(&reader);

    musicpack_package_close(pkg);
    free(orig);
}

/* ------------------------------------------------------------------ */
/* range-source transport tests                                        */
/* ------------------------------------------------------------------ */

/* Recording fake source over an in-memory buffer: logs every requested
   range so tests can assert observable access behavior, and can be
   scripted to fail a chosen call. */
typedef struct fake_src {
    unsigned char *data;
    size_t size;         /* size reported to the container */
    size_t real_len;     /* actual bytes backing data (adapter truth) */
    uint64_t *log_off;
    size_t *log_len;
    size_t log_count;
    size_t log_cap;
    size_t calls;
    size_t fail_at;      /* 1-based call index to fail; 0 = never */
    uint64_t fail_off;   /* targeted failure: exact (offset, len) match */
    size_t fail_len;     /* 0 = disabled */
    int destroyed;
} fake_src;

static musicpack_status
fake_size(void *ctx, uint64_t *out)
{
    fake_src *f = (fake_src *) ctx;

    if (f == 0)
        return MUSICPACK_ERR_INVALID;
    *out = (uint64_t) f->size;
    return MUSICPACK_OK;
}

static musicpack_status
fake_read(void *ctx, uint64_t offset, unsigned char *buf, size_t len)
{
    fake_src *f = (fake_src *) ctx;

    f->calls++;
    if (f->log_count == f->log_cap) {
        size_t ncap = f->log_cap == 0 ? 64 : f->log_cap * 2;
        uint64_t *no = (uint64_t *) realloc(f->log_off, ncap * sizeof *no);
        size_t *nl = (size_t *) realloc(f->log_len, ncap * sizeof *nl);
        if (no != 0)
            f->log_off = no;
        if (nl != 0)
            f->log_len = nl;
        if (no == 0 || nl == 0)
            return MUSICPACK_ERR_NOMEM;
        f->log_cap = ncap;
    }
    f->log_off[f->log_count] = offset;
    f->log_len[f->log_count] = len;
    f->log_count++;
    if (f->fail_at != 0 && f->calls == f->fail_at)
        return MUSICPACK_ERR_IO;
    if (f->fail_len != 0 && offset == f->fail_off && len == f->fail_len)
        return MUSICPACK_ERR_IO;
    /* the adapter owns the bytes: it can never serve a range outside
       its real data, whatever size the container layer believes */
    if (offset > (uint64_t) f->real_len ||
        len > (uint64_t) f->real_len - offset)
        return MUSICPACK_ERR_IO;
    if (len > 0)
        memcpy(buf, f->data + offset, len);
    return MUSICPACK_OK;
}

static void
fake_destroy(void *ctx)
{
    fake_src *f = (fake_src *) ctx;

    if (f == 0)
        return;
    f->destroyed = 1;
    /* buffers intentionally kept: the test frees them and asserts the
       destroy/ownership contract through the flag */
}

static void
fake_free_buffers(fake_src *f)
{
    free(f->log_off);
    free(f->log_len);
    free(f->data);
}

static void
fake_init(fake_src *f, const unsigned char *data, size_t len)
{
    memset(f, 0, sizeof *f);
    f->data = (unsigned char *) malloc(len > 0 ? len : 1);
    if (data != 0 && len > 0)
        memcpy(f->data, data, len);
    f->size = len;
    f->real_len = len;
}

static const musicpack_range_source *
fake_vtable(void)
{
    static musicpack_range_source vt;

    if (vt.size == 0) {
        vt.size = fake_size;
        vt.read = fake_read;
        vt.destroy = fake_destroy;
    }
    return &vt;
}

static int
range_log_aligned(const fake_src *f, size_t *count)
{
    /* every logged range must be one 64 KiB block-aligned fetch
       (clamped only by the end of the object) */
    size_t i;

    *count = f->log_count;
    for (i = 0; i < f->log_count; i++) {
        if (f->log_off[i] % (64u * 1024u) != 0)
            return 0;
        if (f->log_len[i] == 0 ||
            f->log_len[i] > 64u * 1024u)
            return 0;
        if (f->log_off[i] + f->log_len[i] > f->size)
            return 0;
    }
    return 1;
}

static void
test_range_stdio_adapter(const char *tmp)
{
    char path[4200];
    musicpack_range_source src;
    unsigned char *pattern;
    unsigned char got[128];
    size_t i;
    const size_t plen = (200u * 1024u) + 37u;

    join_path(path, sizeof path, tmp, "adapter.bin");
    pattern = (unsigned char *) malloc(plen);
    CHECK(pattern != 0, "alloc pattern");
    if (pattern == 0)
        return;
    for (i = 0; i < plen; i++)
        pattern[i] = (unsigned char) (i * 31 + 7);

    /* deterministic file: exactly two 64 KiB-aligned spans + 37 bytes */
    CHECK(write_file(path, pattern, plen) == 0, "write pattern");

    /* nonexistent path and directory fail cleanly */
    CHECK(musicpack_range_source_stdio("/definitely/not/here.bin", &src)
              != MUSICPACK_OK, "missing file rejected");
    CHECK(musicpack_range_source_stdio(tmp, &src) != MUSICPACK_OK,
          "directory rejected");

    CHECK(musicpack_range_source_stdio(path, &src) == MUSICPACK_OK,
          "stdio adapter opens");
    if (src.ctx == 0)
        return;

    {
        uint64_t sz = 0;
        CHECK(src.size(src.ctx, &sz) == MUSICPACK_OK && sz == plen,
              "size reported");
    }
    /* exact reads, including one crossing the 64 KiB boundary */
    CHECK(src.read(src.ctx, 0, got, 10) == MUSICPACK_OK &&
          memcmp(got, pattern, 10) == 0, "read at 0");
    CHECK(src.read(src.ctx, 65530, got, 100) == MUSICPACK_OK &&
          memcmp(got, pattern + 65530, 100) == 0, "boundary-crossing read");
    CHECK(src.read(src.ctx, plen - 1, got, 1) == MUSICPACK_OK &&
          got[0] == pattern[plen - 1], "last byte");
    /* zero-length read succeeds without touching the transport */
    CHECK(src.read(src.ctx, 12345, got, 0) == MUSICPACK_OK, "len 0");
    /* out-of-bounds rejected */
    CHECK(src.read(src.ctx, plen, got, 1) != MUSICPACK_OK, "EOF read");
    CHECK(src.read(src.ctx, plen - 1, got, 2) != MUSICPACK_OK,
          "overrun read");
    CHECK(src.read(src.ctx, (uint64_t) -1, got, 1) != MUSICPACK_OK,
          "huge offset rejected");
    /* repeated reads are stable */
    CHECK(src.read(src.ctx, 65530, got, 100) == MUSICPACK_OK &&
          memcmp(got, pattern + 65530, 100) == 0, "repeated read");
    /* manual destroy lifecycle (no leak under ASan) */
    src.destroy(src.ctx);
    free(pattern);
}

static void
test_range_open_equivalence(const char *tmp)
{
    char root[4200], mpak[4200];
    musicpack_package *local_pkg, *range_pkg;
    musicpack_range_source src;
    musicpack_status s;
    char *json_local = 0, *json_range = 0;
    unsigned char *got_l = 0, *got_r = 0;
    size_t len_l = 0, len_r = 0;

    join_path(root, sizeof root, tmp, "req_pkg");
    join_path(mpak, sizeof mpak, tmp, "req.mpak");
    CHECK(build_min_package(root, "audio/01.mpc", "lyric/ab.txt") == 0,
          "build");
    CHECK(pack_ok(root, mpak) == 0, "pack");

    local_pkg = musicpack_package_open(mpak, &s);
    CHECK(local_pkg != 0, "local open");
    CHECK(musicpack_range_source_stdio(mpak, &src) == MUSICPACK_OK,
          "stdio source");
    range_pkg = musicpack_package_open_range(&src, &s);
    CHECK(range_pkg != 0, "range open");
    if (local_pkg == 0 || range_pkg == 0) {
        musicpack_package_close(local_pkg);
        musicpack_package_close(range_pkg);
        return;
    }

    /* verification behaves identically */
    CHECK(musicpack_package_verify(local_pkg, 0, 0, 0) == MUSICPACK_OK,
          "local verify");
    CHECK(musicpack_package_verify(range_pkg, 0, 0, 0) == MUSICPACK_OK,
          "range verify");

    /* manifest is identical */
    CHECK(musicpack_manifest_write(musicpack_package_manifest(local_pkg),
                                   &json_local) == MUSICPACK_OK,
          "local manifest serialize");
    CHECK(musicpack_manifest_write(musicpack_package_manifest(range_pkg),
                                   &json_range) == MUSICPACK_OK,
          "range manifest serialize");
    CHECK(json_local != 0 && json_range != 0 &&
          strcmp(json_local, json_range) == 0, "manifests identical");

    /* member reads byte-exact through both backends */
    CHECK(musicpack_package_read_member(local_pkg, "audio/01.mpc",
                                        16u << 20, &got_l, &len_l)
              == MUSICPACK_OK, "local member read");
    CHECK(musicpack_package_read_member(range_pkg, "audio/01.mpc",
                                        16u << 20, &got_r, &len_r)
              == MUSICPACK_OK, "range member read");
    CHECK(len_l == len_r && len_l > 0 && memcmp(got_l, got_r, len_l) == 0,
          "member bytes identical");
    free(got_l);
    free(got_r);
    got_l = got_r = 0;
    CHECK(musicpack_package_read_member(local_pkg, "lyric/ab.txt", 1024,
                                        &got_l, &len_l) == MUSICPACK_OK,
          "local extras read");
    CHECK(musicpack_package_read_member(range_pkg, "lyric/ab.txt", 1024,
                                        &got_r, &len_r) == MUSICPACK_OK,
          "range extras read");
    CHECK(len_l == len_r && memcmp(got_l, got_r, len_l) == 0,
          "extras bytes identical");
    free(got_l);
    free(got_r);

    musicpack_package_close(local_pkg);
    musicpack_package_close(range_pkg); /* adopts + destroys the source */
    free(json_local);
    free(json_range);
}

static void
test_range_request_pattern(const char *tmp)
{
    char root[4200], mpak[4200];
    unsigned char *packed;
    size_t packed_len;
    musicpack_package *pkg;
    musicpack_status s;
    fake_src fake;
    unsigned char *orig = 0, *got = 0;
    size_t orig_len = 0, blocks;
    mpc_reader reader;

    join_path(root, sizeof root, tmp, "pat_pkg");
    join_path(mpak, sizeof mpak, tmp, "pat.mpak");
    CHECK(build_min_package(root, "audio/01.mpc", 0) == 0, "build");
    CHECK(pack_ok(root, mpak) == 0, "pack");
    packed = read_file_all(mpak, &packed_len);
    CHECK(packed != 0, "read packed");
    if (packed == 0)
        return;
    orig = read_file_all(g_fixture_mpc, &orig_len);
    CHECK(orig != 0, "read fixture");

    fake_init(&fake, packed, packed_len);
    {   /* bind the shared vtable to this instance */
        musicpack_range_source local_vt = *fake_vtable();
        local_vt.ctx = &fake;
        pkg = musicpack_package_open_range(&local_vt, &s);
    }
    CHECK(pkg != 0, "range open with fake source");
    if (pkg == 0) {
        fake_free_buffers(&fake);
        free(packed);
        free(orig);
        return;
    }

    /* observable access behavior: every source fetch is exactly one
       64 KiB block-aligned read (clamped at EOF) */
    CHECK(range_log_aligned(&fake, &blocks), "block-aligned fetches");
    CHECK((size_t) blocks <= (packed_len + (64u * 1024u) - 1) / (64u * 1024u),
          "no redundant fetches during open");

    /* whole-member read through the track reader */
    memset(&reader, 0, sizeof reader);
    CHECK(musicpack_package_track_open_reader(pkg, 0, 0, &reader)
              == MUSICPACK_OK, "open member reader");
    got = (unsigned char *) malloc(orig_len);
    CHECK(got != 0, "alloc");
    if (got != 0) {
        mpc_int32_t n;
        size_t total = 0;
        while (total < orig_len &&
               (n = reader.read(&reader, got + total,
                                (mpc_int32_t) (orig_len - total))) > 0)
            total += (size_t) n;
        CHECK(total == orig_len, "full member via reader");
        CHECK(memcmp(got, orig, orig_len) == 0, "byte-exact over ranges");
    }
    free(got);

    /* the member is now cached: a second full pass causes no new
       source reads */
    {
        size_t calls_before = fake.calls;
        got = (unsigned char *) malloc(orig_len);
        CHECK(got != 0, "alloc 2");
        if (got != 0) {
            mpc_int32_t n;
            size_t total = 0;
            CHECK(reader.seek(&reader, 0) == MPC_TRUE, "rewind");
            while (total < orig_len &&
                   (n = reader.read(&reader, got + total,
                                    (mpc_int32_t) (orig_len - total))) > 0)
                total += (size_t) n;
            CHECK(total == orig_len, "second full read");
            CHECK(fake.calls == calls_before, "second read fully cached");
        }
        free(got);
    }
    musicpack_package_track_close_reader(&reader);
    musicpack_package_close(pkg);

    CHECK(fake.destroyed == 1, "adopted source destroyed at close");
    fake_free_buffers(&fake);
    free(packed);
    free(orig);
}

static void
test_range_seeking(const char *tmp)
{
    char root[4200], mpak[4200];
    musicpack_package *pkg;
    musicpack_status s;
    musicpack_range_source src;
    mpc_reader reader;
    unsigned char *orig;
    size_t orig_len;
    unsigned char probe[4];

    join_path(root, sizeof root, tmp, "seek_pkg");
    join_path(mpak, sizeof mpak, tmp, "seekr.mpak");
    CHECK(build_min_package(root, "audio/01.mpc", 0) == 0, "build");
    CHECK(pack_ok(root, mpak) == 0, "pack");
    orig = read_file_all(g_fixture_mpc, &orig_len);
    CHECK(orig != 0 && orig_len > 8, "fixture");
    if (orig == 0)
        return;

    CHECK(musicpack_range_source_stdio(mpak, &src) == MUSICPACK_OK,
          "stdio source");
    pkg = musicpack_package_open_range(&src, &s);
    CHECK(pkg != 0, "range open");
    if (pkg == 0) {
        free(orig);
        return;
    }
    memset(&reader, 0, sizeof reader);
    CHECK(musicpack_package_track_open_reader(pkg, 0, 0, &reader)
              == MUSICPACK_OK, "reader");
    CHECK(reader.canseek(&reader) == MPC_TRUE, "seekable");
    CHECK(reader.get_size(&reader) == (mpc_seek_t) orig_len, "size");
    /* seek does not require reading: jump straight to the middle */
    CHECK(reader.seek(&reader, (mpc_seek_t)(orig_len / 2)) == MPC_TRUE,
          "mid seek");
    CHECK(reader.tell(&reader) == (mpc_seek_t)(orig_len / 2), "tell");
    CHECK(reader.read(&reader, probe, 4) == 4 &&
          memcmp(probe, orig + orig_len / 2, 4) == 0, "post-seek bytes");
    /* boundary semantics: seek to size is legal, reads there give EOF */
    CHECK(reader.seek(&reader, (mpc_seek_t) orig_len) == MPC_TRUE,
          "seek to EOF");
    CHECK(reader.read(&reader, probe, 4) == 0, "read at EOF");
    CHECK(reader.seek(&reader, (mpc_seek_t) orig_len + 1) == MPC_FALSE,
          "seek past member rejected");
    /* member boundaries cannot be crossed */
    CHECK(reader.seek(&reader, 0) == MPC_TRUE, "rewind");
    CHECK(reader.read(&reader, probe, 4) == 4 &&
          memcmp(probe, orig, 4) == 0, "first bytes are the SV8 stream");
    musicpack_package_track_close_reader(&reader);
    musicpack_package_close(pkg);
    free(orig);
}

static void
test_range_mpc_decode(const char *tmp)
{
    char root[4200], mpak[4200];
    musicpack_package *pkg;
    musicpack_status s;
    musicpack_range_source src;
    mpc_reader reader;
    musepack_decoder *dec;
    float pcm[1152 * 2];
    uint64_t frames = 0, total = 0;

    join_path(root, sizeof root, tmp, "dec_pkg");
    join_path(mpak, sizeof mpak, tmp, "decr.mpak");
    CHECK(build_min_package(root, "audio/01.mpc", 0) == 0, "build");
    CHECK(pack_ok(root, mpak) == 0, "pack");
    CHECK(musicpack_range_source_stdio(mpak, &src) == MUSICPACK_OK,
          "stdio source");
    pkg = musicpack_package_open_range(&src, &s);
    CHECK(pkg != 0, "range open");
    if (pkg == 0)
        return;
    memset(&reader, 0, sizeof reader);
    CHECK(musicpack_package_track_open_reader(pkg, 0, 0, &reader)
              == MUSICPACK_OK, "reader");
    dec = musepack_decoder_open(&reader, 0);
    CHECK(dec != 0, "decoder over range-backed reader");
    if (dec != 0) {
        while (musepack_decoder_read(dec, pcm, 1152, &frames) == MUSEPACK_OK)
            total += frames;
        CHECK(total > 0, "decoded frames over HTTP-style transport");
        musepack_decoder_close(dec);
    }
    /* in-track seeking through the range-backed reader, using the
       decoder's own SV8 seek machinery (SO/ST inside the member) */
    CHECK(reader.seek(&reader, 0) == MPC_TRUE, "rewind for decoder seek");
    dec = musepack_decoder_open(&reader, 0);
    CHECK(dec != 0, "decoder for seek test");
    if (dec != 0) {
        total = 0;
        while (total < 1152 * 5 &&
               musepack_decoder_read(dec, pcm, 1152, &frames) == MUSEPACK_OK)
            total += frames;
        CHECK(total > 0, "initial decode");
        CHECK(musepack_decoder_seek_sample(dec, 22050) == MUSEPACK_OK,
              "decoder seek to mid-stream");
        total = 0;
        while (musepack_decoder_read(dec, pcm, 1152, &frames) == MUSEPACK_OK)
            total += frames;
        CHECK(total > 0, "decoded after mid-stream seek");
        musepack_decoder_close(dec);
    }
    musicpack_package_track_close_reader(&reader);
    musicpack_package_close(pkg);
}

static void
test_range_adversarial(const char *tmp)
{
    char root[4200], mpak[4200];
    unsigned char *packed;
    size_t packed_len;
    musicpack_package *pkg;
    musicpack_status s;
    musicpack_range_source local_vt;
    fake_src fake;
    unsigned char *got = 0;
    size_t got_len = 0;
    size_t orig_len = 0;

    char big[4200];
    join_path(root, sizeof root, tmp, "adv_pkg");
    join_path(mpak, sizeof mpak, tmp, "adv.mpak");
    /* pad the audio member past one 64 KiB block so open requires
       multiple source fetches (multi-block transport behavior) */
    join_path(big, sizeof big, tmp, "big.mpc");
    {
        unsigned char *fixture = read_file_all(g_fixture_mpc, &orig_len);
        unsigned char *padded;
        CHECK(fixture != 0, "fixture");
        if (fixture == 0)
            return;
        padded = (unsigned char *) calloc(1, orig_len + 48u * 1024u);
        CHECK(padded != 0, "alloc padded");
        if (padded == 0) {
            free(fixture);
            return;
        }
        memcpy(padded, fixture, orig_len); /* tail zeros: not decoded */
        CHECK(write_file(big, padded, orig_len + 48u * 1024u) == 0,
              "write padded member");
        free(padded);
        free(fixture);
    }
    CHECK(build_min_package_from(root, "audio/01.mpc", "lyric/ab.txt", big)
              == 0, "build");
    CHECK(pack_ok(root, mpak) == 0, "pack");
    packed = read_file_all(mpak, &packed_len);
    CHECK(packed != 0, "read");
    if (packed == 0)
        return;

    /* transport failure on the very first read: open fails, the source
       is NOT destroyed (caller ownership), and manual cleanup is safe */
    fake_init(&fake, packed, packed_len);
    fake.fail_at = 1;
    local_vt = *fake_vtable();
    local_vt.ctx = &fake;
    pkg = musicpack_package_open_range(&local_vt, &s);
    CHECK(pkg == 0 && s == MUSICPACK_ERR_IO, "first-read failure fails open");
    CHECK(fake.destroyed == 0, "failed open does not destroy source");
    fake_destroy(&fake);           /* caller-side cleanup */
    fake_free_buffers(&fake);

    /* transport failure mid-scan (the container spans multiple 64 KiB
       blocks): clean failure, no crash */
    CHECK(packed_len > 64u * 1024u, "container spans several blocks");
    fake_init(&fake, packed, packed_len);
    fake.fail_at = 2;
    local_vt = *fake_vtable();
    local_vt.ctx = &fake;
    pkg = musicpack_package_open_range(&local_vt, &s);
    CHECK(pkg == 0, "mid-scan failure fails open");
    CHECK(fake.destroyed == 0, "no destroy on failed open (2)");
    fake_destroy(&fake);
    fake_free_buffers(&fake);

    /* lying size (declared larger than the actual object): the
       adapter refuses reads beyond its real bytes, so the open fails
       cleanly — a size lie can never produce a working session */
    fake_init(&fake, packed, packed_len);
    fake.size = packed_len + (64u * 1024u);
    local_vt = *fake_vtable();
    local_vt.ctx = &fake;
    pkg = musicpack_package_open_range(&local_vt, &s);
    CHECK(pkg == 0, "size lie fails open");
    CHECK(fake.destroyed == 0, "no destroy on failed open (3)");
    fake_destroy(&fake);
    fake_free_buffers(&fake);

    /* hostile INDX entry (offset beyond the object) through a range
       transport: index discarded, scan fallback still serves members */
    {
        uint64_t io, ipo, ipl;
        unsigned char *mod;
        CHECK(find_block(packed, packed_len, "INDX", &io, &ipo, &ipl) == 0,
              "find INDX");
        mod = (unsigned char *) malloc(packed_len);
        memcpy(mod, packed, packed_len);
        {
            uint64_t e0 = ipo + 4 + 2 + strlen("audio/01.mpc");
            mod[(size_t) e0] = 0x7F; /* MSB set -> offset beyond object */
        }
        fake_init(&fake, mod, packed_len);
        local_vt = *fake_vtable();
        local_vt.ctx = &fake;
        pkg = musicpack_package_open_range(&local_vt, &s);
        CHECK(pkg != 0, "hostile index does not block open");
        if (pkg != 0) {
            msg_bag bag;
            memset(&bag, 0, sizeof bag);
            CHECK(musicpack_package_verify(pkg, 0, bag_report, &bag)
                      != MUSICPACK_OK, "hostile index fails verify");
            CHECK(bag_has(&bag, "corrupt INDX discarded"),
                  "index discarded via range transport");
            CHECK(musicpack_package_read_member(pkg, "lyric/ab.txt", 1024,
                                                &got, &got_len)
                      == MUSICPACK_OK, "scan fallback member read");
            free(got);
            got = 0;
            musicpack_package_close(pkg);
        }
        fake_destroy(&fake);
        fake_free_buffers(&fake);
        free(mod);
    }
    free(packed);
}

static void
test_range_degraded_containers(const char *tmp)
{
    char root[4200], mpak[4200];
    unsigned char *buf, *mod;
    size_t len, nlen;
    musicpack_status s;
    musicpack_range_source src;
    musicpack_package *pkg;
    msg_bag bag;

    join_path(root, sizeof root, tmp, "deg_pkg");
    join_path(mpak, sizeof mpak, tmp, "deg.mpak");
    CHECK(build_min_package(root, "audio/01.mpc", "lyric/ab.txt") == 0,
          "build");
    CHECK(pack_ok(root, mpak) == 0, "pack");
    buf = read_file_all(mpak, &len);
    CHECK(buf != 0, "read");
    if (buf == 0)
        return;

    /* INDX-less, hint-cleared, TAIL removed: readable via scan with
       warnings — identical to local semantics */
    mod = strip_indx_and_tail(buf, len, &nlen);
    CHECK(mod != 0, "strip");
    if (mod != 0) {
        write_file(mpak, mod, nlen);
        CHECK(musicpack_range_source_stdio(mpak, &src) == MUSICPACK_OK,
              "source");
        pkg = musicpack_package_open_range(&src, &s);
        CHECK(pkg != 0, "range open without index");
        if (pkg != 0) {
            unsigned char *got = 0;
            size_t got_len = 0;
            memset(&bag, 0, sizeof bag);
            CHECK(musicpack_package_verify(pkg, 0, bag_report, &bag)
                      == MUSICPACK_OK, "degraded verify warning-only");
            CHECK(bag_has(&bag, "missing INDX"), "missing index warning");
            CHECK(bag_has(&bag, "completeness unproven"),
                  "missing tail warning");
            CHECK(musicpack_package_read_member(pkg, "audio/01.mpc",
                                                16u << 20, &got, &got_len)
                      == MUSICPACK_OK, "member via scan fallback");
            free(got);
            musicpack_package_close(pkg);
        }
        free(mod);
    }

    /* truncated container through a range source: open survives,
       verify fails, size comes from the transport */
    {
        uint64_t dbo, dpo, dpl, cut;
        CHECK(find_block(buf, len, "DATA", &dbo, &dpo, &dpl) == 0, "DATA");
        cut = dpo + dpl + MPAK_BHDR + 5;
        CHECK(cut < len, "second block in bounds");
        write_file(mpak, buf, (size_t) cut);
        CHECK(musicpack_range_source_stdio(mpak, &src) == MUSICPACK_OK,
              "source");
        pkg = musicpack_package_open_range(&src, &s);
        CHECK(pkg != 0, "truncated range open");
        if (pkg != 0) {
            CHECK(musicpack_package_verify(pkg, 0, 0, 0) != MUSICPACK_OK,
                  "truncated does not verify");
            musicpack_package_close(pkg);
        }
    }
    free(buf);
}

static void
test_range_ownership_failure(const char *tmp)
{
    char root[4200], mpak[4200];
    unsigned char *packed;
    size_t packed_len;
    musicpack_package *pkg;
    musicpack_status s;
    musicpack_range_source local_vt;
    fake_src fake;

    join_path(root, sizeof root, tmp, "own_pkg");
    join_path(mpak, sizeof mpak, tmp, "own.mpak");
    CHECK(build_min_package(root, "audio/01.mpc", 0) == 0, "build");
    CHECK(pack_ok(root, mpak) == 0, "pack");
    packed = read_file_all(mpak, &packed_len);
    CHECK(packed != 0, "read");
    if (packed == 0)
        return;

    /* corrupt the MANF payload so the open reaches the package-build
       failure path (framing intact, JSON unparseable) */
    {
        uint64_t bo, po, pl;
        unsigned char *pristine;
        CHECK(find_block(packed, packed_len, "MANF", &bo, &po, &pl) == 0,
              "find MANF");
        pristine = (unsigned char *) malloc(packed_len);
        CHECK(pristine != 0, "alloc pristine");
        memcpy(pristine, packed, packed_len);
        packed[po] = '[';

        fake_init(&fake, packed, packed_len);
        local_vt = *fake_vtable();
        local_vt.ctx = &fake;
        pkg = musicpack_package_open_range(&local_vt, &s);
        CHECK(pkg == 0, "corrupt MANF fails open");
        CHECK(s == MUSICPACK_ERR_JSON, "build failure is a json error");
        /* D1 regression: on failure the package must NOT destroy the
           caller's source — the caller retains ownership */
        CHECK(fake.destroyed == 0, "destroy not called on failed open");

        /* caller-side cleanup required by the documented contract */
        fake_destroy(&fake);
        fake_free_buffers(&fake);

        /* success-path ownership companion over the pristine bytes */
        fake_init(&fake, pristine, packed_len);
        local_vt.ctx = &fake;
        pkg = musicpack_package_open_range(&local_vt, &s);
        CHECK(pkg != 0, "valid container opens");
        if (pkg != 0) {
            musicpack_package_close(pkg);
            CHECK(fake.destroyed == 1, "adopted source destroyed at close");
        }
        fake_free_buffers(&fake);
        free(pristine);
        free(packed);
        return;
    }

    fake_init(&fake, packed, packed_len);
    local_vt = *fake_vtable();
    local_vt.ctx = &fake;
    pkg = musicpack_package_open_range(&local_vt, &s);
    CHECK(pkg == 0, "corrupt MANF fails open");
    CHECK(s == MUSICPACK_ERR_JSON, "build failure is a json error");
    /* D1 regression: on failure the package must NOT destroy the
       caller's source — the caller retains ownership */
    CHECK(fake.destroyed == 0, "destroy not called on failed open");

    /* caller-side cleanup required by the documented contract */
    fake_destroy(&fake);
    fake_free_buffers(&fake);
    free(packed);
}

int
main(int argc, char **argv)
{
    char tmp[4200];

    if (argc < 4) {
        fprintf(stderr, "usage: mpak_tests <album.mpack> <flac.mpack> "
                        "<fixture.mpc>\n");
        return 2;
    }
    g_ref_album = argv[1];
    g_ref_flac = argv[2];
    g_fixture_mpc = argv[3];

    if (make_temp_dir(tmp, sizeof tmp) != 0) {
        fprintf(stderr, "cannot create temp dir\n");
        return 1;
    }

    test_format_basics(tmp);
    test_block_skipping(tmp);
    test_data_members(tmp);
    test_manf_preservation(tmp);
    test_indx(tmp);
    test_tail(tmp);
    test_recovery(tmp);
    test_recovery_invalid_preamble(tmp);
    test_indx_hint_lie(tmp);
    test_extra_blocks(tmp);
    test_path_rules(tmp);
    test_second_manf(tmp);
    test_determinism(tmp);
    test_roundtrip_album(tmp, g_ref_album, "mpc");
    test_roundtrip_album(tmp, g_ref_flac, "flac");
    test_mpc_integration(tmp);
    test_range_stdio_adapter(tmp);
    test_range_open_equivalence(tmp);
    test_range_request_pattern(tmp);
    test_range_seeking(tmp);
    test_range_mpc_decode(tmp);
    test_range_adversarial(tmp);
    test_range_ownership_failure(tmp);
    test_range_degraded_containers(tmp);

    if (failures) {
        fprintf(stderr, "%d mpak test(s) failed\n", failures);
        return 1;
    }
    printf("all mpak tests passed\n");
    return 0;
}
