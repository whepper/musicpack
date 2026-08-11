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
void Init_Psychoakustik ( PsyModel* );
void Init_Psychoakustiktabellen ( PsyModel* );
void SetQualityParams ( PsyModel*, float );
SMRTyp Psychoakustisches_Modell ( PsyModel*, const int, const PCMDataTyp*,
                                  int* TransientL, int* TransientR );

#define FRAMES 8

static int failures = 0;

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
        if ( out_a[i] != out_b[i] ) { report_div (stage, i, out_a[i], out_b[i]); break; }
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
    int trans_a[PART_SHORT], trans_b[PART_SHORT];
    int f, i;

    // deterministic input
    for ( i = 0; i < ANABUFFER; i++ ) {
        float v = 0.35f * sinf (0.01745f * i) + 0.2f * sinf (0.06283f * i)
                + 0.05f * sinf (0.31f * i);
        Main.L[i] = v;
        Main.R[i] = 0.8f * v;
        Main.M[i] = v;
        Main.S[i] = 0.2f * v;
    }
    for ( i = 0; i < 2048; i++ )
        x[i] = Main.L[i];

    // ---- kernel level ----------------------------------------------------
    mpc_psy_set_impl (MPC_PSY_SCALAR);
    check_kernel ("PowSpec256",   x, PowSpec256,  PowSpec256,  F_a, F_b, 128);
    check_kernel ("PowSpec1024",  x, PowSpec1024, PowSpec1024, F_a, F_b, 512);
    check_kernel ("PowSpec2048",  x, PowSpec2048, PowSpec2048, F_a, F_b, 1024);
    // PolarSpec1024 (3-arg): compare erg and phs separately.
    mpc_psy_set_impl (MPC_PSY_SCALAR); PolarSpec1024 (x, F_a, ph_a);
    mpc_psy_set_impl (MPC_PSY_SIMD);   PolarSpec1024 (x, F_b, ph_b);
    for ( i = 0; i < 512; i++ )
        if ( F_a[i] != F_b[i] ) { report_div ("PolarSpec1024.erg", i, F_a[i], F_b[i]); break; }
    for ( i = 0; i < 512; i++ )
        if ( ph_a[i] != ph_b[i] ) { report_div ("PolarSpec1024.phs", i, ph_a[i], ph_b[i]); break; }

    if ( failures ) {
        printf ( "psy_ab: kernel-level divergence\n" );
        return 1;
    }
    printf ( "psy_ab: kernel level bit-identical (PowSpec256/1024/2048, PolarSpec1024+phs)\n" );

    // ---- model level (evolving state) ------------------------------------
    memset ( &m, 0, sizeof m );
    SetQualityParams (&m, 6.0f);        // q6 profile
    Init_Psychoakustik (&m);            // FFT/ANS tables + zeroed state
    m.SampleFreq = 44100.f;
    SetQualityParams (&m, 6.0f);        // re-apply after init reset (mirrors mpcenc)
    Init_Psychoakustiktabellen (&m);    // ATH tables with final params
    m.Max_Band = 31;

    mpc_psy_set_impl (MPC_PSY_SCALAR);
    mpc_psy_reset_state (&m);
    for ( f = 0; f < FRAMES; f++ ) {
        memset ( trans_a, 0, sizeof trans_a );
        Sm_a[f] = Psychoakustisches_Modell (&m, 31, &Main, trans_a, trans_a);
    }

    mpc_psy_set_impl (MPC_PSY_SIMD);
    mpc_psy_reset_state (&m);
    for ( f = 0; f < FRAMES; f++ ) {
        memset ( trans_b, 0, sizeof trans_b );
        Sm_b[f] = Psychoakustisches_Modell (&m, 31, &Main, trans_b, trans_b);
    }

    for ( f = 0; f < FRAMES && !failures; f++ ) {
        for ( i = 0; i < 32; i++ ) {
            if ( Sm_a[f].L[i] != Sm_b[f].L[i] ) { report_div ("SMR.L", f*32+i, Sm_a[f].L[i], Sm_b[f].L[i]); break; }
            if ( Sm_a[f].R[i] != Sm_b[f].R[i] ) { report_div ("SMR.R", f*32+i, Sm_a[f].R[i], Sm_b[f].R[i]); break; }
        }
        if ( trans_a[0] != trans_b[0] ) { report_div ("Transient", f, (float)trans_a[0], (float)trans_b[0]); break; }
    }

    if ( failures ) {
        printf ( "psy_ab: model-level divergence\n" );
        return 1;
    }
    printf ( "psy_ab: model level bit-identical (%d frames, SMR + transients)\n", FRAMES );
    return 0;
}
