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

/*
 * Scalar-vs-SIMD synthesis filter differential test.
 *
 * Decodes each .mpc fixture twice through the same build — once with the
 * synthesis filter forced to the scalar reference path and once forced to
 * the SIMD path — and compares the decoded float PCM. The decoder has no
 * bit-exact requirement, so the gate is the fixture tolerance: worst-case
 * deviation must stay well under +-2 LSB of 16-bit PCM (2/32768 ~= 6.1e-5).
 *
 * White-box: reaches the inner mpc_decoder via mpc_demux and the
 * mpc_decoder_set_synth_impl hook (internal.h). Only meaningful when the
 * library was built with MPC_ENABLE_SIMD=ON (both kernels compiled in).
 *
 * Usage: synth_ab <fixture.mpc> [...]
 * Exit: 0 if every input is within tolerance, 1 otherwise.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mpc/mpcdec.h>
#include <mpc/reader.h>

#include "decoder.h"
#include "internal.h"

#define TOLERANCE (2.0 / 32768.0)

typedef struct {
    float *pcm;
    size_t samples;
    unsigned rate;
} pcm_t;

static void
decode_with_impl(const char *path, int impl, pcm_t *out)
{
    mpc_reader reader;
    mpc_demux *demux;
    mpc_frame_info frame;
    mpc_streaminfo si;
    float *buf;
    size_t cap = 0, len = 0;
    float tmp[MPC_DECODER_BUFFER_LENGTH];

    if (mpc_reader_init_stdio(&reader, path) != MPC_STATUS_OK) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(2);
    }
    demux = mpc_demux_init(&reader);
    if (demux == 0) {
        fprintf(stderr, "cannot demux %s\n", path);
        exit(2);
    }
    if (!mpc_decoder_set_synth_impl(demux->d, impl)) {
        fprintf(stderr, "cannot select requested synthesis implementation\n");
        exit(2);
    }
    if ((impl == MPC_SYNTH_SCALAR && demux->d->synth != mpc_synthese_filter_float_scalar)
#ifdef MPC_ENABLE_SIMD_KERNEL
        || (impl == MPC_SYNTH_SIMD && demux->d->synth != mpc_synthese_filter_float_simd)
#endif
        ) {
        fprintf(stderr, "requested synthesis implementation was not selected\n");
        exit(2);
    }
    mpc_demux_get_info(demux, &si);

    buf = 0;
    memset(&frame, 0, sizeof frame);
    frame.buffer = tmp;
    for (;;) {
        mpc_demux_decode(demux, &frame);
        if (frame.bits == -1)
            break;
        if (frame.samples == 0)
            continue;
        if (len + (size_t)frame.samples * si.channels > cap) {
            cap = cap ? cap * 2 : 1u << 16;
            float *new_buf = realloc(buf, cap * sizeof *buf);
            if (new_buf == 0) {
                free(buf);
                fprintf(stderr, "out of memory decoding %s\n", path);
                exit(2);
            }
            buf = new_buf;
        }
        memcpy(buf + len, frame.buffer,
               (size_t)frame.samples * si.channels * sizeof *buf);
        len += (size_t)frame.samples * si.channels;
    }

    mpc_demux_exit(demux);
    mpc_reader_exit_stdio(&reader);

    out->pcm = buf;
    out->samples = len;
    out->rate = si.sample_freq;
}

int
main(int argc, char **argv)
{
    int rc = 0;
    int i;

    if (argc < 2) {
        fprintf(stderr, "usage: synth_ab <fixture.mpc> [...]\n");
        return 2;
    }
    if (!mpc_decoder_has_synth_simd()) {
        fprintf(stderr, "synth_ab: SIMD kernel is not compiled in\n");
        return 1;
    }

    for (i = 1; i < argc; i++) {
        pcm_t a, b;
        size_t n, changed = 0, over = 0, nonfinite = 0;
        float worst = 0.0f;
        size_t worst_at = 0;

        decode_with_impl(argv[i], MPC_SYNTH_SCALAR, &a);
        decode_with_impl(argv[i], MPC_SYNTH_SIMD, &b);

        n = a.samples < b.samples ? a.samples : b.samples;
        if (a.samples != b.samples) {
            fprintf(stderr, "FAIL %s: frame count differs (%zu vs %zu)\n",
                    argv[i], a.samples, b.samples);
            rc = 1;
        }
        for (size_t j = 0; j < n; j++) {
            if (!isfinite(a.pcm[j]) || !isfinite(b.pcm[j])) {
                fprintf(stderr, "FAIL %s: non-finite PCM at %zu\n", argv[i], j);
                rc = 1;
                nonfinite++;
                continue;
            }
            float d = fabsf(a.pcm[j] - b.pcm[j]);
            if (d > 0.0f) {
                changed++;
                if (d > worst) {
                    worst = d;
                    worst_at = j;
                }
            }
            if (d > TOLERANCE) {
                over++;
            }
        }

        if (nonfinite != 0) {
            printf("FAIL %-28s %zu non-finite sample values\n", argv[i], nonfinite);
        } else if (changed == 0 && a.samples == b.samples) {
            printf("PASS %-28s identical (%zu sample values, worst diff 0)\n",
                    argv[i], n);
        } else if (over == 0 && a.samples == b.samples) {
            printf("PASS %-28s %zu samples differ within tolerance (worst %.3g at %zu)\n",
                   argv[i], changed, worst, worst_at);
        } else if (over != 0) {
            printf("FAIL %-28s %zu samples over tolerance (worst %.3g at %zu, "
                   "%.1f LSB)\n", argv[i], over, worst, worst_at,
                   worst * 32768.0);
            rc = 1;
        }

        free(a.pcm);
        free(b.pcm);
    }

    return rc;
}
