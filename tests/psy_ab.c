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
 * Psychoacoustic scalar-vs-SIMD differential test (Phase 3, white-box).
 *
 * Two levels:
 *  1. Kernel level: runs each spectrum kernel (PowSpec256/1024/2048,
 *     PolarSpec1024) under the forced scalar and forced SIMD implementations
 *     on identical inputs and asserts bit-identical output. This is where the
 *     SIMD kernels (windowing, power, lane-parallel FFT) live.
 *  2. Model level: runs the full Psychoakustisches_Modell under each
 *     implementation from an identical reset state for several frames
 *     (evolving state) and asserts bit-identical SMR output. This is the
 *     "first divergence" tool: it reports the first (stage, index) or
 *     (frame, band) divergence with exact bit patterns.
 *
 * The final acceptance gate remains enc_compat (full MPC SHA-256).
 *
 * Usage: psy_ab   (no arguments; deterministic)
 * Exit:  0 if every checkpoint is bit-identical, 1 otherwise.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <mpc/datatypes.h>

#include "libmpcpsy.h"

// White-box declarations (libmpcpsy internals; mpcenc.h declares them too).
#ifdef FAST_MATH
void Init_FastMath ( void );
#endif
void Init_Psychoakustik ( PsyModel* );
void Init_Psychoakustiktabellen ( PsyModel* );
void SetQualityParams ( PsyModel*, float );
SMRTyp Psychoakustisches_Modell ( PsyModel*, const int, const PCMDataTyp*,
                                  int* TransientL, int* TransientR );

#define FRAMES 8

static int failures = 0;

// Exact bit-pattern comparison (NaN == NaN when the bits match).
static int
same_bits ( float a, float b )
{
    unsigned int ba, bb;
    memcpy ( &ba, &a, 4 );
    memcpy ( &bb, &b, 4 );
    return ba == bb;
}

static void
report_div ( const char* stage, int idx, float a, float b )
{
    unsigned int ba, bb;
    memcpy ( &ba, &a, 4 );
    memcpy ( &bb, &b, 4 );
    printf ( "  first psycho divergence: stage=%s index=%d scalar=0x%08x simd=0x%08x\n",
             stage, idx, ba, bb );
    failures = 1;
}

// Kernel-level comparison: run the dispatch under both impls, compare outputs.
static void
check_kernel ( const char* stage, const float* x,
               void (*run_scalar) (const float*, float*),
               void (*run_simd)   (const float*, float*),
               float* out_a, float* out_b, int n )
{
    mpc_psy_set_impl (MPC_PSY_SCALAR); run_scalar (x, out_a);
    mpc_psy_set_impl (MPC_PSY_SIMD);   run_simd   (x, out_b);
    for ( int i = 0; i < n; i++ ) {
        if ( !same_bits (out_a[i], out_b[i]) ) { report_div (stage, i, out_a[i], out_b[i]); break; }
    }
}

int
main ( void )
{
    PsyModel m;
    PCMDataTyp Main;
    float F_a[1024], F_b[1024];
    float ph_a[512], ph_b[512];
    float x[2048];
    static SMRTyp Sm_a[FRAMES], Sm_b[FRAMES];
    static int trans_a[FRAMES][2][PART_SHORT];
    static int trans_b[FRAMES][2][PART_SHORT];
    int f, i, q;

    if ( !mpc_psy_has_simd () ) {
        fprintf ( stderr, "psy_ab: SIMD kernels are not compiled in\n" );
        return 1;
    }

    // Deterministic input. NOTE: a pure sine-mix can produce FFT bins whose
    // real part is ~0, making fastmath my_atan2's table index go out of
    // range (a pre-existing latent UB in the reference encoder); white noise
    // keeps every bin well inside the table on all platforms.
    {
        unsigned s = 12345u;
        for ( i = 0; i < ANABUFFER; i++ ) {
            s = s * 1664525u + 1013904223u;
            float v = ((float)(s >> 8) / 16777216.0f) * 2.0f - 1.0f;
            Main.L[i] = 0.5f * v;
            Main.R[i] = 0.4f * v;
            Main.M[i] = 0.5f * v;
            Main.S[i] = 0.1f * v;
        }
    }
    for ( i = 0; i < ANABUFFER; i++ )
        x[i] = Main.L[i];

#ifdef FAST_MATH
    Init_FastMath ();
#endif
    memset ( &m, 0, sizeof m );
    SetQualityParams (&m, 6.0f);        // q6 profile
    Init_Psychoakustik (&m);            // FFT/ANS tables + zeroed state
    m.SampleFreq = 44100.f;
    SetQualityParams (&m, 6.0f);        // re-apply after init reset (mirrors mpcenc)
    Init_Psychoakustiktabellen (&m);    // ATH tables with final params
    m.Max_Band = 31;

    // ---- kernel level ----------------------------------------------------
    mpc_psy_set_impl (MPC_PSY_SCALAR);
    check_kernel ("PowSpec256",   x, PowSpec256,  PowSpec256,  F_a, F_b, 128);
    check_kernel ("PowSpec1024",  x, PowSpec1024, PowSpec1024, F_a, F_b, 512);
    check_kernel ("PowSpec2048",  x, PowSpec2048, PowSpec2048, F_a, F_b, 1024);
    // PolarSpec1024 (3-arg): compare erg and phs separately.
    mpc_psy_set_impl (MPC_PSY_SCALAR); PolarSpec1024 (x, F_a, ph_a);
    mpc_psy_set_impl (MPC_PSY_SIMD);   PolarSpec1024 (x, F_b, ph_b);
    for ( i = 0; i < 512; i++ )
        if ( !same_bits (F_a[i], F_b[i]) ) { report_div ("PolarSpec1024.erg", i, F_a[i], F_b[i]); break; }
    for ( i = 0; i < 512; i++ )
        if ( !same_bits (ph_a[i], ph_b[i]) ) { report_div ("PolarSpec1024.phs", i, ph_a[i], ph_b[i]); break; }

    // ---- batch level (lane-parallel FFT) ---------------------------------
    {
        static float w4a[4][128], w4b[4][128];
        static float p2a[2][512],  p2b[2][512];
        static float p4a[2][1024], p4b[2][1024];
        static float pea[2][512],  peb[2][512];
        static float poa[2][512],  pob[2][512];
        int l;

        mpc_psy_set_impl (MPC_PSY_SCALAR);
        PowSpec256_4 (x, x + 576, x, x + 576, w4a[0], w4a[1], w4a[2], w4a[3]);
        PowSpec1024_2 (x, x + 576, p2a[0], p2a[1]);
        PowSpec2048_2 (x, x,       p4a[0], p4a[1]);
        PolarSpec1024_2 (x, x + 576, pea[0], pea[1], poa[0], poa[1]);
        mpc_psy_set_impl (MPC_PSY_SIMD);
        PowSpec256_4 (x, x + 576, x, x + 576, w4b[0], w4b[1], w4b[2], w4b[3]);
        PowSpec1024_2 (x, x + 576, p2b[0], p2b[1]);
        PowSpec2048_2 (x, x,       p4b[0], p4b[1]);
        PolarSpec1024_2 (x, x + 576, peb[0], peb[1], pob[0], pob[1]);

        for ( l = 0; l < 4 && !failures; l++ )
            for ( i = 0; i < 128 && !failures; i++ )
                if ( !same_bits (w4a[l][i], w4b[l][i]) ) report_div ("PowSpec256_4", l * 128 + i, w4a[l][i], w4b[l][i]);
        for ( l = 0; l < 2 && !failures; l++ )
            for ( i = 0; i < 512 && !failures; i++ ) {
                if ( !same_bits (p2a[l][i], p2b[l][i]) ) report_div ("PowSpec1024_2", l * 512 + i, p2a[l][i], p2b[l][i]);
                if ( !same_bits (pea[l][i], peb[l][i]) ) report_div ("PolarSpec1024_2.e", l * 512 + i, pea[l][i], peb[l][i]);
                if ( !same_bits (poa[l][i], pob[l][i]) ) report_div ("PolarSpec1024_2.p", l * 512 + i, poa[l][i], pob[l][i]);
            }
        for ( l = 0; l < 2 && !failures; l++ )
            for ( i = 0; i < 1024 && !failures; i++ )
                if ( !same_bits (p4a[l][i], p4b[l][i]) ) report_div ("PowSpec2048_2", l * 1024 + i, p4a[l][i], p4b[l][i]);
    }

    // ---- cepstrum batch level (Phase 4: CVD cepstrum FFT) ------------------
    {
        static float cepL_a[4096], cepR_a[4096];
        static float cepL_b[4096], cepR_b[4096];
        int n;

        // Build a log-spectrum-style input as CVD2048_prepare would, then
        // zero the tail (cep[512..1024]). Any deterministic input proves the
        // scalar/SIMD identity; this mirrors the CVD input shape.
        for ( n = 0; n < 512; n++ ) {
            cepL_a[n] = cepL_b[n] = 0.5f + 0.01f * x[n];
            cepR_a[n] = cepR_b[n] = 0.25f + 0.02f * x[n + 1];
        }
        memset ( cepL_a + 512, 0, 513 * sizeof *cepL_a );
        memset ( cepR_a + 512, 0, 513 * sizeof *cepR_a );
        memset ( cepL_b + 512, 0, 513 * sizeof *cepL_b );
        memset ( cepR_b + 512, 0, 513 * sizeof *cepR_b );

        mpc_psy_set_impl ( MPC_PSY_SCALAR );
        Cepstrum2048_2 ( cepL_a, cepR_a, MAX_ANALYZED_IDX );
        mpc_psy_set_impl ( MPC_PSY_SIMD );
        Cepstrum2048_2 ( cepL_b, cepR_b, MAX_ANALYZED_IDX );

        for ( n = 0; n <= MAX_ANALYZED_IDX && !failures; n++ ) {
            if ( !same_bits (cepL_a[n], cepL_b[n]) )
                report_div ("Cepstrum2048_2.L", n, cepL_a[n], cepL_b[n]);
            if ( !same_bits (cepR_a[n], cepR_b[n]) )
                report_div ("Cepstrum2048_2.R", n, cepR_a[n], cepR_b[n]);
        }
    }

    if ( failures ) {
        printf ( "psy_ab: kernel-level divergence\n" );
        return 1;
    }
    printf ( "psy_ab: kernel level bit-identical (PowSpec256/1024/2048, PolarSpec1024+phs, batches, Cepstrum2048_2)\n" );

    // ---- model level (evolving state, q5/q6/q7) ---------------------------
    for ( q = 5; q <= 7 && !failures; q++ ) {
        PsyModel initial;

        memset ( &m, 0, sizeof m );
        SetQualityParams (&m, (float) q);
        Init_Psychoakustik (&m);
        m.SampleFreq = 44100.f;
        SetQualityParams (&m, (float) q);
        Init_Psychoakustiktabellen (&m);
        m.Max_Band = 31;
        initial = m;

        mpc_psy_set_impl (MPC_PSY_SCALAR);
        m = initial;
        memset ( trans_a, 0, sizeof trans_a );
        for ( f = 0; f < FRAMES; f++ )
            Sm_a[f] = Psychoakustisches_Modell (&m, 31, &Main,
                                                trans_a[f][0], trans_a[f][1]);

        mpc_psy_set_impl (MPC_PSY_SIMD);
        m = initial;
        memset ( trans_b, 0, sizeof trans_b );
        for ( f = 0; f < FRAMES; f++ )
            Sm_b[f] = Psychoakustisches_Modell (&m, 31, &Main,
                                                trans_b[f][0], trans_b[f][1]);

        for ( f = 0; f < FRAMES && !failures; f++ ) {
            const float* channels_a[] = { Sm_a[f].L, Sm_a[f].R, Sm_a[f].M, Sm_a[f].S };
            const float* channels_b[] = { Sm_b[f].L, Sm_b[f].R, Sm_b[f].M, Sm_b[f].S };
            const char* names[] = { "SMR.L", "SMR.R", "SMR.M", "SMR.S" };
            int channel;

            for ( channel = 0; channel < 4 && !failures; channel++ )
                for ( i = 0; i < 32; i++ )
                    if ( !same_bits (channels_a[channel][i], channels_b[channel][i]) ) {
                        report_div (names[channel], (q - 5) * FRAMES * 32 + f * 32 + i,
                                    channels_a[channel][i], channels_b[channel][i]);
                        break;
                    }
            for ( channel = 0; channel < 2 && !failures; channel++ )
                for ( i = 0; i < PART_SHORT; i++ )
                    if ( trans_a[f][channel][i] != trans_b[f][channel][i] ) {
                        report_div (channel == 0 ? "Transient.L" : "Transient.R",
                                    (q - 5) * FRAMES * PART_SHORT + f * PART_SHORT + i,
                                    (float) trans_a[f][channel][i], (float) trans_b[f][channel][i]);
                        break;
                    }
        }
    }

    if ( failures ) {
        printf ( "psy_ab: model-level divergence\n" );
        return 1;
    }
    printf ( "psy_ab: model level bit-identical (q5/q6/q7, %d frames, L/R/M/S SMR + transients)\n", FRAMES );
    return 0;
}
