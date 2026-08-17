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

  * Neither the name of the MusicPack Development Team nor the
  names of its contributors may be used to endorse or promote
  products derived from this software without specific prior
  written permission.

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
/// \file package.c
/// Directory-form `.mpack` package handle.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
# include <windows.h>
# include <sys/stat.h>
#else
# include <dirent.h>
# include <fcntl.h>
# include <sys/stat.h>
# include <unistd.h>
#endif

#include "internal.h"
#include <musicpack/checksum.h>
#include <musicpack/path.h>
#include <musicpack/sonic.h>
#include <musicpack/waveform.h>

#define MANIFEST_NAME "manifest.json"
#define MANIFEST_MAX  (16u * 1024u * 1024u)

struct musicpack_package {
    char *root;             /* absolute package root */
    musicpack_manifest *manifest;
    cJSON *original;        /* original manifest tree (unknown-field save) */
};

/* ------------------------------------------------------------------ */
/* manifest file I/O                                                   */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
static int
is_regular_file(const char *path)
{
    struct _stat st;
    if (_stat(path, &st) != 0)
        return 0;
    return (st.st_mode & _S_IFREG) != 0;
}
#endif

static FILE *
open_regular_read(const char *path)
{
#ifdef _WIN32
    if (!is_regular_file(path))
        return 0;
    return fopen(path, "rb");
#else
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
    struct stat st;
    if (fd < 0)
        return 0;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink > 1) {
        close(fd);
        return 0;
    }
    {
        int flags = fcntl(fd, F_GETFL);
        if (flags >= 0)
            fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
    {
        FILE *f = fdopen(fd, "rb");
        if (f == 0)
            close(fd);
        return f;
    }
#endif
}

static char *
read_file(const char *path, size_t max, size_t *len_out, musicpack_status *status)
{
    FILE *f;
    long len;
    char *buf;

    f = open_regular_read(path);
    if (f == 0) {
        *status = MUSICPACK_ERR_IO;
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); *status = MUSICPACK_ERR_IO; return 0; }
    len = ftell(f);
    if (len < 0 || (size_t) len > max) { fclose(f); *status = MUSICPACK_ERR_IO; return 0; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); *status = MUSICPACK_ERR_IO; return 0; }
    buf = (char *) malloc((size_t) len + 1);
    if (buf == 0) { fclose(f); *status = MUSICPACK_ERR_NOMEM; return 0; }
    if (len > 0 && fread(buf, 1, (size_t) len, f) != (size_t) len) {
        free(buf); fclose(f); *status = MUSICPACK_ERR_IO; return 0;
    }
    fclose(f);
    buf[len] = '\0';
    *len_out = (size_t) len;
    *status = MUSICPACK_OK;
    return buf;
}

static musicpack_status
write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    size_t len = strlen(text);
    if (f == 0)
        return MUSICPACK_ERR_IO;
    if (len > 0 && fwrite(text, 1, len, f) != len) {
        fclose(f);
        return MUSICPACK_ERR_IO;
    }
    if (fclose(f) != 0)
        return MUSICPACK_ERR_IO;
    return MUSICPACK_OK;
}

/* ------------------------------------------------------------------ */
/* open / close                                                        */
/* ------------------------------------------------------------------ */

musicpack_package *
musicpack_package_open_dir(const char *dir, musicpack_status *status)
{
    musicpack_package *pkg;
    musicpack_status local = MUSICPACK_OK;
    char *json, manifest_path[MUSICPACK_PATH_MAX + 2];
    size_t json_len;
    cJSON *root;

    if (status == 0)
        status = &local;
    *status = MUSICPACK_OK;
    if (dir == 0) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    if (snprintf(manifest_path, sizeof manifest_path, "%s/%s", dir, MANIFEST_NAME)
            >= (int) sizeof manifest_path) {
        *status = MUSICPACK_ERR_PATH;
        return 0;
    }

    json = read_file(manifest_path, MANIFEST_MAX, &json_len, status);
    if (json == 0)
        return 0;

    if (memchr(json, '\0', json_len) != 0) {
        free(json);
        *status = MUSICPACK_ERR_JSON;
        return 0;
    }
    root = cJSON_ParseWithLengthOpts(json, json_len + 1, 0, 1);
    if (root == 0) {
        free(json);
        *status = MUSICPACK_ERR_JSON;
        return 0;
    }

    pkg = (musicpack_package *) calloc(1, sizeof *pkg);
    if (pkg == 0) {
        free(json);
        cJSON_Delete(root);
        *status = MUSICPACK_ERR_NOMEM;
        return 0;
    }
    pkg->manifest = (musicpack_manifest *) calloc(1, sizeof *pkg->manifest);
    if (pkg->manifest == 0) {
        free(pkg);
        free(json);
        cJSON_Delete(root);
        *status = MUSICPACK_ERR_NOMEM;
        return 0;
    }
    *status = musicpack_manifest_parse_tree(root, pkg->manifest);
    if (*status != MUSICPACK_OK) {
        musicpack_manifest_free(pkg->manifest);
        free(pkg);
        free(json);
        cJSON_Delete(root);
        return 0;
    }
    pkg->original = root;
    free(json);

    pkg->root = strdup(dir);
    if (pkg->root == 0) {
        musicpack_manifest_free(pkg->manifest);
        cJSON_Delete(pkg->original);
        free(pkg);
        *status = MUSICPACK_ERR_NOMEM;
        return 0;
    }
    return pkg;
}

void
musicpack_package_close(musicpack_package *pkg)
{
    if (pkg == 0)
        return;
    musicpack_manifest_free(pkg->manifest);
    cJSON_Delete(pkg->original);
    free(pkg->root);
    free(pkg);
}

const musicpack_manifest *
musicpack_package_manifest(const musicpack_package *pkg)
{
    return pkg != 0 ? pkg->manifest : 0;
}

musicpack_manifest *
musicpack_package_manifest_mutable(musicpack_package *pkg)
{
    return pkg != 0 ? pkg->manifest : 0;
}

musicpack_status
musicpack_package_resolve_path(const musicpack_package *pkg, const char *rel,
                               char *out, size_t cap)
{
    if (pkg == 0)
        return MUSICPACK_ERR_INVALID;
    return musicpack_path_resolve(pkg->root, rel, out, cap);
}

/* ------------------------------------------------------------------ */
/* verify                                                              */
/* ------------------------------------------------------------------ */

static void
report(musicpack_report *rep, musicpack_report_fn fn, void *ctx,
       const char *message, int is_error)
{
    if (is_error)
        rep->errors++;
    else
        rep->warnings++;
    if (fn != 0)
        fn(ctx, message, is_error);
}

/* The public manifest model is mutable. Bound and check every array before
   passing it to the existing serializer's deep round-trip validation. */
static int
manifest_shape_safe(const musicpack_manifest *m)
{
    size_t d, t;

    if (m == 0 || m->album_artist_count == 0 ||
        m->album_artist_count > MUSICPACK_MANIFEST_MAX_ARTISTS_PER_CREDIT ||
        m->album_artists == 0 || m->genre_count > MUSICPACK_MANIFEST_MAX_GENRES ||
        (m->genre_count > 0 && m->genres == 0) || m->disc_count == 0 ||
        m->disc_count > MUSICPACK_MANIFEST_MAX_DISCS || m->discs == 0 ||
        m->artwork_count > MUSICPACK_MANIFEST_MAX_ARTWORK ||
        (m->artwork_count > 0 && m->artwork == 0) ||
        m->booklet_count > MUSICPACK_MANIFEST_MAX_BOOKLET ||
        (m->booklet_count > 0 && m->booklet == 0) ||
        m->lyrics_count > MUSICPACK_MANIFEST_MAX_LYRICS ||
        (m->lyrics_count > 0 && m->lyrics == 0) ||
        m->extras_count > MUSICPACK_MANIFEST_MAX_EXTRAS ||
        (m->extras_count > 0 && m->extras == 0) ||
        m->analysis_count > MUSICPACK_MANIFEST_MAX_ANALYSIS ||
        (m->analysis_count > 0 && m->analysis == 0))
        return 0;

    for (d = 0; d < m->disc_count; d++) {
        const musicpack_disc *disc = &m->discs[d];
        if (disc->track_count == 0 ||
            disc->track_count > MUSICPACK_MANIFEST_MAX_TRACKS_PER_DISC ||
            disc->tracks == 0)
            return 0;
        for (t = 0; t < disc->track_count; t++) {
            const musicpack_track *track = &disc->tracks[t];
            if (track->artist_count > MUSICPACK_MANIFEST_MAX_ARTISTS_PER_CREDIT ||
                (track->artist_count > 0 && track->artists == 0))
                return 0;
        }
    }
    return 1;
}

/* ---- verification resource budgets ------------------------------------- */
/* Bounded totals enforced during a single verification pass so an untrusted
   package cannot drive unbounded I/O. Per-file and aggregate byte budgets
   live in manifest.h; the inode list avoids re-hashing the same object
   (hard links are already rejected, but an asset may be referenced twice
   through two manifest paths). */

#ifndef _WIN32
typedef struct verify_inode {
    unsigned long long dev, ino;
} verify_inode;
#endif

typedef struct verify_budget {
    unsigned long long total_bytes;
    size_t inode_count;
    size_t inode_cap;
#ifndef _WIN32
    verify_inode *inodes;
#endif
} verify_budget;

static long long
checked_file_size(const char *path)
{
#ifdef _WIN32
    struct _stat st;
    if (_stat(path, &st) != 0)
        return -1;
    if ((st.st_mode & _S_IFREG) == 0)
        return -1;
    return (long long) st.st_size;
#else
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
    struct stat st;
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink > 1) {
        close(fd);
        return -1;
    }
    close(fd);
    return (long long) st.st_size;
#endif
}

static void
verify_budget_init(verify_budget *b)
{
    memset(b, 0, sizeof *b);
}

static void
verify_budget_free(verify_budget *b)
{
#ifndef _WIN32
    free(b->inodes);
    b->inodes = 0;
#endif
}

#ifndef _WIN32
/* Returns 1 if (dev,ino) was already seen (content already hashed), else
   records it and returns 0. Bounded by the asset cap. */
static int
inode_seen(verify_budget *b, unsigned long long dev, unsigned long long ino)
{
    size_t i;
    for (i = 0; i < b->inode_count; i++)
        if (b->inodes[i].dev == dev && b->inodes[i].ino == ino)
            return 1;
    if (b->inode_count >= b->inode_cap) {
        size_t ncap = b->inode_cap == 0 ? 64 : b->inode_cap * 2;
        verify_inode *ni = (verify_inode *) realloc(b->inodes, ncap * sizeof *ni);
        if (ni == 0)
            return 0;
        b->inodes = ni;
        b->inode_cap = ncap;
    }
    b->inodes[b->inode_count].dev = dev;
    b->inodes[b->inode_count].ino = ino;
    b->inode_count++;
    return 0;
}

static void
inode_of(const char *path, unsigned long long *dev, unsigned long long *ino)
{
    struct stat st;
    if (lstat(path, &st) == 0) {
        *dev = (unsigned long long) st.st_dev;
        *ino = (unsigned long long) st.st_ino;
    }
}
#endif

static void
verify_assets(const musicpack_package *pkg, const musicpack_asset *assets,
              size_t count, const char *kind, musicpack_report *rep,
              musicpack_report_fn fn, void *ctx, int *failed,
              verify_budget *budget)
{
    size_t i;

    for (i = 0; i < count; i++) {
        const musicpack_asset *a = &assets[i];
        char abs[MUSICPACK_PATH_MAX + 2];
        char buf[512];
        long long size;

        if (musicpack_package_resolve_path(pkg, a->path, abs, sizeof abs) != MUSICPACK_OK) {
            snprintf(buf, sizeof buf, "%s: unsafe path '%s'", kind, a->path);
            report(rep, fn, ctx, buf, 1);
            *failed = 1;
            continue;
        }
        size = checked_file_size(abs);
        if (size < 0) {
            snprintf(buf, sizeof buf, "%s: missing file '%s'", kind, a->path);
            report(rep, fn, ctx, buf, 1);
            *failed = 1;
            continue;
        }
        if (size > (long long) MUSICPACK_MANIFEST_MAX_FILE_SIZE) {
            snprintf(buf, sizeof buf, "%s: '%s' exceeds %llu-byte file limit",
                     kind, a->path,
                     (unsigned long long) MUSICPACK_MANIFEST_MAX_FILE_SIZE);
            report(rep, fn, ctx, buf, 1);
            *failed = 1;
            continue;
        }
        if (budget != 0) {
#ifndef _WIN32
            unsigned long long dev = 0, ino = 0;
            inode_of(abs, &dev, &ino);
            if (inode_seen(budget, dev, ino)) {
                /* same underlying object already hashed this pass */
                continue;
            }
#else
            /* Windows st_ino is not a reliable object identity (often 0 or
               identical across files), so inode-based dedup is disabled there
               and every referenced asset is hashed. */
#endif
            budget->total_bytes += (unsigned long long) size;
            if (budget->total_bytes > MUSICPACK_MANIFEST_MAX_TOTAL_BYTES) {
                snprintf(buf, sizeof buf,
                         "%s: aggregate referenced bytes exceed %llu-byte limit",
                         kind,
                         (unsigned long long) MUSICPACK_MANIFEST_MAX_TOTAL_BYTES);
                report(rep, fn, ctx, buf, 1);
                *failed = 1;
                continue;
            }
        }
        if (a->sha256 != 0) {
            char hex[MUSICPACK_SHA256_HEX_SIZE];
            if (musicpack_sha256_file(abs, hex, sizeof hex) != MUSICPACK_OK) {
                snprintf(buf, sizeof buf, "%s: cannot hash '%s'", kind, a->path);
                report(rep, fn, ctx, buf, 1);
                *failed = 1;
            } else if (!musicpack_sha256_eq(hex, a->sha256)) {
                snprintf(buf, sizeof buf, "%s: checksum mismatch '%s'", kind, a->path);
                report(rep, fn, ctx, buf, 1);
                *failed = 1;
            }
        }
    }
}

#if !defined(_WIN32)
static void
walk_dir(const char *abs_base, const char *rel_base, char ***files, size_t *count,
         size_t *cap)
{
    DIR *d;
    struct dirent *e;
    char abspath[MUSICPACK_PATH_MAX + 2];

    d = opendir(abs_base);
    if (d == 0)
        return;
    while ((e = readdir(d)) != 0) {
        struct stat st;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (snprintf(abspath, sizeof abspath, "%s/%s", abs_base, e->d_name)
                >= (int) sizeof abspath)
            continue;
        if (lstat(abspath, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            char relnext[MUSICPACK_PATH_MAX + 2];
            if (rel_base[0] == '\0')
                snprintf(relnext, sizeof relnext, "%s", e->d_name);
            else
                snprintf(relnext, sizeof relnext, "%s/%s", rel_base, e->d_name);
            walk_dir(abspath, relnext, files, count, cap);
        } else if (S_ISREG(st.st_mode)) {
            char *rel;
            if (*count >= *cap) {
                size_t newcap = *cap == 0 ? 64 : *cap * 2;
                char **nf = (char **) realloc(*files, newcap * sizeof *nf);
                if (nf == 0)
                    break;
                *files = nf;
                *cap = newcap;
            }
            if (rel_base[0] == '\0')
                rel = strdup(e->d_name);
            else {
                size_t n = snprintf(0, 0, "%s/%s", rel_base, e->d_name);
                rel = (char *) malloc(n + 1);
                if (rel != 0)
                    snprintf(rel, n + 1, "%s/%s", rel_base, e->d_name);
            }
            if (rel != 0)
                (*files)[(*count)++] = rel;
        }
    }
    closedir(d);
}
#endif

static void
verify_extra_files(const musicpack_package *pkg, musicpack_report *rep,
                   musicpack_report_fn fn, void *ctx)
{
#if !defined(_WIN32)
    char **files = 0;
    size_t count = 0, cap = 0, i;
    const musicpack_manifest *m = pkg->manifest;

    walk_dir(pkg->root, "", &files, &count, &cap);
    for (i = 0; i < count; i++) {
        int referenced = strcmp(files[i], MANIFEST_NAME) == 0;
        size_t d, t, a;
        if (!referenced)
            for (d = 0; d < m->disc_count && !referenced; d++)
                for (t = 0; t < m->discs[d].track_count && !referenced; t++)
                    if (strcmp(m->discs[d].tracks[t].audio.path, files[i]) == 0)
                        referenced = 1;
        if (!referenced)
            for (a = 0; a < m->artwork_count && !referenced; a++)
                if (strcmp(m->artwork[a].asset.path, files[i]) == 0)
                    referenced = 1;
        if (!referenced)
            for (a = 0; a < m->booklet_count && !referenced; a++)
                if (strcmp(m->booklet[a].path, files[i]) == 0)
                    referenced = 1;
        if (!referenced)
            for (a = 0; a < m->lyrics_count && !referenced; a++)
                if (strcmp(m->lyrics[a].path, files[i]) == 0)
                    referenced = 1;
        if (!referenced)
            for (a = 0; a < m->extras_count && !referenced; a++)
                if (strcmp(m->extras[a].path, files[i]) == 0)
                    referenced = 1;
        if (!referenced)
            for (a = 0; a < m->analysis_count && !referenced; a++)
                if (strcmp(m->analysis[a].asset.path, files[i]) == 0)
                    referenced = 1;
        if (!referenced)
            for (d = 0; d < m->disc_count && !referenced; d++)
                for (t = 0; t < m->discs[d].track_count && !referenced; t++)
                    if (m->discs[d].tracks[t].waveform.present &&
                        strcmp(m->discs[d].tracks[t].waveform.path, files[i]) == 0)
                        referenced = 1;

        if (!referenced) {
            char buf[512];
            snprintf(buf, sizeof buf, "unreferenced file '%s'", files[i]);
            report(rep, fn, ctx, buf, 0);
        }
        free(files[i]);
    }
    free(files);
#endif
}

/* Validates every referenced `sonic` analysis document: parses it, checks
   the manifest's profile reference matches, and validates semantics against
   the package manifest. Unknown/research-only profiles are warnings. */
static void
verify_sonic_documents(const musicpack_package *pkg, musicpack_report *rep,
                        musicpack_report_fn fn, void *ctx, int *failed)
{
    const musicpack_manifest *m = pkg->manifest;
    size_t i;

    for (i = 0; i < m->analysis_count; i++) {
        const musicpack_analysis *a = &m->analysis[i];
        char abs[MUSICPACK_PATH_MAX + 2];
        char buf[512];
        char *json;
        musicpack_status s;
        musicpack_sonic *sonic;
        musicpack_sonic_profile_state state;
        long long size;
        size_t json_len;

        if (strcmp(a->type, "sonic") != 0)
            continue;
        if (musicpack_package_resolve_path(pkg, a->asset.path, abs, sizeof abs) != MUSICPACK_OK)
            continue; /* already reported by verify_assets */

        size = checked_file_size(abs);
        if (size > (long long) MUSICPACK_SONIC_DOC_MAX) {
            snprintf(buf, sizeof buf,
                     "analysis: sonic document '%s' exceeds %u-byte limit",
                     a->asset.path, (unsigned int) MUSICPACK_SONIC_DOC_MAX);
            report(rep, fn, ctx, buf, 1);
            *failed = 1;
            continue;
        }
        json = read_file(abs, MUSICPACK_SONIC_DOC_MAX, &json_len, &s);
        if (json == 0) {
            snprintf(buf, sizeof buf, "analysis: cannot read sonic document '%s'",
                     a->asset.path);
            report(rep, fn, ctx, buf, 1);
            *failed = 1;
            continue;
        }

        if (memchr(json, '\0', json_len) != 0)
            sonic = 0;
        else
            sonic = musicpack_sonic_parse(json, json_len, &s);
        free(json);
        if (sonic == 0) {
            snprintf(buf, sizeof buf, "analysis: malformed sonic document '%s'",
                     a->asset.path);
            report(rep, fn, ctx, buf, 1);
            *failed = 1;
            continue;
        }
        if (a->profile != 0 && strcmp(a->profile, sonic->profile_id) != 0) {
            snprintf(buf, sizeof buf,
                     "analysis: sonic profile mismatch (document '%s', manifest '%s')",
                     sonic->profile_id, a->profile);
            report(rep, fn, ctx, buf, 1);
            *failed = 1;
            musicpack_sonic_free(sonic);
            continue;
        }
        s = musicpack_sonic_validate(sonic, m, &state);
        if (s != MUSICPACK_OK) {
            snprintf(buf, sizeof buf,
                     "analysis: sonic document '%s' fails validation", a->asset.path);
            report(rep, fn, ctx, buf, 1);
            *failed = 1;
        } else if (state == MUSICPACK_SONIC_PROFILE_UNKNOWN) {
            snprintf(buf, sizeof buf,
                     "analysis: sonic profile '%s' is unknown; validated structurally only",
                     sonic->profile_id);
            report(rep, fn, ctx, buf, 0);
        } else if (state == MUSICPACK_SONIC_PROFILE_RESERVED) {
            snprintf(buf, sizeof buf,
                     "analysis: sonic profile '%s' is research-only; vectors not comparable",
                     sonic->profile_id);
            report(rep, fn, ctx, buf, 0);
        }
        musicpack_sonic_free(sonic);
    }
}

/* Per-track waveform verification: same containment + size + checksum rules
   as every other asset, plus payload consistency (`points == bytes/2`,
   closed-enum metadata, byte range) and an optional duration cross-check
   (warning, not error). */
static void
verify_waveform_track(const musicpack_package *pkg, const musicpack_track *tr,
                      musicpack_report *rep, musicpack_report_fn fn, void *ctx,
                      int *failed)
{
    char abs[MUSICPACK_PATH_MAX + 2];
    char buf[512];
    long long size;
    unsigned char *bytes = 0;
    size_t bytes_len = 0;
    musicpack_waveform_meta meta;
    musicpack_status s;
    FILE *f;

    if (!tr->waveform.present)
        return;

    /* Containment + file existence + size budget. Reuse the generic asset
       verifier (path safety, file size limit, hash). */
    {
        musicpack_asset a = { tr->waveform.path, tr->waveform.sha256 };
        verify_assets(pkg, &a, 1, "waveform", rep, fn, ctx, failed, 0);
    }
    if (musicpack_package_resolve_path(pkg, tr->waveform.path, abs, sizeof abs) != MUSICPACK_OK)
        return; /* already reported */

    size = checked_file_size(abs);
    if (size < 0) {
        snprintf(buf, sizeof buf, "waveform: cannot size '%s'", tr->waveform.path);
        report(rep, fn, ctx, buf, 1);
        *failed = 1;
        return;
    }
    if ((unsigned long long) size != (unsigned long long) tr->waveform.points * 2ULL) {
        snprintf(buf, sizeof buf,
                 "waveform: '%s' payload size inconsistent with points (%lld vs %lu)",
                 tr->waveform.path, size,
                 (unsigned long) (tr->waveform.points * 2u));
        report(rep, fn, ctx, buf, 1);
        *failed = 1;
        return;
    }

    /* Read + structural validate the payload. */
    if (size > (long long) MUSICPACK_WAVEFORM_MAX_BYTES) {
        snprintf(buf, sizeof buf, "waveform: '%s' exceeds per-track payload limit",
                 tr->waveform.path);
        report(rep, fn, ctx, buf, 1);
        *failed = 1;
        return;
    }
    f = fopen(abs, "rb");
    if (f == 0) {
        snprintf(buf, sizeof buf, "waveform: cannot open '%s'", tr->waveform.path);
        report(rep, fn, ctx, buf, 1);
        *failed = 1;
        return;
    }
    bytes_len = (size_t) size;
    bytes = (unsigned char *) malloc(bytes_len > 0 ? bytes_len : 1);
    if (bytes == 0) {
        fclose(f);
        report(rep, fn, ctx, "waveform: out of memory", 1);
        *failed = 1;
        return;
    }
    if (bytes_len > 0 && fread(bytes, 1, bytes_len, f) != bytes_len) {
        free(bytes);
        fclose(f);
        snprintf(buf, sizeof buf, "waveform: cannot read '%s'", tr->waveform.path);
        report(rep, fn, ctx, buf, 1);
        *failed = 1;
        return;
    }
    fclose(f);

    meta.version = (uint32_t) tr->waveform.version;
    meta.interval_ms = (uint32_t) tr->waveform.interval_ms;
    meta.floor_db = (int32_t) tr->waveform.floor_db;
    meta.points = (uint32_t) tr->waveform.points;
    s = musicpack_waveform_validate(bytes, bytes_len, &meta);
    free(bytes);
    bytes = 0;
    if (s != MUSICPACK_OK) {
        snprintf(buf, sizeof buf, "waveform: '%s' payload validation failed",
                 tr->waveform.path);
        report(rep, fn, ctx, buf, 1);
        *failed = 1;
        return;
    }

    /* Duration cross-check is a warning, not an error. */
    if (tr->has_duration) {
        long expected = (long) (tr->duration * 10.0 + 0.5);
        long actual = (long) tr->waveform.points;
        long diff = expected - actual;
        if (diff < 0) diff = -diff;
        if (diff > 2) {
            snprintf(buf, sizeof buf,
                     "waveform: '%s' points=%lu differs from duration=%g (%ld buckets)",
                     tr->waveform.path, (unsigned long) tr->waveform.points,
                     tr->duration, diff);
            report(rep, fn, ctx, buf, 0);
        }
    }
}

musicpack_status
musicpack_package_verify(const musicpack_package *pkg, musicpack_report *rep,
                          musicpack_report_fn fn, void *ctx)
{
    const musicpack_manifest *m;
    musicpack_report local = { 0, 0 };
    verify_budget budget;
    musicpack_status manifest_status;
    char *validated_json = 0;
    int failed = 0;
    size_t d, t, i;

    if (pkg == 0)
        return MUSICPACK_ERR_INVALID;
    if (rep == 0)
        rep = &local;

    m = pkg->manifest;
    manifest_status = manifest_shape_safe(m) ?
        musicpack_manifest_write(m, &validated_json) : MUSICPACK_ERR_INVALID;
    free(validated_json);
    if (manifest_status != MUSICPACK_OK) {
        report(rep, fn, ctx, "manifest: mutable manifest fails validation", 1);
        return manifest_status;
    }

    verify_budget_init(&budget);
    for (d = 0; d < m->disc_count; d++) {
        for (t = 0; t < m->discs[d].track_count; t++) {
            musicpack_track *tr = &m->discs[d].tracks[t];
            verify_assets(pkg, &tr->audio, 1, "track", rep, fn, ctx, &failed,
                          &budget);
            verify_waveform_track(pkg, tr, rep, fn, ctx, &failed);
        }
    }
    for (i = 0; i < m->artwork_count; i++)
        verify_assets(pkg, &m->artwork[i].asset, 1, "artwork", rep, fn, ctx,
                      &failed, &budget);
    verify_assets(pkg, m->booklet, m->booklet_count, "booklet", rep, fn, ctx,
                  &failed, &budget);
    verify_assets(pkg, m->lyrics, m->lyrics_count, "lyrics", rep, fn, ctx,
                  &failed, &budget);
    verify_assets(pkg, m->extras, m->extras_count, "extras", rep, fn, ctx,
                  &failed, &budget);
    for (i = 0; i < m->analysis_count; i++)
        verify_assets(pkg, &m->analysis[i].asset, 1, "analysis", rep, fn, ctx,
                      &failed, &budget);

    verify_sonic_documents(pkg, rep, fn, ctx, &failed);

    verify_extra_files(pkg, rep, fn, ctx);

    verify_budget_free(&budget);
    return failed ? MUSICPACK_ERR_CHECKSUM : MUSICPACK_OK;
}

musicpack_status
musicpack_package_save_manifest(const musicpack_package *pkg)
{
    char *json = 0;
    char path[MUSICPACK_PATH_MAX + 2];
    musicpack_status s;

    if (pkg == 0)
        return MUSICPACK_ERR_INVALID;
    s = musicpack_manifest_write_with_original(pkg->manifest, pkg->original, &json);
    if (s != MUSICPACK_OK)
        return s;
    if (snprintf(path, sizeof path, "%s/%s", pkg->root, MANIFEST_NAME)
            >= (int) sizeof path) {
        free(json);
        return MUSICPACK_ERR_PATH;
    }
    s = write_file(path, json);
    free(json);
    return s;
}
