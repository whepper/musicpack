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
/// \file mpak_wasm.c
/// MPAK-over-HTTP for the browser: exposes musicpack_package_open_range()
/// to the WASM module over a JS-import-backed range source (see
/// wasm/mpak_range_library.js and demo/mpakrange.js). The JS host acquires
/// and validates the complete container over HTTP Range requests
/// asynchronously, then serves the range source synchronously — the MPAK
/// core, its 64 KiB block cache and musicpack_package_open_range() run
/// unchanged. libmusicpack stays network-free: all HTTP behavior lives in
/// the JS host.
///
/// Ownership (identical to the native seam): on a successful open the
/// package adopts the range source — closing the package calls
/// Module.mpakRangeDestroy() exactly once. On a failed open the wrapper
/// destroys the source itself and the JS host drops its references. No JS
/// callback runs after either destroy path.

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <musicpack/musicpack.h>
#include <musicpack/range.h>

#include <musepack/musepack.h>

/* declared in mpak_range_library.js (host-provided, synchronous) */
extern double mpak_range_size(void);
extern int mpak_range_read(double offset, int ptr, int len);
extern void mpak_range_destroy(void);

#define MPAK_WASM_MAX_PACKAGES 4
#define MPAK_WASM_MAX_TRACKS 8
/* JS numbers represent integers exactly up to 2^53; container sizes are
   far below that and the transport rejects anything larger. */
#define MPAK_WASM_JS_SAFE_MAX 9007199254740992.0

typedef struct {
    int used;
    musicpack_package *pkg;
    int source_adopted;
} mpak_wasm_pkg;

typedef struct {
    int used;
    musicpack_package *pkg;      /* weak: outlives the track handle */
    mpc_reader reader;
    musepack_decoder *decoder;
} mpak_wasm_track;

static mpak_wasm_pkg pkgs[MPAK_WASM_MAX_PACKAGES];
static mpak_wasm_track tracks[MPAK_WASM_MAX_TRACKS];

/* ---- musicpack_range_source over the JS imports ---------------------- */

static musicpack_status
wasm_range_size(void *ctx, uint64_t *out)
{
    double v = mpak_range_size();

    (void) ctx;
    if (!(v >= 1.0) || v >= MPAK_WASM_JS_SAFE_MAX)
        return MUSICPACK_ERR_IO;
    *out = (uint64_t) v;
    return MUSICPACK_OK;
}

static musicpack_status
wasm_range_read(void *ctx, uint64_t offset, unsigned char *buf, size_t len)
{
    if (len == 0)
        return MUSICPACK_OK;
    if (len > 0x7fffffff || offset >= MPAK_WASM_JS_SAFE_MAX)
        return MUSICPACK_ERR_IO;
    /* the host copies exactly len bytes into WASM memory at ptr and
       returns len; anything else is a failed exact read */
    if (mpak_range_read((double) offset, (int) (intptr_t) buf, (int) len)
            != (int) len)
        return MUSICPACK_ERR_IO;
    (void) ctx;
    return MUSICPACK_OK;
}

static void
wasm_range_destroy(void *ctx)
{
    (void) ctx;
    mpak_range_destroy();
}

int mpak_wasm_open_range(double size)
{
    musicpack_range_source src;
    musicpack_status s;
    musicpack_package *pkg;
    int i;

    for (i = 0; i < MPAK_WASM_MAX_PACKAGES; i++)
        if (!pkgs[i].used)
            break;
    if (i == MPAK_WASM_MAX_PACKAGES)
        return -1;

    memset(&src, 0, sizeof src);
    src.ctx = 0;
    src.size = wasm_range_size;
    src.read = wasm_range_read;
    src.destroy = wasm_range_destroy;
    /* the JS host validates the size at acquire time; the seam contract
       requires size() to be the authority, so ignore the argument except
       as a sanity gate */
    if (size < 1.0 || size >= MPAK_WASM_JS_SAFE_MAX)
        return -1;

    pkg = musicpack_package_open_range(&src, &s);
    if (pkg == 0) {
        /* failed open: ownership stays with the caller — drop the JS
           source now so no reference outlives the attempt */
        wasm_range_destroy(0);
        return -1;
    }
    memset(&pkgs[i], 0, sizeof pkgs[i]);
    pkgs[i].used = 1;
    pkgs[i].pkg = pkg;
    pkgs[i].source_adopted = 1;   /* close() destroys the JS source */
    return i;
}

static mpak_wasm_pkg *
pkg_get(int h)
{
    if (h < 0 || h >= MPAK_WASM_MAX_PACKAGES || !pkgs[h].used)
        return 0;
    return &pkgs[h];
}

int mpak_wasm_verify(int h)
{
    mpak_wasm_pkg *p = pkg_get(h);

    if (p == 0)
        return MUSEPACK_ERR_INVALID;
    return (int) musicpack_package_verify(p->pkg, 0, 0, 0);
}

int mpak_wasm_track_count(int h)
{
    mpak_wasm_pkg *p = pkg_get(h);
    const musicpack_manifest *m;
    int count = 0;
    size_t d;

    if (p == 0)
        return -1;
    m = musicpack_package_manifest(p->pkg);
    if (m == 0)
        return -1;
    for (d = 0; d < m->disc_count; d++)
        count += (int) m->discs[d].track_count;
    return count;
}

/* Opens track `index` (0-based across discs) as a decoder bound to the
   package's member reader. Returns a track handle for the
   mpak_wasm_track_* exports. */
int mpak_wasm_track_open(int h, int index)
{
    mpak_wasm_pkg *p = pkg_get(h);
    const musicpack_manifest *m;
    musepack_error err = MUSEPACK_OK;
    size_t d, n;
    int i, ti = index;

    if (p == 0 || index < 0)
        return -1;
    m = musicpack_package_manifest(p->pkg);
    if (m == 0)
        return -1;
    for (d = 0; d < m->disc_count; d++) {
        n = m->discs[d].track_count;
        if ((size_t) ti < n) {
            for (i = 0; i < MPAK_WASM_MAX_TRACKS; i++)
                if (!tracks[i].used)
                    break;
            if (i == MPAK_WASM_MAX_TRACKS)
                return -1;
            memset(&tracks[i], 0, sizeof tracks[i]);
            if (musicpack_package_track_open_reader(p->pkg, d,
                                                    (size_t) ti,
                                                    &tracks[i].reader)
                    != MUSICPACK_OK)
                return -1;
            tracks[i].used = 1;
            tracks[i].pkg = p->pkg;
            tracks[i].decoder = musepack_decoder_open(&tracks[i].reader,
                                                      &err);
            if (tracks[i].decoder == 0) {
                musicpack_package_track_close_reader(&tracks[i].reader);
                tracks[i].used = 0;
                return -1;
            }
            return i;
        }
        ti -= (int) n;
    }
    return -1;
}

static mpak_wasm_track *
track_get(int h)
{
    if (h < 0 || h >= MPAK_WASM_MAX_TRACKS || !tracks[h].used ||
        tracks[h].decoder == 0)
        return 0;
    return &tracks[h];
}

/// Decodes up to max_frames sample-frames into ptr (interleaved float).
/// Returns frames written, or a negative musepack_error.
int mpak_wasm_track_read(int h, float *ptr, int max_frames)
{
    mpak_wasm_track *t = track_get(h);
    uint64_t frames = 0;
    musepack_error e;

    if (t == 0 || ptr == 0 || max_frames <= 0)
        return MUSEPACK_ERR_INVALID;
    e = musepack_decoder_read(t->decoder, ptr, (uint64_t) max_frames,
                              &frames);
    if (e == MUSEPACK_OK)
        return (int) frames;
    if (e == MUSEPACK_ERR_EOF)
        return MUSEPACK_ERR_EOF;
    return (int) e;
}

int mpak_wasm_track_seek_sample(int h, double sample)
{
    mpak_wasm_track *t = track_get(h);

    if (t == 0 || sample < 0)
        return MUSEPACK_ERR_INVALID;
    return (int) musepack_decoder_seek_sample(t->decoder,
                                              (uint64_t) sample);
}

double mpak_wasm_track_position(int h)
{
    mpak_wasm_track *t = track_get(h);

    if (t == 0)
        return 0;
    return (double) musepack_decoder_position(t->decoder);
}

int mpak_wasm_track_sample_rate(int h)
{
    mpak_wasm_track *t = track_get(h);
    mpc_streaminfo si;

    if (t == 0 || musepack_decoder_get_info(t->decoder, &si) != MUSEPACK_OK)
        return 0;
    return (int) si.sample_freq;
}

void mpak_wasm_track_destroy(int h)
{
    mpak_wasm_track *t = track_get(h);

    if (t == 0)
        return;
    musepack_decoder_close(t->decoder);
    musicpack_package_track_close_reader(&t->reader);
    memset(t, 0, sizeof *t);
}

/// Closes the package. The adopted range source (opened successfully) is
/// destroyed here: Module.mpakRangeDestroy() runs exactly once.
void mpak_wasm_close(int h)
{
    mpak_wasm_pkg *p = pkg_get(h);
    int i;

    if (p == 0)
        return;
    for (i = 0; i < MPAK_WASM_MAX_TRACKS; i++) {
        if (tracks[i].used && tracks[i].pkg == p->pkg) {
            if (tracks[i].decoder != 0)
                musepack_decoder_close(tracks[i].decoder);
            musicpack_package_track_close_reader(&tracks[i].reader);
            memset(&tracks[i], 0, sizeof tracks[i]);
        }
    }
    musicpack_package_close(p->pkg);   /* adopts + destroys the source */
    memset(p, 0, sizeof *p);
}
