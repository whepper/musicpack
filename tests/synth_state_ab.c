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

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "decoder.h"
#include "internal.h"

#define FRAMES 8
#define GUARD 16
#define MAX_VALUES (MPC_FRAME_LENGTH * 2)
#define TOLERANCE (2.0f / 32768.0f)

static uint32_t next_u32(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static float sample_value(uint32_t *state)
{
    return ((float)(next_u32(state) >> 8) / 8388608.0f - 1.0f) * 0.75f;
}

static void fill_state(mpc_decoder *decoder, uint32_t seed)
{
    size_t i;

    mpc_decoder_setup(decoder);
    for (i = 0; i < sizeof decoder->V_L / sizeof decoder->V_L[0]; i++) {
        decoder->V_L[i] = sample_value(&seed);
        decoder->V_R[i] = sample_value(&seed);
    }
}

static void fill_subbands(mpc_decoder *a, mpc_decoder *b, uint32_t *seed)
{
    int n, band;

    for (n = 0; n < 36; n++) {
        for (band = 0; band < 32; band++) {
            float left = sample_value(seed);
            float right = sample_value(seed);
            a->Y_L[n][band] = b->Y_L[n][band] = left;
            a->Y_R[n][band] = b->Y_R[n][band] = right;
        }
    }
}

static int compare_values(const float *a, const float *b, size_t count,
                          const char *label, int frame)
{
    size_t i;

    for (i = 0; i < count; i++) {
        float diff;
        if (!isfinite(a[i]) || !isfinite(b[i])) {
            fprintf(stderr, "%s frame %d non-finite at %zu\n", label, frame, i);
            return 1;
        }
        diff = fabsf(a[i] - b[i]);
        if (diff > TOLERANCE) {
            fprintf(stderr, "%s frame %d differs at %zu: %.9g\n",
                    label, frame, i, diff);
            return 1;
        }
    }
    return 0;
}

static int run_case(int channels)
{
    mpc_decoder scalar, simd;
    float scalar_out[MAX_VALUES + 2 * GUARD];
    float simd_out[MAX_VALUES + 2 * GUARD];
    float mono_right_before[MPC_V_MEM + 960];
    uint32_t seed = channels == 1 ? 0x13579bdfu : 0x2468ace0u;
    int frame;

    fill_state(&scalar, seed);
    simd = scalar;
    if (!mpc_decoder_set_synth_impl(&scalar, MPC_SYNTH_SCALAR) ||
        !mpc_decoder_set_synth_impl(&simd, MPC_SYNTH_SIMD) ||
        scalar.synth != mpc_synthese_filter_float_scalar ||
        simd.synth != mpc_synthese_filter_float_simd ||
        scalar.synth == simd.synth) {
        fprintf(stderr, "synthesis implementation identity check failed\n");
        return 1;
    }

    if (channels == 1)
        memcpy(mono_right_before, scalar.V_R, sizeof mono_right_before);

    for (frame = 0; frame < FRAMES; frame++) {
        size_t values = MPC_FRAME_LENGTH * (size_t)channels;
        size_t i;

        fill_subbands(&scalar, &simd, &seed);
        for (i = 0; i < MAX_VALUES + 2 * GUARD; i++)
            scalar_out[i] = simd_out[i] = 12345.25f;

        scalar.synth(&scalar, scalar_out + GUARD, channels);
        simd.synth(&simd, simd_out + GUARD, channels);

        for (i = 0; i < GUARD; i++) {
            if (scalar_out[i] != 12345.25f || simd_out[i] != 12345.25f ||
                scalar_out[GUARD + values + i] != 12345.25f ||
                simd_out[GUARD + values + i] != 12345.25f) {
                fprintf(stderr, "channels=%d frame=%d output canary changed\n",
                        channels, frame);
                return 1;
            }
        }
        if (compare_values(scalar_out + GUARD, simd_out + GUARD, values,
                           channels == 1 ? "mono PCM" : "stereo PCM", frame) ||
            compare_values(scalar.V_L, simd.V_L,
                           sizeof scalar.V_L / sizeof scalar.V_L[0],
                           "V_L", frame) ||
            compare_values(scalar.V_R, simd.V_R,
                           sizeof scalar.V_R / sizeof scalar.V_R[0],
                           "V_R", frame))
            return 1;
    }

    if (channels == 1 &&
        (memcmp(scalar.V_R, mono_right_before, sizeof mono_right_before) != 0 ||
         memcmp(simd.V_R, mono_right_before, sizeof mono_right_before) != 0)) {
        fprintf(stderr, "mono synthesis modified right-channel history\n");
        return 1;
    }
    return 0;
}

int main(void)
{
    if (!mpc_decoder_has_synth_simd()) {
        fprintf(stderr, "synth_state_ab requires SIMD\n");
        return 1;
    }
    if (run_case(1) || run_case(2))
        return 1;
    puts("PASS scalar/SIMD state: mono+stereo, 8 frames, PCM/V histories/canaries");
    return 0;
}
