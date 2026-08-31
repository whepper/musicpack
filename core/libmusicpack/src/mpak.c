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
/// \file mpak.c
/// MPAK v1 single-file container (specs/mpak-v1.md).
///
/// Layout: fixed 16-byte header, INDX (derived acceleration), MANF (exact
/// manifest.json bytes), DATA blocks (path preamble + byte-exact member),
/// optional TAIL (fixity/identity). INDX is never a prerequisite: the
/// sequential scan of DATA blocks is the source of truth for recovery.
///
/// All container fields are untrusted: lengths are validated against the
/// wire maximum and the actual file size before use, every offset+length
/// computation is overflow-checked, and no allocation is sized directly
/// by unchecked container values.
///
/// Recovery scanning (resynchronizing after a damaged block header by
/// searching for the next header whose CRC-16 validates) is best-effort
/// structural recovery only: CRC validation does not guarantee
/// synchronization, normal reading never depends on it, and recovered
/// members are still SHA-256 verified before being treated as intact.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
# include <direct.h>
# include <windows.h>
# include <sys/stat.h>
# include <sys/types.h>
#else
# include <fcntl.h>
# include <sys/stat.h>
# include <sys/types.h>
# include <unistd.h>
#endif

#include "internal.h"
#include "sha256_internal.h"
#include <musicpack/checksum.h>
#include <musicpack/mpak.h>
#include <musicpack/path.h>

#define MPAK_HEADER_SIZE 16u
#define MPAK_BLOCK_HEADER_SIZE 14u
#define MPAK_TAIL_PAYLOAD_SIZE 52u
/* Wire-level maximum block payload: reject before trusting the length. */
#define MPAK_MAX_BLOCK_LENGTH (UINT64_MAX >> 1)
#define MPAK_MAX_MEMBERS 4096u
#define MPAK_MAX_MEMBER_BYTES MUSICPACK_MANIFEST_MAX_FILE_SIZE
#define MPAK_MANIFEST_MAX (16u * 1024u * 1024u)
#define MPAK_MAX_TOTAL_BYTES MUSICPACK_MANIFEST_MAX_TOTAL_BYTES
/* Recovery resynchronization scans candidate headers inside a fixed-size
   read window instead of issuing one seek+read per candidate byte (a
   damaged region must not cost one syscall per byte scanned). The window
   is refilled at most once per MPak_SCAN_WINDOW bytes of forward
   scanning. */
#define MPAK_SCAN_WINDOW (64u * 1024u)

static const unsigned char MPAK_MAGIC[4] = { 0x4D, 0x50, 0x41, 0x4B }; /* "MPAK" */

/* ------------------------------------------------------------------ */
/* byte helpers                                                        */
/* ------------------------------------------------------------------ */

static uint16_t
rd_u16(const unsigned char *p)
{
    return (uint16_t) (((uint16_t) p[0] << 8) | (uint16_t) p[1]);
}

static uint32_t
rd_u32(const unsigned char *p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16)
         | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

static uint64_t
rd_u64(const unsigned char *p)
{
    return ((uint64_t) rd_u32(p) << 32) | rd_u32(p + 4);
}

static void
wr_u16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char) (v >> 8);
    p[1] = (unsigned char) v;
}

static void
wr_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char) (v >> 24);
    p[1] = (unsigned char) (v >> 16);
    p[2] = (unsigned char) (v >> 8);
    p[3] = (unsigned char) v;
}

static void
wr_u64(unsigned char *p, uint64_t v)
{
    wr_u32(p, (uint32_t) (v >> 32));
    wr_u32(p + 4, (uint32_t) v);
}

static int
u64_add(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a > UINT64_MAX - b)
        return 0;
    *out = a + b;
    return 1;
}

static int
seek_absolute(FILE *f, uint64_t off)
{
#ifdef _WIN32
    return _fseeki64(f, (__int64) off, SEEK_SET) == 0 ? 0 : -1;
#else
    return fseeko(f, (off_t) off, SEEK_SET) == 0 ? 0 : -1;
#endif
}

/* CRC-16/BUYPASS: poly 0x8005, init 0xFFFF, no reflection, xorout 0.
   Table-driven form: crc = (crc << 8) ^ crc16_table[(crc >> 8) ^ byte].
   The table is the exact table-driven equivalent of the bitwise routine
   (verified by exhaustive comparison); the recovery scan evaluates a
   candidate header at every byte position of a damaged region, so the
   per-byte cost matters. */
static const uint16_t crc16_table[256] = {
    0x0000, 0x8005, 0x800F, 0x000A, 0x801B, 0x001E, 0x0014, 0x8011,
    0x8033, 0x0036, 0x003C, 0x8039, 0x0028, 0x802D, 0x8027, 0x0022,
    0x8063, 0x0066, 0x006C, 0x8069, 0x0078, 0x807D, 0x8077, 0x0072,
    0x0050, 0x8055, 0x805F, 0x005A, 0x804B, 0x004E, 0x0044, 0x8041,
    0x80C3, 0x00C6, 0x00CC, 0x80C9, 0x00D8, 0x80DD, 0x80D7, 0x00D2,
    0x00F0, 0x80F5, 0x80FF, 0x00FA, 0x80EB, 0x00EE, 0x00E4, 0x80E1,
    0x00A0, 0x80A5, 0x80AF, 0x00AA, 0x80BB, 0x00BE, 0x00B4, 0x80B1,
    0x8093, 0x0096, 0x009C, 0x8099, 0x0088, 0x808D, 0x8087, 0x0082,
    0x8183, 0x0186, 0x018C, 0x8189, 0x0198, 0x819D, 0x8197, 0x0192,
    0x01B0, 0x81B5, 0x81BF, 0x01BA, 0x81AB, 0x01AE, 0x01A4, 0x81A1,
    0x01E0, 0x81E5, 0x81EF, 0x01EA, 0x81FB, 0x01FE, 0x01F4, 0x81F1,
    0x81D3, 0x01D6, 0x01DC, 0x81D9, 0x01C8, 0x81CD, 0x81C7, 0x01C2,
    0x0140, 0x8145, 0x814F, 0x014A, 0x815B, 0x015E, 0x0154, 0x8151,
    0x8173, 0x0176, 0x017C, 0x8179, 0x0168, 0x816D, 0x8167, 0x0162,
    0x8123, 0x0126, 0x012C, 0x8129, 0x0138, 0x813D, 0x8137, 0x0132,
    0x0110, 0x8115, 0x811F, 0x011A, 0x810B, 0x010E, 0x0104, 0x8101,
    0x8303, 0x0306, 0x030C, 0x8309, 0x0318, 0x831D, 0x8317, 0x0312,
    0x0330, 0x8335, 0x833F, 0x033A, 0x832B, 0x032E, 0x0324, 0x8321,
    0x0360, 0x8365, 0x836F, 0x036A, 0x837B, 0x037E, 0x0374, 0x8371,
    0x8353, 0x0356, 0x035C, 0x8359, 0x0348, 0x834D, 0x8347, 0x0342,
    0x03C0, 0x83C5, 0x83CF, 0x03CA, 0x83DB, 0x03DE, 0x03D4, 0x83D1,
    0x83F3, 0x03F6, 0x03FC, 0x83F9, 0x03E8, 0x83ED, 0x83E7, 0x03E2,
    0x83A3, 0x03A6, 0x03AC, 0x83A9, 0x03B8, 0x83BD, 0x83B7, 0x03B2,
    0x0390, 0x8395, 0x839F, 0x039A, 0x838B, 0x038E, 0x0384, 0x8381,
    0x0280, 0x8285, 0x828F, 0x028A, 0x829B, 0x029E, 0x0294, 0x8291,
    0x82B3, 0x02B6, 0x02BC, 0x82B9, 0x02A8, 0x82AD, 0x82A7, 0x02A2,
    0x82E3, 0x02E6, 0x02EC, 0x82E9, 0x02F8, 0x82FD, 0x82F7, 0x02F2,
    0x02D0, 0x82D5, 0x82DF, 0x02DA, 0x82CB, 0x02CE, 0x02C4, 0x82C1,
    0x8243, 0x0246, 0x024C, 0x8249, 0x0258, 0x825D, 0x8257, 0x0252,
    0x0270, 0x8275, 0x827F, 0x027A, 0x826B, 0x026E, 0x0264, 0x8261,
    0x0220, 0x8225, 0x822F, 0x022A, 0x823B, 0x023E, 0x0234, 0x8231,
    0x8213, 0x0216, 0x021C, 0x8219, 0x0208, 0x820D, 0x8207, 0x0202,
};

static uint16_t
mpak_crc16(const unsigned char *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    size_t i;

    for (i = 0; i < len; i++)
        crc = (uint16_t) ((crc << 8) ^
                         crc16_table[((crc >> 8) ^ data[i]) & 0xFF]);
    return crc;
}

static void
mpak_write_block_header(unsigned char hdr[MPAK_BLOCK_HEADER_SIZE],
                        const char type[4], uint64_t length)
{
    memcpy(hdr, type, 4);
    wr_u64(hdr + 4, length);
    wr_u16(hdr + 12, mpak_crc16(hdr, 12));
}

/* Returns 1 when the 14 bytes at hdr form a plausible block header whose
   payload [pos + 14, pos + 14 + length) lies inside file_size. The cheap
   length/bounds conditions are checked before the CRC is computed; every
   condition must pass for acceptance, so the evaluation order cannot
   change the outcome — it only avoids needless CRC work while the
   recovery scan walks through damaged regions. A header is never acted
   upon (its length never trusted) unless its CRC-16 validates. */
static int
mpak_check_block_header(const unsigned char hdr[MPAK_BLOCK_HEADER_SIZE],
                        uint64_t pos, uint64_t file_size, uint64_t *length_out)
{
    uint64_t length = rd_u64(hdr + 4), end;

    if (length > MPAK_MAX_BLOCK_LENGTH)
        return 0;
    if (!u64_add(pos, MPAK_BLOCK_HEADER_SIZE, &end))
        return 0;
    if (!u64_add(end, length, &end))
        return 0;
    if (end > file_size)
        return 0;
    if (rd_u16(hdr + 12) != mpak_crc16(hdr, 12))
        return 0;
    *length_out = length;
    return 1;
}

static int
hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int
parse_sha256_hex(const char *hex, unsigned char out[32])
{
    int i;

    if (hex == 0 || strlen(hex) != 64)
        return 0;
    for (i = 0; i < 32; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return 0;
        out[i] = (unsigned char) ((hi << 4) | lo);
    }
    return 1;
}

static void
sha_bytes_to_hex(const unsigned char sha[32], char hex[MUSICPACK_SHA256_HEX_SIZE])
{
    static const char hexc[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 32; i++) {
        hex[i * 2] = hexc[sha[i] >> 4];
        hex[i * 2 + 1] = hexc[sha[i] & 0xF];
    }
    hex[64] = '\0';
}

/* ------------------------------------------------------------------ */
/* container I/O                                                       */
/* ------------------------------------------------------------------ */

/* Internal container-source abstraction: every byte the scanner, the
   member I/O vtable and the member reader need is an exact read at an
   absolute offset. Two backends implement it: a local FILE* (the
   hardened-open path used by .mpak files) and a caller-provided
   musicpack_range_source (the remote transport seam; the block cache
   lives here so all readers share it). */
typedef struct mpak_cio {
    void *ctx;
    /* Exact read: fills buf with len bytes at offset. len may be 0
       (succeeds without I/O). Returns 1 on success, 0 on any failure
       (short read, out-of-bounds range, transport error). Callers only
       read within [0, file_size). */
    int (*read_at)(void *ctx, uint64_t offset, unsigned char *buf,
                   size_t len);
    void (*close)(void *ctx);   /* releases backend resources */
} mpak_cio;

static int
cio_read(const mpak_cio *cio, uint64_t offset, unsigned char *buf, size_t len)
{
    if (len == 0)
        return 1;
    return cio->read_at(cio->ctx, offset, buf, len);
}

/* ---- FILE* backend ------------------------------------------------- */

typedef struct mpak_file_ctx {
    FILE *f;
    uint64_t size;
} mpak_file_ctx;

static int
file_read_at(void *ctx, uint64_t offset, unsigned char *buf, size_t len)
{
    mpak_file_ctx *c = (mpak_file_ctx *) ctx;

    if (len == 0)
        return 1;
    if (offset > c->size || len > c->size - offset)
        return 0;
    if (seek_absolute(c->f, offset) != 0)
        return 0;
    return fread(buf, 1, len, c->f) == len;
}

static void
file_cio_close(void *ctx)
{
    mpak_file_ctx *c = (mpak_file_ctx *) ctx;

    if (c == 0)
        return;
    if (c->f != 0)
        fclose(c->f);
    free(c);
}

/* Opens path with the hardened regular-file checks and fills cio.
   Returns MUSICPACK_OK and the file size, or an error (cio close is
   then the caller's no-op/owned as usual). */
static musicpack_status
mpak_open_file_cio(const char *path, mpak_cio *cio, uint64_t *size_out)
{
    mpak_file_ctx *c;
    musicpack_status s = MUSICPACK_OK;

    c = (mpak_file_ctx *) calloc(1, sizeof *c);
    if (c == 0)
        return MUSICPACK_ERR_NOMEM;
    c->f = musicpack_open_regular_read(path);
    if (c->f == 0) {
        free(c);
        return MUSICPACK_ERR_IO;
    }
    {
#ifdef _WIN32
        struct _stat st;
        int fd = _fileno(c->f);
        if (fd < 0 || _fstat(fd, &st) != 0 || (st.st_mode & _S_IFREG) == 0) {
#else
        int fd = fileno(c->f);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
#endif
            file_cio_close(c);
            return MUSICPACK_ERR_IO;
        }
        c->size = (uint64_t) st.st_size;
    }
    cio->ctx = c;
    cio->read_at = file_read_at;
    cio->close = file_cio_close;
    *size_out = c->size;
    return s;
}

/* ---- range-source backend (with the shared block cache) ------------ */

/* Cache parameters follow specs/mpak-http-range-design.md §8 (the
   parameters proven by demo/networker.js): 64 KiB block-aligned fetches,
   16 blocks (~1 MiB), insertion-order eviction, no threads, no disk
   cache, no speculative prefetching. The cache lives at the container
   layer so the scanner, the member vtable and the member reader share
   it; transport adapters stay free of container caching policy. */
#define MPAK_CACHE_BLOCK (64u * 1024u)
#define MPAK_CACHE_SLOTS 16u

typedef struct mpak_cache_slot {
    unsigned char *data;      /* MPAK_CACHE_BLOCK bytes */
    uint64_t base;            /* block base offset; UINT64_MAX = empty */
} mpak_cache_slot;

typedef struct mpak_range_ctx {
    musicpack_range_source src;   /* adopted on successful open */
    int adopted;                  /* 1 once destroy is the package's job */
    uint64_t size;
    mpak_cache_slot slots[MPAK_CACHE_SLOTS];
    unsigned next_victim;
} mpak_range_ctx;

static int
range_fetch_block(mpak_range_ctx *r, mpak_cache_slot *slot, uint64_t base)
{
    uint64_t block_len = r->size - base;   /* caller guarantees base < size */

    if (block_len > MPAK_CACHE_BLOCK)
        block_len = MPAK_CACHE_BLOCK;
    if (slot->data == 0) {
        slot->data = (unsigned char *) malloc(MPAK_CACHE_BLOCK);
        if (slot->data == 0)
            return 0;
    }
    slot->base = base;
    return r->src.read(r->src.ctx, base, slot->data, (size_t) block_len)
               == MUSICPACK_OK;
}

static int
range_read_at(void *ctx, uint64_t offset, unsigned char *buf, size_t len)
{
    mpak_range_ctx *r = (mpak_range_ctx *) ctx;

    if (len == 0)
        return 1;
    if (offset > r->size || len > r->size - offset)
        return 0;
    while (len > 0) {
        uint64_t base = offset & ~((uint64_t) MPAK_CACHE_BLOCK - 1);
        size_t in_off = (size_t) (offset - base);
        size_t avail = (size_t) (r->size - base);
        size_t take = MPAK_CACHE_BLOCK - in_off;
        mpak_cache_slot *slot = 0;
        size_t i;
        int found = 0;

        if (take > len)
            take = len;
        if (take > avail - in_off)
            take = avail - in_off;
        for (i = 0; i < MPAK_CACHE_SLOTS; i++) {
            if (r->slots[i].data != 0 && r->slots[i].base == base) {
                slot = &r->slots[i];
                found = 1;
                break;
            }
        }
        if (!found) {
            slot = &r->slots[r->next_victim++ % MPAK_CACHE_SLOTS];
            if (!range_fetch_block(r, slot, base))
                return 0;
        }
        memcpy(buf, slot->data + in_off, take);
        buf += take;
        offset += take;
        len -= take;
    }
    return 1;
}

static void
range_cio_close(void *ctx)
{
    mpak_range_ctx *r = (mpak_range_ctx *) ctx;
    size_t i;

    if (r == 0)
        return;
    /* destroy is the package's job only once the open has adopted the
       source; a failed open leaves caller ownership intact */
    if (r->adopted && r->src.destroy != 0)
        r->src.destroy(r->src.ctx);
    for (i = 0; i < MPAK_CACHE_SLOTS; i++)
        free(r->slots[i].data);
    free(r);
}

/* Builds a container I/O over a caller-provided range source. The
   source is NOT adopted here (a failed load must leave caller
   ownership intact); the caller flips `adopted` on success. */
static musicpack_status
mpak_open_range_cio(const musicpack_range_source *src, mpak_cio *cio,
                    uint64_t *size_out)
{
    mpak_range_ctx *r;
    uint64_t size = 0;

    if (src == 0 || src->size == 0 || src->read == 0)
        return MUSICPACK_ERR_INVALID;
    r = (mpak_range_ctx *) calloc(1, sizeof *r);
    if (r == 0)
        return MUSICPACK_ERR_NOMEM;
    r->src = *src;
    {
        mpak_cache_slot empty = { 0, UINT64_MAX };
        size_t i;
        for (i = 0; i < MPAK_CACHE_SLOTS; i++)
            r->slots[i] = empty;
    }
    if (src->size(src->ctx, &size) != MUSICPACK_OK) {
        range_cio_close(r);
        return MUSICPACK_ERR_IO;
    }
    r->size = size;
    cio->ctx = r;
    cio->read_at = range_read_at;
    cio->close = range_cio_close;
    *size_out = size;
    return MUSICPACK_OK;
}

/* Incremental SHA-256 over a container range, through the container
   I/O (so remote containers hash exactly the bytes they were opened
   with). */
static musicpack_status
mpak_hash_range(const mpak_cio *cio, uint64_t offset, uint64_t length,
                char *hex, size_t cap)
{
    musicpack_sha256_ctx c;
    unsigned char digest[32];
    unsigned char buf[MUSICPACK_IO_CHUNK];
    static const char hexc[] = "0123456789abcdef";
    uint64_t remaining = length;
    uint64_t pos = offset;
    size_t i;

    if (hex == 0 || cap < MUSICPACK_SHA256_HEX_SIZE)
        return MUSICPACK_ERR_INVALID;
    musicpack_sha256_init(&c);
    while (remaining > 0) {
        size_t want = remaining > sizeof buf ? sizeof buf
                                             : (size_t) remaining;
        if (!cio_read(cio, pos, buf, want))
            return MUSICPACK_ERR_IO;
        musicpack_sha256_update(&c, buf, want);
        pos += want;
        remaining -= want;
    }
    musicpack_sha256_final(&c, digest);
    for (i = 0; i < 32; i++) {
        hex[i * 2] = hexc[digest[i] >> 4];
        hex[i * 2 + 1] = hexc[digest[i] & 0xF];
    }
    hex[64] = '\0';
    return MUSICPACK_OK;
}

/* ------------------------------------------------------------------ */
/* container state                                                     */
/* ------------------------------------------------------------------ */

typedef struct mpak_member {
    char *path;               /* canonical package-relative path */
    uint64_t offset;          /* absolute offset of member bytes */
    uint64_t length;          /* member byte count */
    unsigned char sha256[32]; /* INDX entries only (scan: zeroed) */
} mpak_member;

typedef struct musicpack_mpak {
    mpak_cio cio;             /* container byte source (owned) */
    uint64_t file_size;
    unsigned major, minor;
    unsigned flags;
    unsigned reserved_nonzero;

    /* sequential scan results (DATA order) */
    mpak_member *scan;
    size_t scan_count;
    size_t duplicate_members;
    char *duplicate_example;
    size_t skipped_members;   /* recovery mode: invalid-path DATA blocks */
    int resynced;
    int recovery;             /* 1 = physical recovery (unpack) policy */

    /* MANF */
    unsigned char *manifest;
    size_t manifest_len;
    unsigned manifest_count;

    /* INDX */
    int indx_present;
    int indx_extra;
    int indx_valid;           /* parsed, self-consistent and reconciled */
    int indx_duplicate;
    uint64_t indx_offset;     /* block offset */
    uint64_t indx_length;     /* block payload length */
    mpak_member *indx;
    size_t indx_count;

    /* TAIL */
    int tail_present;
    int tail_extra;
    int tail_malformed;
    uint64_t tail_offset;     /* block offset */
    uint64_t tail_total_size;
    uint64_t tail_indx_offset;
    uint32_t tail_objects;
    unsigned char tail_digest[32];
} musicpack_mpak;

static void
mpak_free(musicpack_mpak *mf)
{
    size_t i;

    if (mf == 0)
        return;
    if (mf->cio.close != 0)
        mf->cio.close(mf->cio.ctx);
    for (i = 0; i < mf->scan_count; i++)
        free(mf->scan[i].path);
    free(mf->scan);
    for (i = 0; i < mf->indx_count; i++)
        free(mf->indx[i].path);
    free(mf->indx);
    free(mf->manifest);
    free(mf->duplicate_example);
    free(mf);
}

void
musicpack_mpak_io_free(void *io_ctx)
{
    mpak_free((musicpack_mpak *) io_ctx);
}

/* ------------------------------------------------------------------ */
/* sequential scan (with best-effort resynchronization)                */
/* ------------------------------------------------------------------ */

/* Recovery (unpack) policy: a DATA block whose framing is valid and
   whose length gives a safe block boundary, but whose preamble is
   structurally invalid, is skipped and counted; the damaged member is
   never attributed to a path. Normal loading keeps every preamble
   failure a hard error. */
#define MPAK_RECOVERY_SKIP(mf, code)                                         \
    do {                                                                    \
        if ((mf)->recovery) {                                               \
            (mf)->skipped_members++;                                        \
            return MUSICPACK_OK;                                            \
        }                                                                   \
        return (code);                                                      \
    } while (0)

static musicpack_status
scan_data_payload(musicpack_mpak *mf, uint64_t payload_pos, uint64_t length)
{
    unsigned char pbuf[2];
    unsigned char path[MUSICPACK_PATH_MAX + 1];
    uint16_t path_len;
    uint64_t member_offset, member_length;
    mpak_member *slot;
    size_t i;

    if (length < 2)
        MPAK_RECOVERY_SKIP(mf, MUSICPACK_ERR_INVALID);
    if (!cio_read(&mf->cio, payload_pos, pbuf, 2))
        return MUSICPACK_ERR_IO;
    path_len = rd_u16(pbuf);
    if (path_len == 0 || path_len > MUSICPACK_PATH_MAX)
        MPAK_RECOVERY_SKIP(mf, MUSICPACK_ERR_PATH);
    if ((uint64_t) path_len + 2u > length)
        MPAK_RECOVERY_SKIP(mf, MUSICPACK_ERR_INVALID);
    if (!cio_read(&mf->cio, payload_pos + 2, path, path_len))
        return MUSICPACK_ERR_IO;
    path[path_len] = '\0';
    if (musicpack_path_validate((const char *) path) != MUSICPACK_OK)
        MPAK_RECOVERY_SKIP(mf, MUSICPACK_ERR_PATH);

    if (!u64_add(payload_pos, 2u + (uint64_t) path_len, &member_offset))
        return MUSICPACK_ERR_INVALID;
    member_length = length - 2u - (uint64_t) path_len;
    if (member_length > MPAK_MAX_MEMBER_BYTES)
        MPAK_RECOVERY_SKIP(mf, MUSICPACK_ERR_INVALID);

    /* the 4096-member budget is a physical policy limit in both modes */
    if (mf->scan_count >= MPAK_MAX_MEMBERS)
        return MUSICPACK_ERR_INVALID;
    for (i = 0; i < mf->scan_count; i++) {
        if (strcmp(mf->scan[i].path, (const char *) path) == 0) {
            mf->duplicate_members++;
            if (mf->duplicate_example == 0)
                mf->duplicate_example = strdup((const char *) path);
            return MUSICPACK_OK; /* keep the first occurrence */
        }
    }

    slot = &mf->scan[mf->scan_count];
    slot->path = strdup((const char *) path);
    if (slot->path == 0)
        return MUSICPACK_ERR_NOMEM;
    slot->offset = member_offset;
    slot->length = member_length;
    mf->scan_count++;
    return MUSICPACK_OK;
}

/* Parses the INDX payload at mf->indx_offset; validates entries
   structurally (bounds, canonical paths, strict lexicographic order,
   exact payload consumption). */
static musicpack_status
parse_indx(musicpack_mpak *mf)
{
    unsigned char buf[8];
    unsigned char path[MUSICPACK_PATH_MAX + 1];
    uint64_t pos = mf->indx_offset + MPAK_BLOCK_HEADER_SIZE;
    uint64_t consumed;
    uint32_t count, i;
    mpak_member *entries;

    if (mf->indx_length < 4 || mf->indx_length > MPAK_MANIFEST_MAX * 2u)
        return MUSICPACK_ERR_INVALID;
    if (!cio_read(&mf->cio, pos, buf, 4))
        return MUSICPACK_ERR_IO;
    count = rd_u32(buf);
    if (count > MPAK_MAX_MEMBERS)
        return MUSICPACK_ERR_INVALID;
    pos += 4;

    entries = (mpak_member *) calloc(count > 0 ? count : 1, sizeof *entries);
    if (entries == 0)
        return MUSICPACK_ERR_NOMEM;

    for (i = 0; i < count; i++) {
        uint16_t path_len;
        uint64_t offset, length, end;

        if (!cio_read(&mf->cio, pos, buf, 2))
            goto malformed;
        path_len = rd_u16(buf);
        if (path_len == 0 || path_len > MUSICPACK_PATH_MAX)
            goto malformed;
        if (!cio_read(&mf->cio, pos + 2, path, path_len))
            goto malformed;
        path[path_len] = '\0';
        if (musicpack_path_validate((const char *) path) != MUSICPACK_OK)
            goto malformed;
        if (i > 0 && strcmp(entries[i - 1].path, (const char *) path) >= 0) {
            mf->indx_duplicate = 1;
            goto malformed;
        }
        if (!cio_read(&mf->cio, pos + 2 + path_len, buf, 8))
            goto malformed;
        offset = rd_u64(buf);
        if (!cio_read(&mf->cio, pos + 2 + path_len + 8, buf, 8))
            goto malformed;
        length = rd_u64(buf);
        if (length > MPAK_MAX_MEMBER_BYTES)
            goto malformed;
        if (!u64_add(offset, length, &end) || end > mf->file_size)
            goto malformed;
        if (!cio_read(&mf->cio, pos + 2 + path_len + 16, entries[i].sha256,
                      32))
            goto malformed;
        entries[i].path = strdup((const char *) path);
        if (entries[i].path == 0) {
            size_t j;
            for (j = 0; j < i; j++)
                free(entries[j].path);
            free(entries);
            return MUSICPACK_ERR_NOMEM;
        }
        entries[i].offset = offset;
        entries[i].length = length;
        pos += 2u + path_len + 48u;
    }
    consumed = pos - (mf->indx_offset + MPAK_BLOCK_HEADER_SIZE);
    if (consumed != mf->indx_length)
        goto malformed;

    mf->indx = entries;
    mf->indx_count = count;
    return MUSICPACK_OK;

malformed:
    {
        size_t j;
        for (j = 0; j < count; j++)
            free(entries[j].path); /* zeroed by calloc for j >= i */
        free(entries);
    }
    return MUSICPACK_ERR_INVALID;
}

static int
member_cmp(const void *a, const void *b)
{
    const mpak_member *ma = (const mpak_member *) a;
    const mpak_member *mb = (const mpak_member *) b;
    return strcmp(ma->path, mb->path);
}

/* Reconciles the parsed INDX against the scan: the sorted path sets and
   every offset/length must agree exactly, otherwise the index is
   discarded (scan fallback). Guarantees that INDX-based lookups can never
   point anywhere the block stream does not. */
static void
reconcile_indx(musicpack_mpak *mf)
{
    mpak_member *sorted;
    size_t i;

    if (!mf->indx_valid)
        return;
    if (mf->indx_count != mf->scan_count) {
        mf->indx_valid = 0;
        return;
    }
    sorted = (mpak_member *) malloc((mf->scan_count > 0 ? mf->scan_count : 1)
                                    * sizeof *sorted);
    if (sorted == 0) {
        mf->indx_valid = 0;
        return;
    }
    memcpy(sorted, mf->scan, mf->scan_count * sizeof *sorted);
    qsort(sorted, mf->scan_count, sizeof *sorted, member_cmp);
    for (i = 0; i < mf->scan_count; i++) {
        if (strcmp(sorted[i].path, mf->indx[i].path) != 0 ||
            sorted[i].offset != mf->indx[i].offset ||
            sorted[i].length != mf->indx[i].length) {
            mf->indx_valid = 0;
            break;
        }
    }
    free(sorted);
}

static musicpack_status
mpak_load_cio(uint64_t file_size, int need_manifest, int recovery,
              const mpak_cio *cio, musicpack_mpak **out)
{
    musicpack_mpak *mf;
    unsigned char hdr[MPAK_HEADER_SIZE];
    unsigned char *win = 0;
    uint64_t win_base = 0, win_len = 0;
    uint64_t pos;
    musicpack_status s = MUSICPACK_OK;

    *out = 0;
    mf = (musicpack_mpak *) calloc(1, sizeof *mf);
    if (mf == 0)
        return MUSICPACK_ERR_NOMEM;
    mf->recovery = recovery;
    mf->cio = *cio;
    mf->file_size = file_size;
    if (mf->file_size < MPAK_HEADER_SIZE) {
        mpak_free(mf);
        return MUSICPACK_ERR_INVALID;
    }
    if (!cio_read(&mf->cio, 0, hdr, MPAK_HEADER_SIZE)) {
        mpak_free(mf);
        return MUSICPACK_ERR_IO;
    }
    if (memcmp(hdr, MPAK_MAGIC, 4) != 0) {
        mpak_free(mf);
        return MUSICPACK_ERR_INVALID;
    }
    mf->major = hdr[4];
    mf->minor = hdr[5];
    if (mf->major != MUSICPACK_MPAK_MAJOR) {
        mpak_free(mf);
        return MUSICPACK_ERR_VERSION;
    }
    mf->flags = rd_u16(hdr + 6);
    if (rd_u64(hdr + 8) != 0)
        mf->reserved_nonzero = 1; /* tolerated: warning at verify */

    mf->scan = (mpak_member *) calloc(MPAK_MAX_MEMBERS, sizeof *mf->scan);
    if (mf->scan == 0) {
        mpak_free(mf);
        return MUSICPACK_ERR_NOMEM;
    }
    win = (unsigned char *) malloc(MPAK_SCAN_WINDOW);
    if (win == 0) {
        mpak_free(mf);
        return MUSICPACK_ERR_NOMEM;
    }

    /* Sequential block scan from byte 16. Candidate headers are validated
       inside a fixed-size buffered window: on a malformed header the scan
       advances one byte and retries, but the window is refilled at most
       once per MPAK_SCAN_WINDOW bytes of forward scanning, never once per
       candidate byte. CRC-16 must validate before any header field is
       trusted; it remains a resynchronization confidence mechanism only,
       not an integrity guarantee. Unknown (uppercase) and private types
       are skipped by their declared length. */
    pos = MPAK_HEADER_SIZE;
    while (pos + MPAK_BLOCK_HEADER_SIZE <= mf->file_size) {
        unsigned char type[4];
        uint64_t length = 0, payload_pos, next;
        const unsigned char *bhdr;

        payload_pos = pos + MPAK_BLOCK_HEADER_SIZE;
        if (pos < win_base ||
            payload_pos > win_base + win_len) {
            win_base = pos;
            win_len = mf->file_size - pos;
            if (win_len > MPAK_SCAN_WINDOW)
                win_len = MPAK_SCAN_WINDOW;
            if (!cio_read(&mf->cio, win_base, win, (size_t) win_len)) {
                s = MUSICPACK_ERR_IO;
                goto fail;
            }
        }
        bhdr = win + (pos - win_base);
        if (!mpak_check_block_header(bhdr, pos, mf->file_size, &length)) {
            pos++; /* resync: best-effort, not a guarantee */
            mf->resynced = 1;
            continue;
        }
        memcpy(type, bhdr, 4);

        if (memcmp(type, "DATA", 4) == 0) {
            s = scan_data_payload(mf, payload_pos, length);
            if (s != MUSICPACK_OK)
                goto fail;
        } else if (memcmp(type, "MANF", 4) == 0) {
            if (length > MPAK_MANIFEST_MAX) {
                s = MUSICPACK_ERR_INVALID;
                goto fail;
            }
            if (mf->manifest_count == 0) {
                mf->manifest = (unsigned char *) malloc(length > 0
                                                    ? (size_t) length : 1);
                if (mf->manifest == 0) {
                    s = MUSICPACK_ERR_NOMEM;
                    goto fail;
                }
                if (!cio_read(&mf->cio, payload_pos, mf->manifest,
                              (size_t) length)) {
                    s = MUSICPACK_ERR_IO;
                    goto fail;
                }
                mf->manifest_len = (size_t) length;
            }
            mf->manifest_count++;
        } else if (memcmp(type, "INDX", 4) == 0) {
            if (!mf->indx_present) {
                mf->indx_present = 1;
                mf->indx_offset = pos;
                mf->indx_length = length;
            } else {
                mf->indx_extra = 1;
            }
        } else if (memcmp(type, "TAIL", 4) == 0) {
            if (!mf->tail_present) {
                mf->tail_present = 1;
                mf->tail_offset = pos;
                if (length != MPAK_TAIL_PAYLOAD_SIZE) {
                    mf->tail_malformed = 1;
                } else {
                    unsigned char tbuf[MPAK_TAIL_PAYLOAD_SIZE];
                    if (!cio_read(&mf->cio, payload_pos, tbuf,
                                  sizeof tbuf)) {
                        s = MUSICPACK_ERR_IO;
                        goto fail;
                    }
                    mf->tail_total_size = rd_u64(tbuf);
                    mf->tail_objects = rd_u32(tbuf + 8);
                    mf->tail_indx_offset = rd_u64(tbuf + 12);
                    memcpy(mf->tail_digest, tbuf + 20, 32);
                }
            } else {
                mf->tail_extra = 1;
            }
        }

        if (!u64_add(payload_pos, length, &next)) {
            s = MUSICPACK_ERR_INVALID;
            goto fail;
        }
        pos = next;
    }

    if (need_manifest) {
        if (mf->manifest_count == 0) {
            s = MUSICPACK_ERR_MISSING;
            goto fail;
        }
        if (mf->manifest_count > 1) {
            s = MUSICPACK_ERR_INVALID; /* exactly one MANF (spec §15/D3) */
            goto fail;
        }
    }

    if (mf->indx_present) {
        s = parse_indx(mf);
        if (s == MUSICPACK_OK) {
            mf->indx_valid = 1;
            reconcile_indx(mf);
            s = MUSICPACK_OK;
        } else if (s == MUSICPACK_ERR_NOMEM) {
            goto fail;
        } else {
            mf->indx_valid = 0; /* index is optional: scan fallback */
        }
    }

    free(win);
    *out = mf;
    return MUSICPACK_OK;

fail:
    free(win);
    mpak_free(mf);
    return s;
}

static musicpack_status
mpak_load(const char *file, int need_manifest, int recovery,
          musicpack_mpak **out)
{
    mpak_cio cio;
    uint64_t size;
    musicpack_status s = mpak_open_file_cio(file, &cio, &size);

    if (s != MUSICPACK_OK)
        return s;
    /* ownership of the file cio passes to the loaded container (its
       close runs from mpak_free on both success and failure paths) */
    return mpak_load_cio(size, need_manifest, recovery, &cio, out);
}

/* ------------------------------------------------------------------ */
/* member lookup + package-handle backend                              */
/* ------------------------------------------------------------------ */

static const mpak_member *
find_member(const musicpack_mpak *mf, const char *rel)
{
    size_t i;

    if (mf->indx_valid) {
        size_t lo = 0, hi = mf->indx_count;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            int c = strcmp(mf->indx[mid].path, rel);
            if (c == 0)
                return &mf->indx[mid];
            if (c < 0)
                lo = mid + 1;
            else
                hi = mid;
        }
        return 0;
    }
    for (i = 0; i < mf->scan_count; i++)
        if (strcmp(mf->scan[i].path, rel) == 0)
            return &mf->scan[i];
    return 0;
}

static musicpack_status
io_size(void *ctx, const char *rel, long long *out)
{
    const musicpack_mpak *mf = (const musicpack_mpak *) ctx;
    const mpak_member *m = find_member(mf, rel);

    if (m == 0)
        return MUSICPACK_ERR_MISSING;
    *out = (long long) m->length;
    return MUSICPACK_OK;
}

static musicpack_status
io_sha256(void *ctx, const char *rel, char *hex, size_t cap)
{
    const musicpack_mpak *mf = (const musicpack_mpak *) ctx;
    const mpak_member *m = find_member(mf, rel);

    if (m == 0)
        return MUSICPACK_ERR_MISSING;
    return mpak_hash_range(&mf->cio, m->offset, m->length, hex, cap);
}

static musicpack_status
io_read(void *ctx, const char *rel, size_t max, unsigned char **out,
        size_t *len)
{
    musicpack_mpak *mf = (musicpack_mpak *) ctx;
    const mpak_member *m = find_member(mf, rel);
    unsigned char *buf;

    if (m == 0)
        return MUSICPACK_ERR_MISSING;
    if (m->length > (uint64_t) max)
        return MUSICPACK_ERR_IO;
    buf = (unsigned char *) malloc(m->length > 0 ? (size_t) m->length : 1);
    if (buf == 0)
        return MUSICPACK_ERR_NOMEM;
    if (m->length > 0) {
        if (!cio_read(&mf->cio, m->offset, buf, (size_t) m->length)) {
            free(buf);
            return MUSICPACK_ERR_IO;
        }
    }
    *out = buf;
    *len = (size_t) m->length;
    return MUSICPACK_OK;
}

static musicpack_status
io_list(void *ctx, char ***paths, size_t *count)
{
    musicpack_mpak *mf = (musicpack_mpak *) ctx;
    char **list;
    size_t i;

    list = (char **) calloc(mf->scan_count > 0 ? mf->scan_count : 1,
                            sizeof *list);
    if (list == 0)
        return MUSICPACK_ERR_NOMEM;
    for (i = 0; i < mf->scan_count; i++) {
        list[i] = strdup(mf->scan[i].path);
        if (list[i] == 0) {
            size_t j;
            for (j = 0; j < i; j++)
                free(list[j]);
            free(list);
            return MUSICPACK_ERR_NOMEM;
        }
    }
    *paths = list;
    *count = mf->scan_count;
    return MUSICPACK_OK;
}

static const musicpack_member_io mpak_io = {
    0, io_size, io_sha256, io_read, io_list
};

static musicpack_package *
mpak_build_package(musicpack_mpak *mf, const char *root_label,
                   musicpack_status *status)
{
    musicpack_package *pkg;
    cJSON *root;

    /* Parse the manifest exactly like the directory backend. */
    if (memchr(mf->manifest, '\0', mf->manifest_len) != 0) {
        mpak_free(mf);
        *status = MUSICPACK_ERR_JSON;
        return 0;
    }
    {
        char *zbuf = (char *) malloc(mf->manifest_len + 1);
        if (zbuf == 0) {
            mpak_free(mf);
            *status = MUSICPACK_ERR_NOMEM;
            return 0;
        }
        memcpy(zbuf, mf->manifest, mf->manifest_len);
        zbuf[mf->manifest_len] = '\0';
        root = cJSON_ParseWithLengthOpts(zbuf, mf->manifest_len + 1, 0, 1);
        free(zbuf);
    }
    if (root == 0) {
        mpak_free(mf);
        *status = MUSICPACK_ERR_JSON;
        return 0;
    }

    pkg = (musicpack_package *) calloc(1, sizeof *pkg);
    if (pkg == 0) {
        cJSON_Delete(root);
        mpak_free(mf);
        *status = MUSICPACK_ERR_NOMEM;
        return 0;
    }
    pkg->manifest = (musicpack_manifest *) calloc(1, sizeof *pkg->manifest);
    if (pkg->manifest == 0) {
        free(pkg);
        cJSON_Delete(root);
        mpak_free(mf);
        *status = MUSICPACK_ERR_NOMEM;
        return 0;
    }
    *status = musicpack_manifest_parse_tree(root, pkg->manifest);
    if (*status != MUSICPACK_OK) {
        musicpack_manifest_free(pkg->manifest);
        free(pkg);
        cJSON_Delete(root);
        mpak_free(mf);
        return 0;
    }
    pkg->original = root;
    pkg->root = strdup(root_label);
    if (pkg->root == 0) {
        musicpack_manifest_free(pkg->manifest);
        cJSON_Delete(pkg->original);
        free(pkg);
        mpak_free(mf);
        *status = MUSICPACK_ERR_NOMEM;
        return 0;
    }
    pkg->io = &mpak_io;
    pkg->io_ctx = mf;
    return pkg;
}

musicpack_package *
musicpack_mpak_open_package(const char *file, musicpack_status *status)
{
    musicpack_status local = MUSICPACK_OK, s;
    musicpack_mpak *mf;
    musicpack_package *pkg;

    if (status == 0)
        status = &local;
    *status = MUSICPACK_OK;
    if (file == 0) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    s = mpak_load(file, 1, 0, &mf);
    if (s != MUSICPACK_OK) {
        *status = s;
        return 0;
    }
    pkg = mpak_build_package(mf, file, status);
    if (pkg == 0)
        return 0;
    return pkg;
}

musicpack_package *
musicpack_package_open_range(const musicpack_range_source *src,
                             musicpack_status *status)
{
    musicpack_status local = MUSICPACK_OK, s;
    mpak_cio cio;
    uint64_t size;
    musicpack_mpak *mf;
    musicpack_package *pkg;

    if (status == 0)
        status = &local;
    *status = MUSICPACK_OK;
    if (src == 0) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    /* Strict normal-reader semantics: range-backed opening uses the
       same hardened scanner and never the recovery policy. */
    s = mpak_open_range_cio(src, &cio, &size);
    if (s != MUSICPACK_OK) {
        *status = s;
        return 0;
    }
    s = mpak_load_cio(size, 1, 0, &cio, &mf);
    if (s != MUSICPACK_OK) {
        /* the failed load released the wrapper and cache but did NOT
           destroy the caller's source: ownership stays with the caller */
        *status = s;
        return 0;
    }
    pkg = mpak_build_package(mf, "(range-source)", status);
    if (pkg == 0)
        return 0; /* released the container; source ownership NOT taken */
    {   /* fully built: the package adopts the source (destroy at close) */
        mpak_range_ctx *r = (mpak_range_ctx *) cio.ctx;
        r->adopted = 1;
    }
    return pkg;
}

/* ------------------------------------------------------------------ */
/* container-level verification                                        */
/* ------------------------------------------------------------------ */

static void
mpak_report(musicpack_report *rep, musicpack_report_fn fn, void *ctx,
            const char *message, int is_error)
{
    if (is_error)
        rep->errors++;
    else
        rep->warnings++;
    if (fn != 0)
        fn(ctx, message, is_error);
}

typedef struct mpak_asset_ref {
    const char *path;
    const char *sha256;
} mpak_asset_ref;

/* Collects every manifest-referenced asset (path + hex hash). */
static musicpack_status
collect_manifest_assets(const musicpack_manifest *m, mpak_asset_ref **out,
                        size_t *count)
{
    size_t cap = 0, n = 0, d, t, r, i;
    mpak_asset_ref *refs;

    for (d = 0; d < m->disc_count; d++) {
        cap += m->discs[d].track_count;
        for (t = 0; t < m->discs[d].track_count; t++) {
            cap += m->discs[d].tracks[t].representation_count;
            if (m->discs[d].tracks[t].waveform.present)
                cap++;
        }
    }
    cap += m->artwork_count + m->booklet_count + m->lyrics_count
         + m->extras_count + m->analysis_count;
    refs = (mpak_asset_ref *) calloc(cap > 0 ? cap : 1, sizeof *refs);
    if (refs == 0)
        return MUSICPACK_ERR_NOMEM;

    for (d = 0; d < m->disc_count; d++) {
        for (t = 0; t < m->discs[d].track_count; t++) {
            const musicpack_track *tr = &m->discs[d].tracks[t];
            refs[n].path = tr->audio.path;
            refs[n].sha256 = tr->audio.sha256;
            n++;
            for (r = 0; r < tr->representation_count; r++) {
                refs[n].path = tr->representations[r].path;
                refs[n].sha256 = tr->representations[r].sha256;
                n++;
            }
            if (tr->waveform.present) {
                refs[n].path = tr->waveform.path;
                refs[n].sha256 = tr->waveform.sha256;
                n++;
            }
        }
    }
    for (i = 0; i < m->artwork_count; i++) {
        refs[n].path = m->artwork[i].asset.path;
        refs[n].sha256 = m->artwork[i].asset.sha256;
        n++;
    }
    for (i = 0; i < m->booklet_count; i++) {
        refs[n].path = m->booklet[i].path;
        refs[n].sha256 = m->booklet[i].sha256;
        n++;
    }
    for (i = 0; i < m->lyrics_count; i++) {
        refs[n].path = m->lyrics[i].path;
        refs[n].sha256 = m->lyrics[i].sha256;
        n++;
    }
    for (i = 0; i < m->extras_count; i++) {
        refs[n].path = m->extras[i].path;
        refs[n].sha256 = m->extras[i].sha256;
        n++;
    }
    for (i = 0; i < m->analysis_count; i++) {
        refs[n].path = m->analysis[i].asset.path;
        refs[n].sha256 = m->analysis[i].asset.sha256;
        n++;
    }
    *out = refs;
    *count = n;
    return MUSICPACK_OK;
}

musicpack_status
musicpack_mpak_verify_extra(const musicpack_package *pkg, musicpack_report *rep,
                            musicpack_report_fn fn, void *ctx, int *failed)
{
    const musicpack_mpak *mf = (const musicpack_mpak *) pkg->io_ctx;
    char buf[512];

    if (mf->minor > 0) {
        snprintf(buf, sizeof buf,
                 "container: newer minor version %u (downgrade-compatible)",
                 mf->minor);
        mpak_report(rep, fn, ctx, buf, 0);
    }
    if (mf->reserved_nonzero) {
        mpak_report(rep, fn, ctx,
                    "container: nonzero reserved header bytes tolerated", 0);
    }
    if (mf->resynced) {
        mpak_report(rep, fn, ctx,
                    "container: damaged block framing; best-effort "
                    "resynchronization used (recovery scan)", 0);
    }
    if (mf->duplicate_members > 0 && mf->duplicate_example != 0) {
        snprintf(buf, sizeof buf, "duplicate object path '%s'",
                 mf->duplicate_example);
        mpak_report(rep, fn, ctx, buf, 1);
        *failed = 1;
    }

    /* ---- INDX ------------------------------------------------------- */
    if (!mf->indx_present) {
        mpak_report(rep, fn, ctx,
                    "index: missing INDX; sequential scan used", 0);
    } else {
        if (mf->indx_extra)
            mpak_report(rep, fn, ctx, "index: extra INDX block ignored", 0);
        if (!mf->indx_valid) {
            if (mf->indx_duplicate) {
                mpak_report(rep, fn, ctx,
                            "index: duplicate INDX path; index discarded", 1);
                *failed = 1;
            }
            mpak_report(rep, fn, ctx,
                        "index: corrupt INDX discarded; sequential scan used",
                        0);
        } else {
            /* Manifest/index consistency: every manifest-referenced asset
               must have exactly one INDX entry with a matching hash. */
            mpak_asset_ref *refs = 0;
            size_t count = 0, i;
            if (collect_manifest_assets(pkg->manifest, &refs, &count)
                    == MUSICPACK_OK) {
                for (i = 0; i < count; i++) {
                    const mpak_member *e = find_member(mf, refs[i].path);
                    unsigned char sha[32];
                    if (e == 0) {
                        snprintf(buf, sizeof buf,
                                 "index: missing entry for '%s'", refs[i].path);
                        mpak_report(rep, fn, ctx, buf, 1);
                        *failed = 1;
                    } else if (refs[i].sha256 != 0 &&
                               parse_sha256_hex(refs[i].sha256, sha) &&
                               memcmp(sha, e->sha256, 32) != 0) {
                        snprintf(buf, sizeof buf,
                                 "index: checksum mismatch '%s'", refs[i].path);
                        mpak_report(rep, fn, ctx, buf, 1);
                        *failed = 1;
                    }
                }
                free(refs);
            }
        }
    }

    /* ---- TAIL ------------------------------------------------------- */
    if (!mf->tail_present) {
        mpak_report(rep, fn, ctx, "completeness unproven (no TAIL)", 0);
    } else {
        if (mf->tail_extra)
            mpak_report(rep, fn, ctx, "tail: extra TAIL block ignored", 0);
        if (mf->tail_malformed) {
            mpak_report(rep, fn, ctx, "tail: malformed TAIL ignored", 0);
        } else {
            if (mf->tail_total_size != mf->file_size) {
                mpak_report(rep, fn, ctx, "tail: total size mismatch", 1);
                *failed = 1;
            }
            if ((uint64_t) mf->tail_objects != (uint64_t) mf->scan_count) {
                snprintf(buf, sizeof buf,
                         "tail: object count mismatch (%u vs %zu)",
                         (unsigned) mf->tail_objects, mf->scan_count);
                mpak_report(rep, fn, ctx, buf, 1);
                *failed = 1;
            }
            if (mf->tail_indx_offset !=
                (mf->indx_present ? mf->indx_offset : 0)) {
                mpak_report(rep, fn, ctx, "tail: INDX offset mismatch", 1);
                *failed = 1;
            }
            {
                char hex[MUSICPACK_SHA256_HEX_SIZE];
                unsigned char sha[32];
                musicpack_status rs =
                    mpak_hash_range(&mf->cio, 0, mf->tail_offset, hex,
                                    sizeof hex);
                if (rs != MUSICPACK_OK) {
                    mpak_report(rep, fn, ctx,
                                "tail: package digest cannot be computed", 1);
                    *failed = 1;
                } else if (!parse_sha256_hex(hex, sha) ||
                           memcmp(sha, mf->tail_digest, 32) != 0) {
                    mpak_report(rep, fn, ctx, "tail: package digest mismatch",
                                1);
                    *failed = 1;
                }
            }
        }
    }
    return MUSICPACK_OK;
}

/* ------------------------------------------------------------------ */
/* container-backed mpc_reader                                         */
/* ------------------------------------------------------------------ */

#define MPAK_READER_MAGIC 0x4D50414Bu

/* The reader borrows the container's byte source (the package owns it):
   every read is an absolute read through the container I/O — for
   range-backed packages this is the cached block source — so member
   reads cannot cross member boundaries and reads never disturb any
   other container state. The package must stay open while a reader
   exists. */
typedef struct mpak_member_reader_ctx {
    unsigned magic;
    mpak_cio cio;
    uint64_t base;
    uint64_t size;
    uint64_t pos;
} mpak_member_reader_ctx;

static mpc_int32_t
member_reader_read(mpc_reader *r, void *ptr, mpc_int32_t size)
{
    mpak_member_reader_ctx *c = (mpak_member_reader_ctx *) r->data;
    uint64_t remaining, take;

    if (c == 0 || c->magic != MPAK_READER_MAGIC || size <= 0)
        return 0;
    if (c->pos >= c->size)
        return 0;
    remaining = c->size - c->pos;
    take = remaining < (uint64_t) size ? remaining : (uint64_t) size;
    if (!cio_read(&c->cio, c->base + c->pos, (unsigned char *) ptr,
                  (size_t) take))
        return 0;
    c->pos += take;
    return (mpc_int32_t) take;
}

static mpc_bool_t
member_reader_seek(mpc_reader *r, mpc_seek_t offset)
{
    mpak_member_reader_ctx *c = (mpak_member_reader_ctx *) r->data;

    if (c == 0 || c->magic != MPAK_READER_MAGIC)
        return MPC_FALSE;
    if ((uint64_t) offset > c->size) /* mpc_seek_t is unsigned */
        return MPC_FALSE;
    c->pos = (uint64_t) offset;
    return MPC_TRUE;
}

static mpc_seek_t
member_reader_tell(mpc_reader *r)
{
    mpak_member_reader_ctx *c = (mpak_member_reader_ctx *) r->data;

    if (c == 0 || c->magic != MPAK_READER_MAGIC)
        return (mpc_seek_t) MPC_STATUS_FAIL;
    return (mpc_seek_t) c->pos;
}

static mpc_seek_t
member_reader_get_size(mpc_reader *r)
{
    mpak_member_reader_ctx *c = (mpak_member_reader_ctx *) r->data;

    if (c == 0 || c->magic != MPAK_READER_MAGIC)
        return (mpc_seek_t) MPC_STATUS_FAIL;
    return (mpc_seek_t) c->size;
}

/* Member reads through the container I/O are seekable — including over
   remote range transports, where a seek only moves the virtual position
   and the block cache serves the target bytes. The SV8 demuxer's
   sequential/seek behavior is therefore identical for local and remote
   packages. */
static mpc_bool_t
member_reader_canseek(mpc_reader *r)
{
    (void) r;
    return MPC_TRUE;
}

musicpack_status
musicpack_mpak_member_reader(const musicpack_package *pkg, const char *rel,
                             mpc_reader *reader)
{
    const musicpack_mpak *mf = (const musicpack_mpak *) pkg->io_ctx;
    const mpak_member *m = find_member(mf, rel);
    mpak_member_reader_ctx *c;

    if (m == 0)
        return MUSICPACK_ERR_MISSING;
    c = (mpak_member_reader_ctx *) malloc(sizeof *c);
    if (c == 0)
        return MUSICPACK_ERR_NOMEM;
    c->magic = MPAK_READER_MAGIC;
    c->cio = mf->cio;          /* borrowed; freed with the package */
    c->base = m->offset;
    c->size = m->length;
    c->pos = 0;

    memset(reader, 0, sizeof *reader);
    reader->read = member_reader_read;
    reader->seek = member_reader_seek;
    reader->tell = member_reader_tell;
    reader->get_size = member_reader_get_size;
    reader->canseek = member_reader_canseek;
    reader->data = c;
    return MUSICPACK_OK;
}

int
musicpack_mpak_reader_is_container(const mpc_reader *reader)
{
    mpak_member_reader_ctx *c;

    if (reader == 0 || reader->data == 0)
        return 0;
    c = (mpak_member_reader_ctx *) reader->data;
    return c->magic == MPAK_READER_MAGIC;
}

void
musicpack_package_track_close_reader(mpc_reader *reader)
{
    mpak_member_reader_ctx *c;

    if (reader == 0)
        return;
    if (musicpack_mpak_reader_is_container(reader)) {
        c = (mpak_member_reader_ctx *) reader->data;
        /* the container source is borrowed: nothing to close here */
        free(c);
        memset(reader, 0, sizeof *reader);
    } else {
        mpc_reader_exit_stdio(reader);
    }
}

/* ------------------------------------------------------------------ */
/* writer                                                              */
/* ------------------------------------------------------------------ */

typedef struct mpak_writer {
    FILE *f;
    uint64_t written;
    int failed;
} mpak_writer;

static int
writer_put(mpak_writer *w, const void *data, size_t len)
{
    if (w->failed)
        return -1;
    if (len > 0 && fwrite(data, 1, len, w->f) != len) {
        w->failed = 1;
        return -1;
    }
    w->written += (uint64_t) len;
    return 0;
}

typedef struct pack_member {
    char *path;
    uint64_t path_len;
    uint64_t size;
    uint64_t offset;           /* filled during layout computation */
    unsigned char sha256[32];
} pack_member;

static int
pack_member_cmp(const void *a, const void *b)
{
    const pack_member *ma = (const pack_member *) a;
    const pack_member *mb = (const pack_member *) b;
    return strcmp(ma->path, mb->path);
}

/* Appends a manifest asset to the pack list (canonical traversal order is
   the order of appends; INDX sorting happens on a copy). */
static musicpack_status
pack_add(pack_member **list, size_t *count, size_t *cap, const char *path,
         const char *sha_hex)
{
    pack_member *slot;

    if (*count >= MPAK_MAX_MEMBERS)
        return MUSICPACK_ERR_INVALID;
    if (*count >= *cap) {
        size_t ncap = *cap == 0 ? 64 : *cap * 2;
        pack_member *nl = (pack_member *) realloc(*list, ncap * sizeof *nl);
        if (nl == 0)
            return MUSICPACK_ERR_NOMEM;
        *list = nl;
        *cap = ncap;
    }
    slot = &(*list)[*count];
    memset(slot, 0, sizeof *slot);
    slot->path = strdup(path);
    if (slot->path == 0)
        return MUSICPACK_ERR_NOMEM;
    slot->path_len = strlen(path);
    if (!parse_sha256_hex(sha_hex, slot->sha256)) {
        free(slot->path);
        slot->path = 0;
        return MUSICPACK_ERR_INVALID;
    }
    (*count)++;
    return MUSICPACK_OK;
}

static musicpack_status
pack_prepare_members(const musicpack_package *pkg, pack_member **out,
                     size_t *out_count)
{
    const musicpack_manifest *m = musicpack_package_manifest(pkg);
    pack_member *list = 0;
    size_t count = 0, cap = 0, d, t, r, i;
    musicpack_status s;

    /* Manifest canonical traversal order: primary audio in media/track
       order (playback order), then per-track representations, per-track
       waveforms, then package-scope assets. */
    for (d = 0; d < m->disc_count; d++)
        for (t = 0; t < m->discs[d].track_count; t++) {
            const musicpack_track *tr = &m->discs[d].tracks[t];
            s = pack_add(&list, &count, &cap, tr->audio.path, tr->audio.sha256);
            if (s != MUSICPACK_OK)
                goto fail;
        }
    for (d = 0; d < m->disc_count; d++)
        for (t = 0; t < m->discs[d].track_count; t++) {
            const musicpack_track *tr = &m->discs[d].tracks[t];
            for (r = 0; r < tr->representation_count; r++) {
                s = pack_add(&list, &count, &cap, tr->representations[r].path,
                             tr->representations[r].sha256);
                if (s != MUSICPACK_OK)
                    goto fail;
            }
        }
    for (d = 0; d < m->disc_count; d++)
        for (t = 0; t < m->discs[d].track_count; t++) {
            const musicpack_track *tr = &m->discs[d].tracks[t];
            if (tr->waveform.present) {
                s = pack_add(&list, &count, &cap, tr->waveform.path,
                             tr->waveform.sha256);
                if (s != MUSICPACK_OK)
                    goto fail;
            }
        }
    for (i = 0; i < m->artwork_count; i++) {
        s = pack_add(&list, &count, &cap, m->artwork[i].asset.path,
                     m->artwork[i].asset.sha256);
        if (s != MUSICPACK_OK)
            goto fail;
    }
    for (i = 0; i < m->booklet_count; i++) {
        s = pack_add(&list, &count, &cap, m->booklet[i].path,
                     m->booklet[i].sha256);
        if (s != MUSICPACK_OK)
            goto fail;
    }
    for (i = 0; i < m->lyrics_count; i++) {
        s = pack_add(&list, &count, &cap, m->lyrics[i].path, m->lyrics[i].sha256);
        if (s != MUSICPACK_OK)
            goto fail;
    }
    for (i = 0; i < m->extras_count; i++) {
        s = pack_add(&list, &count, &cap, m->extras[i].path, m->extras[i].sha256);
        if (s != MUSICPACK_OK)
            goto fail;
    }
    for (i = 0; i < m->analysis_count; i++) {
        s = pack_add(&list, &count, &cap, m->analysis[i].asset.path,
                     m->analysis[i].asset.sha256);
        if (s != MUSICPACK_OK)
            goto fail;
    }
    *out = list;
    *out_count = count;
    return MUSICPACK_OK;

fail:
    {
        size_t j;
        for (j = 0; j < count; j++)
            free(list[j].path);
        free(list);
    }
    return s;
}

static void
pack_members_free(pack_member *members, size_t count)
{
    size_t j;

    if (members == 0)
        return;
    for (j = 0; j < count; j++)
        free(members[j].path);
    free(members);
}

/* Copies the member file into the writer, cross-checking size and hash
   against the layout-computed expectations (a file that changed under us
   aborts the pack). */
static musicpack_status
pack_copy_member(mpak_writer *w, const char *root, const pack_member *pm)
{
    char abs[MUSICPACK_PATH_MAX + 2];
    char expect[MUSICPACK_SHA256_HEX_SIZE];
    char hex[MUSICPACK_SHA256_HEX_SIZE];
    FILE *in;
    unsigned char buf[MUSICPACK_IO_CHUNK];
    uint64_t copied = 0;
    size_t n;

    if (musicpack_path_resolve(root, pm->path, abs, sizeof abs) != MUSICPACK_OK)
        return MUSICPACK_ERR_PATH;
    in = musicpack_open_regular_read(abs);
    if (in == 0)
        return MUSICPACK_ERR_MISSING;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (writer_put(w, buf, n) != 0) {
            fclose(in);
            return MUSICPACK_ERR_IO;
        }
        copied += (uint64_t) n;
    }
    if (ferror(in)) {
        fclose(in);
        return MUSICPACK_ERR_IO;
    }
    fclose(in);
    if (copied != pm->size)
        return MUSICPACK_ERR_CHECKSUM;
    sha_bytes_to_hex(pm->sha256, expect);
    if (musicpack_sha256_file_range(abs, 0, pm->size, hex, sizeof hex)
            != MUSICPACK_OK)
        return MUSICPACK_ERR_IO;
    if (!musicpack_sha256_eq(hex, expect))
        return MUSICPACK_ERR_CHECKSUM;
    return MUSICPACK_OK;
}

musicpack_status
musicpack_mpak_pack_dir(const char *dir, const char *out_file,
                        musicpack_status *status)
{
    musicpack_status local = MUSICPACK_OK, s = MUSICPACK_OK;
    musicpack_package *pkg = 0;
    musicpack_report rep;
    pack_member *members = 0;
    size_t member_count = 0, i;
    unsigned char *manifest_bytes = 0;
    size_t manifest_len = 0;
    char manifest_path[MUSICPACK_PATH_MAX + 2];
    mpak_writer w;
    uint64_t total = 0, indx_payload, pos, tail_offset;
    pack_member *sorted = 0;
    FILE *out = 0;

    if (status == 0)
        status = &local;
    *status = MUSICPACK_OK;
    memset(&w, 0, sizeof w);

    pkg = musicpack_package_open_dir(dir, status);
    if (pkg == 0)
        return *status;

    /* Pack only verified packages: the manifest hashes that land in INDX
       must describe the bytes actually stored. */
    memset(&rep, 0, sizeof rep);
    if (musicpack_package_verify(pkg, &rep, 0, 0) != MUSICPACK_OK) {
        musicpack_package_close(pkg);
        *status = MUSICPACK_ERR_CHECKSUM;
        return *status;
    }

    s = pack_prepare_members(pkg, &members, &member_count);
    if (s != MUSICPACK_OK)
        goto fail;

    /* Exact manifest bytes (never regenerated). */
    if (snprintf(manifest_path, sizeof manifest_path, "%s/manifest.json", dir)
            >= (int) sizeof manifest_path) {
        s = MUSICPACK_ERR_PATH;
        goto fail;
    }
    {
        FILE *mf = musicpack_open_regular_read(manifest_path);
        long long sz;
        if (mf == 0) {
            s = MUSICPACK_ERR_IO;
            goto fail;
        }
        sz = musicpack_checked_file_size(manifest_path);
        if (sz < 0 || (unsigned long long) sz > MPAK_MANIFEST_MAX) {
            fclose(mf);
            s = MUSICPACK_ERR_INVALID;
            goto fail;
        }
        manifest_len = (size_t) sz;
        manifest_bytes = (unsigned char *) malloc(manifest_len > 0 ? manifest_len : 1);
        if (manifest_bytes == 0) {
            fclose(mf);
            s = MUSICPACK_ERR_NOMEM;
            goto fail;
        }
        if (manifest_len > 0 &&
            fread(manifest_bytes, 1, manifest_len, mf) != manifest_len) {
            fclose(mf);
            s = MUSICPACK_ERR_IO;
            goto fail;
        }
        fclose(mf);
    }

    /* The MANF bytes must describe the package that was verified: the
       directory was verified through a separate read of manifest.json,
       so re-parse the embedded bytes and require the same canonical
       model. A manifest changed underneath the packer fails the pack
       instead of embedding unverified semantics. */
    {
        char *zbuf = (char *) malloc(manifest_len + 1);
        musicpack_manifest *m2;
        char *ja = 0, *jb = 0;
        int same;

        if (zbuf == 0) {
            s = MUSICPACK_ERR_NOMEM;
            goto fail;
        }
        memcpy(zbuf, manifest_bytes, manifest_len);
        zbuf[manifest_len] = '\0';
        if (memchr(zbuf, '\0', manifest_len) != 0) {
            free(zbuf);
            s = MUSICPACK_ERR_JSON;
            goto fail;
        }
        m2 = musicpack_manifest_parse(zbuf, 0);
        free(zbuf);
        if (m2 == 0) {
            s = MUSICPACK_ERR_JSON;
            goto fail;
        }
        if (musicpack_manifest_write(pkg->manifest, &ja) != MUSICPACK_OK ||
            musicpack_manifest_write(m2, &jb) != MUSICPACK_OK) {
            musicpack_manifest_free(m2);
            free(ja);
            s = MUSICPACK_ERR_NOMEM;
            goto fail;
        }
        same = strcmp(ja, jb) == 0;
        musicpack_manifest_free(m2);
        free(ja);
        free(jb);
        if (!same) {
            s = MUSICPACK_ERR_CHECKSUM;
            goto fail;
        }
    }

    /* Layout computation (single pass, no backpatching): sizes and
       offsets are fully known before the first byte is written. */
    indx_payload = 4;
    for (i = 0; i < member_count; i++) {
        char abs[MUSICPACK_PATH_MAX + 2];
        long long sz;

        indx_payload += 2 + members[i].path_len + 48;
        if (musicpack_path_resolve(dir, members[i].path, abs, sizeof abs)
                != MUSICPACK_OK) {
            s = MUSICPACK_ERR_PATH;
            goto fail;
        }
        sz = musicpack_checked_file_size(abs);
        if (sz < 0) {
            s = MUSICPACK_ERR_MISSING;
            goto fail;
        }
        members[i].size = (uint64_t) sz;
    }
    for (i = 0; i < member_count; i++) {
        if (members[i].size > MPAK_MAX_MEMBER_BYTES) {
            s = MUSICPACK_ERR_INVALID;
            goto fail;
        }
        if (!u64_add(total, members[i].size, &total)) {
            s = MUSICPACK_ERR_INVALID;
            goto fail;
        }
    }
    if (total > MPAK_MAX_TOTAL_BYTES) {
        s = MUSICPACK_ERR_INVALID;
        goto fail;
    }
    pos = MPAK_HEADER_SIZE + MPAK_BLOCK_HEADER_SIZE + indx_payload
        + MPAK_BLOCK_HEADER_SIZE + manifest_len;
    for (i = 0; i < member_count; i++) {
        pos += MPAK_BLOCK_HEADER_SIZE + 2 + members[i].path_len;
        members[i].offset = pos;
        pos += members[i].size;
    }

    /* ---- write ------------------------------------------------------ */
    out = fopen(out_file, "wb");
    if (out == 0) {
        s = MUSICPACK_ERR_IO;
        goto fail;
    }
    w.f = out;

    /* header */
    {
        unsigned char hdr[MPAK_HEADER_SIZE];
        memcpy(hdr, MPAK_MAGIC, 4);
        hdr[4] = (unsigned char) MUSICPACK_MPAK_MAJOR;
        hdr[5] = 0;
        wr_u16(hdr + 6, (uint16_t) MUSICPACK_MPAK_FLAG_INDX_PRESENT);
        memset(hdr + 8, 0, 8);
        if (writer_put(&w, hdr, sizeof hdr) != 0) {
            s = MUSICPACK_ERR_IO;
            goto fail;
        }
    }

    /* INDX block (entries sorted lexicographically by path; offsets were
       computed in traversal order and travel with the sorted copies) */
    sorted = (pack_member *) malloc(member_count > 0
                                    ? member_count * sizeof *sorted : 1);
    if (sorted == 0) {
        s = MUSICPACK_ERR_NOMEM;
        goto fail;
    }
    memcpy(sorted, members, member_count * sizeof *members);
    qsort(sorted, member_count, sizeof *sorted, pack_member_cmp);
    {
        unsigned char bhdr[MPAK_BLOCK_HEADER_SIZE];
        unsigned char tmp[8];

        mpak_write_block_header(bhdr, "INDX", indx_payload);
        if (writer_put(&w, bhdr, sizeof bhdr) != 0) {
            s = MUSICPACK_ERR_IO;
            goto fail;
        }
        wr_u32(tmp, (uint32_t) member_count);
        if (writer_put(&w, tmp, 4) != 0) {
            s = MUSICPACK_ERR_IO;
            goto fail;
        }
        for (i = 0; i < member_count; i++) {
            unsigned char p2[2];
            wr_u16(p2, (uint16_t) sorted[i].path_len);
            if (writer_put(&w, p2, 2) != 0 ||
                writer_put(&w, sorted[i].path, (size_t) sorted[i].path_len) != 0) {
                s = MUSICPACK_ERR_IO;
                goto fail;
            }
            wr_u64(tmp, sorted[i].offset);
            if (writer_put(&w, tmp, 8) != 0) {
                s = MUSICPACK_ERR_IO;
                goto fail;
            }
            wr_u64(tmp, sorted[i].size);
            if (writer_put(&w, tmp, 8) != 0) {
                s = MUSICPACK_ERR_IO;
                goto fail;
            }
            if (writer_put(&w, sorted[i].sha256, 32) != 0) {
                s = MUSICPACK_ERR_IO;
                goto fail;
            }
        }
    }

    /* MANF block (exact manifest bytes) */
    {
        unsigned char bhdr[MPAK_BLOCK_HEADER_SIZE];
        mpak_write_block_header(bhdr, "MANF", (uint64_t) manifest_len);
        if (writer_put(&w, bhdr, sizeof bhdr) != 0 ||
            (manifest_len > 0 &&
             writer_put(&w, manifest_bytes, manifest_len) != 0)) {
            s = MUSICPACK_ERR_IO;
            goto fail;
        }
    }

    /* DATA blocks in manifest canonical traversal order */
    for (i = 0; i < member_count; i++) {
        unsigned char bhdr[MPAK_BLOCK_HEADER_SIZE];
        unsigned char p2[2];
        uint64_t payload = 2 + members[i].path_len + members[i].size;

        mpak_write_block_header(bhdr, "DATA", payload);
        wr_u16(p2, (uint16_t) members[i].path_len);
        if (writer_put(&w, bhdr, sizeof bhdr) != 0 ||
            writer_put(&w, p2, 2) != 0 ||
            writer_put(&w, members[i].path, (size_t) members[i].path_len) != 0) {
            s = MUSICPACK_ERR_IO;
            goto fail;
        }
        s = pack_copy_member(&w, dir, &members[i]);
        if (s != MUSICPACK_OK)
            goto fail;
    }

    /* TAIL block: digest over every byte preceding TAIL (computed from
       the flushed file, so the digest excludes TAIL itself — no
       circularity) */
    tail_offset = w.written;
    pos = tail_offset + MPAK_BLOCK_HEADER_SIZE + MPAK_TAIL_PAYLOAD_SIZE;
    {
        unsigned char bhdr[MPAK_BLOCK_HEADER_SIZE];
        unsigned char tbuf[MPAK_TAIL_PAYLOAD_SIZE];
        char hex[MUSICPACK_SHA256_HEX_SIZE];
        unsigned char sha[32];

        if (fflush(out) != 0) {
            s = MUSICPACK_ERR_IO;
            goto fail;
        }
        mpak_write_block_header(bhdr, "TAIL", MPAK_TAIL_PAYLOAD_SIZE);
        if (writer_put(&w, bhdr, sizeof bhdr) != 0) {
            s = MUSICPACK_ERR_IO;
            goto fail;
        }
        if (musicpack_sha256_file_range(out_file, 0, tail_offset, hex,
                                        sizeof hex) != MUSICPACK_OK ||
            !parse_sha256_hex(hex, sha)) {
            s = MUSICPACK_ERR_IO;
            goto fail;
        }
        wr_u64(tbuf, pos);
        wr_u32(tbuf + 8, (uint32_t) member_count);
        wr_u64(tbuf + 12, MPAK_HEADER_SIZE); /* INDX block offset */
        memcpy(tbuf + 20, sha, 32);
        if (writer_put(&w, tbuf, sizeof tbuf) != 0) {
            s = MUSICPACK_ERR_IO;
            goto fail;
        }
    }

    if (fclose(out) != 0) {
        out = 0;
        s = MUSICPACK_ERR_IO;
        goto fail;
    }
    out = 0;

    free(sorted);
    free(manifest_bytes);
    pack_members_free(members, member_count);
    musicpack_package_close(pkg);
    *status = MUSICPACK_OK;
    return MUSICPACK_OK;

fail:
    if (out != 0)
        fclose(out);
    remove(out_file);
    if (s == MUSICPACK_OK)
        s = MUSICPACK_ERR_IO;
    free(sorted);
    free(manifest_bytes);
    pack_members_free(members, member_count);
    musicpack_package_close(pkg);
    *status = s;
    return s;
}

/* ------------------------------------------------------------------ */
/* unpack / recovery                                                   */
/* ------------------------------------------------------------------ */

static int
mkdir_one(const char *path)
{
#ifdef _WIN32
    if (_mkdir(path) == 0)
        return 0;
    return errno == EEXIST ? 0 : -1;
#else
    if (mkdir(path, 0777) == 0)
        return 0;
    return errno == EEXIST ? 0 : -1;
#endif
}

static int
mkdir_parents(const char *dir)
{
    char tmp[MUSICPACK_PATH_MAX * 2 + 4];
    size_t len, i;

    len = strlen(dir);
    if (len == 0 || len >= sizeof tmp)
        return -1;
    memcpy(tmp, dir, len + 1);
    for (i = 1; tmp[i] != '\0'; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            tmp[i] = '\0';
            if (mkdir_one(tmp) != 0)
                return -1;
            tmp[i] = c;
        }
    }
    return mkdir_one(tmp);
}

musicpack_status
musicpack_mpak_unpack(const char *in_file, const char *out_dir,
                      musicpack_report *rep, musicpack_report_fn fn, void *ctx)
{
    musicpack_report local = { 0, 0 };
    musicpack_mpak *mf = 0;
    musicpack_status s;
    char path_buf[MUSICPACK_PATH_MAX * 2 + 4];
    unsigned char buf[MUSICPACK_IO_CHUNK];
    size_t i;
    int failed = 0;

    if (rep == 0)
        rep = &local;
    s = mpak_load(in_file, 0, 1, &mf);
    if (s != MUSICPACK_OK)
        return s;

    if (mkdir_parents(out_dir) != 0) {
        mpak_free(mf);
        return MUSICPACK_ERR_IO;
    }

    /* manifest.json from MANF bytes: physical recovery does not require a
       parseable manifest */
    if (mf->manifest != 0) {
        FILE *mfout;
        if (snprintf(path_buf, sizeof path_buf, "%s/manifest.json", out_dir)
                >= (int) sizeof path_buf) {
            mpak_free(mf);
            return MUSICPACK_ERR_PATH;
        }
        mfout = fopen(path_buf, "wb");
        if (mfout == 0) {
            mpak_free(mf);
            return MUSICPACK_ERR_IO;
        }
        {
            int wok = mf->manifest_len == 0 ||
                      fwrite(mf->manifest, 1, mf->manifest_len, mfout)
                          == mf->manifest_len;
            int cok = fclose(mfout) == 0;
            if (!wok || !cok) {
                mpak_free(mf);
                return MUSICPACK_ERR_IO;
            }
        }
    } else {
        mpak_report(rep, fn, ctx, "manifest: no MANF block present", 0);
    }
    if (mf->manifest_count > 1)
        mpak_report(rep, fn, ctx, "manifest: extra MANF block ignored", 0);

    /* members, in DATA (scan) order; each member is SHA-256 verified
       against INDX before being treated as intact */
    for (i = 0; i < mf->scan_count; i++) {
        const mpak_member *m = &mf->scan[i];
        const mpak_member *idx_entry =
            mf->indx_valid ? find_member(mf, m->path) : 0;
        FILE *out;
        uint64_t remaining;
        int copy_ok = 1;

        if (snprintf(path_buf, sizeof path_buf, "%s/%s", out_dir, m->path)
                >= (int) sizeof path_buf) {
            failed = 1;
            break;
        }
        {
            char *slash = strrchr(path_buf, '/');
            if (slash != 0) {
                *slash = '\0';
                if (mkdir_parents(path_buf) != 0) {
                    s = MUSICPACK_ERR_IO;
                    failed = 1;
                    break;
                }
                *slash = '/';
            }
        }
        out = fopen(path_buf, "wb");
        if (out == 0) {
            char msg[512];
            snprintf(msg, sizeof msg, "extract: cannot write '%s'", m->path);
            mpak_report(rep, fn, ctx, msg, 1);
            failed = 1;
            continue;
        }
        remaining = m->length;
        while (copy_ok && remaining > 0) {
            size_t want = remaining > sizeof buf ? sizeof buf
                                                  : (size_t) remaining;
            if (!cio_read(&mf->cio, m->offset + (m->length - remaining),
                          buf, want)) {
                copy_ok = 0;
                break;
            }
            if (fwrite(buf, 1, want, out) != want) {
                copy_ok = 0;
                break;
            }
            remaining -= want;
        }
        fclose(out);

        if (!copy_ok) {
            char msg[512];
            snprintf(msg, sizeof msg, "extract: cannot read '%s'", m->path);
            mpak_report(rep, fn, ctx, msg, 1);
            failed = 1;
            continue;
        }
        if (idx_entry != 0) {
            char hex[MUSICPACK_SHA256_HEX_SIZE];
            unsigned char sha[32];
            if (mpak_hash_range(&mf->cio, m->offset, m->length,
                                hex, sizeof hex) == MUSICPACK_OK &&
                parse_sha256_hex(hex, sha) &&
                memcmp(sha, idx_entry->sha256, 32) != 0) {
                char msg[512];
                snprintf(msg, sizeof msg,
                         "member '%s' checksum mismatch (extracted anyway)",
                         m->path);
                mpak_report(rep, fn, ctx, msg, 1);
                failed = 1;
            }
        }
    }
    if (mf->duplicate_members > 0 && mf->duplicate_example != 0) {
        char msg[512];
        snprintf(msg, sizeof msg, "duplicate object path '%s' (first kept)",
                 mf->duplicate_example);
        mpak_report(rep, fn, ctx, msg, 0);
    }
    if (mf->skipped_members > 0) {
        char msg[512];
        snprintf(msg, sizeof msg,
                 "%zu DATA member(s) skipped: invalid path preamble "
                 "(not extracted)", mf->skipped_members);
        mpak_report(rep, fn, ctx, msg, 1);
        failed = 1;
    }
    if (mf->resynced)
        mpak_report(rep, fn, ctx,
                    "container: damaged block framing; best-effort "
                    "resynchronization used (recovery scan)", 0);
    if (mf->indx_present && !mf->indx_valid)
        mpak_report(rep, fn, ctx,
                    "index: corrupt INDX discarded; members extracted without "
                    "index verification", 0);
    if (!mf->tail_present)
        mpak_report(rep, fn, ctx, "completeness unproven (no TAIL)", 0);

    mpak_free(mf);
    return failed ? MUSICPACK_ERR_CHECKSUM : MUSICPACK_OK;
}

/* ------------------------------------------------------------------ */
/* local stdio range-source adapter                                    */
/* ------------------------------------------------------------------ */

/* The context is the same mpak_file_ctx the file container backend
   uses, so the adapter is exactly the hardened local-file source the
   container itself reads through. */
static musicpack_status
stdio_source_size(void *ctx, uint64_t *out)
{
    mpak_file_ctx *c = (mpak_file_ctx *) ctx;

    if (c == 0)
        return MUSICPACK_ERR_INVALID;
    *out = c->size;
    return MUSICPACK_OK;
}

static musicpack_status
stdio_source_read(void *ctx, uint64_t offset, unsigned char *buf, size_t len)
{
    mpak_file_ctx *c = (mpak_file_ctx *) ctx;

    if (c == 0)
        return MUSICPACK_ERR_INVALID;
    return file_read_at(ctx, offset, buf, len) ? MUSICPACK_OK
                                               : MUSICPACK_ERR_IO;
}

static void
stdio_source_destroy(void *ctx)
{
    file_cio_close(ctx);
}

musicpack_status
musicpack_range_source_stdio(const char *path, musicpack_range_source *out)
{
    mpak_cio cio;
    uint64_t size;
    musicpack_status s;

    if (path == 0 || out == 0)
        return MUSICPACK_ERR_INVALID;
    s = mpak_open_file_cio(path, &cio, &size);
    if (s != MUSICPACK_OK)
        return s;
    out->ctx = cio.ctx;
    out->size = stdio_source_size;
    out->read = stdio_source_read;
    out->destroy = stdio_source_destroy;
    return MUSICPACK_OK;
}
