/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

  * Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the following
  disclaimer in the documentation and/or other materials provided
  with the distribution.

  * Neither the name of the The MusicPack Development Team nor the
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
/// \file synth_filter_simd.c
/// SIMD128 synthesis filter (ARM64 NEON / x86-64 SSE2 / wasm SIMD128) via
/// the shared mpc_simd.h abstraction.
///
/// Two kernels:
///   - matrixing FIR: lane = output index; four outputs processed in
///     parallel with the taps of each lane accumulated in the same order as
///     the scalar reference (no horizontal reduction). Coeffs come from the
///     transposed DiT table so each lane reads 4 contiguous V samples and 4
///     contiguous coefficients.
///   - mpc_compute_new_V: the 32-point transform's two 16-value branches
///     (sum and scaled-difference) each go through the same 4-stage
///     butterfly, kept as 4 f32x4 groups.
///
/// Scalar behavior (and the pV permutation) is preserved; small rounding
/// differences from FP contraction are allowed and covered by the fixture
/// tolerance and the scalar-vs-SIMD differential test.
#include <string.h>
#include <mpc/mpcdec.h>
#include "decoder.h"
#include "mpc_simd.h"

#ifdef MPC_FIXED_POINT
#error "synth_filter_simd.c requires float samples (MPC_FIXED_POINT undefined)"
#endif

// Scale constants shared by both butterfly branches (float mode: every
// MPC_SCALE_CONST* / MPC_MULTIPLY_FRACT_CONST* collapses to plain multiply).
static const float BF_K1A[4] = {
    0.5024192929f, 0.5224986076f, 0.5669440627f, 0.6468217969f
};
static const float BF_K1B[4] = {
    0.7881546021f, 1.0606776476f, 1.7224471569f, 5.1011486053f
};
static const float BF_K2[4] = {
    0.5097956061f, 0.6013448834f, 0.8999761939f, 2.5629155636f
};
static const float BF_K3[4] = {
    0.5411961079f, 1.3065630198f, 0.5411961079f, 1.3065630198f
};
static const float BF_K4 = 0.7071067691f;

// First-stage scaling for the scaled-difference branch: tw[i] = 1/cos-ish
// constants (see the scalar A00..A15 block). Sixteen values, one per pair.
static const float BF_TW[16] = {
    0.5006030202f, 0.5054709315f, 0.5154473186f, 0.5310425758f,
    0.5531039238f, 0.5829349756f, 0.6225041151f, 0.6748083234f,
    0.7445362806f, 0.8393496275f, 0.9725682139f, 1.1694399118f,
    1.4841645956f, 2.0577809811f, 3.4076085091f, 10.1900081635f
};

/// Four-stage forward butterfly on 16 values arranged as 4 groups (each a
/// f32x4 of four consecutive values). Both the sum and scaled-difference
/// branches of the transform use this identical network.
static void
mpc_bf16(mpc_f32x4 g[4], mpc_f32x4 out[4])
{
    mpc_f32x4 b[4], a[4];
    int i;

    // stage 1: 8->16 butterfly (sums and scaled differences across halves)
    b[0] = mpc_simd_add(g[0], mpc_simd_rev4(g[3]));
    b[1] = mpc_simd_add(g[1], mpc_simd_rev4(g[2]));
    b[2] = mpc_simd_mul(mpc_simd_sub(g[0], mpc_simd_rev4(g[3])), mpc_simd_loadu(BF_K1A));
    b[3] = mpc_simd_mul(mpc_simd_sub(g[1], mpc_simd_rev4(g[2])), mpc_simd_loadu(BF_K1B));

    // stage 2: 8->8 butterfly over the two halves
    a[0] = mpc_simd_add(b[0], mpc_simd_rev4(b[1]));
    a[1] = mpc_simd_mul(mpc_simd_sub(b[0], mpc_simd_rev4(b[1])), mpc_simd_loadu(BF_K2));
    a[2] = mpc_simd_add(b[2], mpc_simd_rev4(b[3]));
    a[3] = mpc_simd_mul(mpc_simd_sub(b[2], mpc_simd_rev4(b[3])), mpc_simd_loadu(BF_K2));

    // stage 3: per-group 2x2 -> (sum,sum',scaled diff,scaled diff')
    for (i = 0; i < 4; i++) {
        mpc_f32x4 p = mpc_simd_add(a[i], mpc_simd_rev4(a[i]));
        mpc_f32x4 q = mpc_simd_mul(mpc_simd_sub(a[i], mpc_simd_rev4(a[i])), mpc_simd_loadu(BF_K3));
        b[i] = mpc_simd_blend_lo_lo(p, q);
    }

    // stage 4: per-group adjacent pair sum / 0.7071-scaled difference
    for (i = 0; i < 4; i++) {
        mpc_f32x4 sw = mpc_simd_swap_pairs(b[i]);
        mpc_f32x4 p = mpc_simd_add(b[i], sw);
        mpc_f32x4 q = mpc_simd_mul(mpc_simd_sub(b[i], sw), mpc_simd_set1(BF_K4));
        out[i] = mpc_simd_blend_x(p, q);
    }
}

/// Replicate the scalar pV permutation for both transform branches.
static void
mpc_compute_new_V_store(MPC_SAMPLE_FORMAT *pV,
                        const MPC_SAMPLE_FORMAT S[16],
                        const MPC_SAMPLE_FORMAT D[16])
{
    MPC_SAMPLE_FORMAT tmp;

    // sum branch (scalar lines: pV[48]..pV[42])
    pV[48] = -S[ 0];
    pV[ 0] =  S[ 1];
    pV[40] = -S[ 2] - (pV[ 8] = S[ 3]);
    pV[36] = -((pV[ 4] = S[ 5] + (pV[12] = S[ 7])) + S[ 6]);
    pV[44] = - S[ 4] - S[ 6] - S[ 7];
    pV[ 6] = (pV[10] = S[11] + (pV[14] = S[15])) + S[13];
    pV[38] = (pV[34] = -(pV[ 2] = S[ 9] + S[13] + S[15]) - S[14]) + S[ 9] - S[10] - S[11];
    pV[46] = (tmp = -(S[12] + S[14] + S[15])) - S[ 8];
    pV[42] = tmp - S[10] - S[11];

    // scaled-difference branch (scalar lines: pV[5]..pV[45])
    pV[ 5] = (pV[11] = (pV[13] = D[ 7] + (pV[15] = D[15])) + D[11]) + D[ 5] + D[13];
    pV[ 7] = (pV[ 9] = D[ 3] + D[11] + D[15]) + D[13];
    pV[33] = -(pV[ 1] = D[ 1] + D[ 9] + D[13] + D[15]) - D[14];
    pV[35] = -(pV[ 3] = D[ 5] + D[ 7] + D[ 9] + D[13] + D[15]) - D[ 6] - D[14];
    pV[37] = (tmp = -(D[10] + D[11] + D[13] + D[14] + D[15])) - D[ 5] - D[ 6] - D[ 7];
    pV[39] = tmp - D[ 2] - D[ 3];
    pV[41] = (tmp += D[13] - D[12]) - D[ 2] - D[ 3];
    pV[43] = tmp - D[ 4] - D[ 6] - D[ 7];
    pV[47] = (tmp = -(D[ 8] + D[12] + D[14] + D[15])) - D[ 0];
    pV[45] = tmp - D[ 4] - D[ 6] - D[ 7];

    pV[32] = -pV[ 0];
    pV[31] = -pV[ 1];
    pV[30] = -pV[ 2];
    pV[29] = -pV[ 3];
    pV[28] = -pV[ 4];
    pV[27] = -pV[ 5];
    pV[26] = -pV[ 6];
    pV[25] = -pV[ 7];
    pV[24] = -pV[ 8];
    pV[23] = -pV[ 9];
    pV[22] = -pV[10];
    pV[21] = -pV[11];
    pV[20] = -pV[12];
    pV[19] = -pV[13];
    pV[18] = -pV[14];
    pV[17] = -pV[15];

    pV[63] =  pV[33];
    pV[62] =  pV[34];
    pV[61] =  pV[35];
    pV[60] =  pV[36];
    pV[59] =  pV[37];
    pV[58] =  pV[38];
    pV[57] =  pV[39];
    pV[56] =  pV[40];
    pV[55] =  pV[41];
    pV[54] =  pV[42];
    pV[53] =  pV[43];
    pV[52] =  pV[44];
    pV[51] =  pV[45];
    pV[50] =  pV[46];
    pV[49] =  pV[47];
}

static void
mpc_compute_new_V_simd(const MPC_SAMPLE_FORMAT *p_sample, MPC_SAMPLE_FORMAT *pV)
{
    mpc_f32x4 s[4], d[4], sout[4], dout[4];
    MPC_SAMPLE_FORMAT S[16], D[16];
    int i;

    // Build the two 16-value branches:
    //   s[i] = p[i] + p[31-i], d[i] = (p[i] - p[31-i]) * tw[i]
    for (i = 0; i < 4; i++) {
        mpc_f32x4 lo = mpc_simd_loadu(&p_sample[4 * i]);
        mpc_f32x4 hi = mpc_simd_rev4(mpc_simd_loadu(&p_sample[28 - 4 * i]));
        s[i] = mpc_simd_add(lo, hi);
        d[i] = mpc_simd_mul(mpc_simd_sub(lo, hi), mpc_simd_loadu(&BF_TW[4 * i]));
    }

    mpc_bf16(s, sout);
    mpc_bf16(d, dout);

    for (i = 0; i < 4; i++) {
        S[4 * i + 0] = mpc_simd_extract_lane(sout[i], 0);
        S[4 * i + 1] = mpc_simd_extract_lane(sout[i], 1);
        S[4 * i + 2] = mpc_simd_extract_lane(sout[i], 2);
        S[4 * i + 3] = mpc_simd_extract_lane(sout[i], 3);
        D[4 * i + 0] = mpc_simd_extract_lane(dout[i], 0);
        D[4 * i + 1] = mpc_simd_extract_lane(dout[i], 1);
        D[4 * i + 2] = mpc_simd_extract_lane(dout[i], 2);
        D[4 * i + 3] = mpc_simd_extract_lane(dout[i], 3);
    }

    mpc_compute_new_V_store(pV, S, D);
}

/// Matrixing FIR for the 32 outputs of one subframe. pV points at the start
/// of the newly computed V block; taps are V[k + off[m]].
static void
mpc_synth_fir(MPC_SAMPLE_FORMAT *p_out, const MPC_SAMPLE_FORMAT *pV,
              const MPC_SAMPLE_FORMAT (*DiT)[32], mpc_int_t channels)
{
    // 16 tap offsets into the V history (see the scalar pD loop).
    static const int off[16] = {
        0, 96, 128, 224, 256, 352, 384, 480,
        512, 608, 640, 736, 768, 864, 896, 992
    };
    int k, m;

    for (k = 0; k < 32; k += 4) {
        mpc_f32x4 acc = mpc_simd_set1(0.0f);
        // Lane == output index; each lane accumulates its 16 taps in scalar
        // order (no horizontal reduction).
        for (m = 0; m < 16; m++) {
            mpc_f32x4 v = mpc_simd_loadu(&pV[k + off[m]]);
            mpc_f32x4 c = mpc_simd_loadu(&DiT[m][k]);
            acc = mpc_simd_add(acc, mpc_simd_mul(v, c));
        }
        if (channels == 1) {
            mpc_simd_storeu(&p_out[k], acc);
        } else {
            // Interleaved stereo: each output lane goes to p_out[out*ch].
            p_out[(k + 0) * channels] = mpc_simd_extract_lane(acc, 0);
            p_out[(k + 1) * channels] = mpc_simd_extract_lane(acc, 1);
            p_out[(k + 2) * channels] = mpc_simd_extract_lane(acc, 2);
            p_out[(k + 3) * channels] = mpc_simd_extract_lane(acc, 3);
        }
    }
}

static void
mpc_synthese_filter_float_internal_simd(MPC_SAMPLE_FORMAT *p_out,
                                        MPC_SAMPLE_FORMAT *pV,
                                        const MPC_SAMPLE_FORMAT *pY,
                                        mpc_int_t channels,
                                        const MPC_SAMPLE_FORMAT (*DiT)[32])
{
    mpc_uint32_t n;
    for (n = 0; n < 36; n++, pY += 32) {
        pV -= 64;
        mpc_compute_new_V_simd(pY, pV);
        mpc_synth_fir(p_out, pV, DiT, channels);
        p_out += 32 * channels;
    }
}

void
mpc_synthese_filter_float_simd(mpc_decoder *p_dec, MPC_SAMPLE_FORMAT *p_out,
                               mpc_int_t channels)
{
    /********* left channel ********/
    memmove(&p_dec->V_L[MPC_V_MEM], p_dec->V_L, 960 * sizeof *p_dec->V_L);
    mpc_synthese_filter_float_internal_simd(p_out, &p_dec->V_L[MPC_V_MEM],
                                            p_dec->Y_L[0], channels, p_dec->DiT);

    /******** right channel ********/
    if (channels > 1) {
        memmove(&p_dec->V_R[MPC_V_MEM], p_dec->V_R, 960 * sizeof *p_dec->V_R);
        mpc_synthese_filter_float_internal_simd(p_out + 1, &p_dec->V_R[MPC_V_MEM],
                                                p_dec->Y_R[0], channels, p_dec->DiT);
    }
}
