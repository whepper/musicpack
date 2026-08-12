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
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include <mpc/datatypes.h>

#include "libmpcpsy.h"
extern float a[4096];
extern float Hann_256[256], Hann_1024[1024];
extern float tabatan2[][2], tabcos[][2];
#define a_tab a
#define Hann_1024_tab Hann_1024
#define Hann_256_tab Hann_256

// White-box declarations (libmpcpsy internals; mpcenc.h declares them too).
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

static void
segv_handler ( int sig, siginfo_t* si, void* ctx )
{
    fprintf ( stderr, "SEGV at address %p\n", si->si_addr );
    _Exit (128 + sig);
}

int
main ( void )
{
    struct sigaction sa;
    memset ( &sa, 0, sizeof sa );
    sa.sa_sigaction = segv_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction ( SIGSEGV, &sa, 0 );
    fprintf ( stderr, "tables: a=%p tabatan2=%p tabcos=%p Hann_1024=%p Hann_256=%p\n",
              (void*) a_tab, (void*) tabatan2, (void*) tabcos, (void*) Hann_1024_tab, (void*) Hann_256_tab );
    PsyModel m;
    PCMDataTyp Main;
    float F_a[1024], F_b[1024];
    float ph_a[512], ph_b[512];
    float x[2048];
    static SMRTyp Sm_a[FRAMES], Sm_b[FRAMES];
    int trans_a[PART_SHORT], trans_b[PART_SHORT];
    int f, i;

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

    // ---- kernel level ----------------------------------------------------
    mpc_psy_set_impl (MPC_PSY_SCALAR);
    fprintf (stderr, "c0\n");
    check_kernel ("PowSpec256",   x, PowSpec256,  PowSpec256,  F_a, F_b, 128);
    fprintf (stderr, "c1\n");
    check_kernel ("PowSpec1024",  x, PowSpec1024, PowSpec1024, F_a, F_b, 512);
    fprintf (stderr, "c2\n");
    check_kernel ("PowSpec2048",  x, PowSpec2048, PowSpec2048, F_a, F_b, 1024);
    // PolarSpec1024 (3-arg): compare erg and phs separately.
    fprintf (stderr, "c3\n");
    mpc_psy_set_impl (MPC_PSY_SCALAR); PolarSpec1024 (x, F_a, ph_a);
    fprintf (stderr, "c3a scalar polar done\n");
    mpc_psy_set_impl (MPC_PSY_SIMD);   PolarSpec1024 (x, F_b, ph_b);
    fprintf (stderr, "c3b simd polar done\n");
    for ( i = 0; i < 512; i++ )
        if ( !same_bits (F_a[i], F_b[i]) ) { report_div ("PolarSpec1024.erg", i, F_a[i], F_b[i]); break; }
    for ( i = 0; i < 512; i++ )
        if ( !same_bits (ph_a[i], ph_b[i]) ) { report_div ("PolarSpec1024.phs", i, ph_a[i], ph_b[i]); break; }

    fprintf (stderr, "c4\n");
    // ---- batch level (lane-parallel FFT) ---------------------------------
    {
        static float w4a[4][128], w4b[4][128];
        static float p2a[2][512],  p2b[2][512];
        static float p4a[2][1024], p4b[2][1024];
        static float poa[2][512],  pob[2][512];
        int l;

        fprintf (stderr, "c5\n");
        mpc_psy_set_impl (MPC_PSY_SCALAR);
            PowSpec256_4 (x, x + 576, x, x + 576, w4a[0], w4a[1], w4a[2], w4a[3]);
            PowSpec1024_2 (x, x + 576, p2a[0], p2a[1]);
            PowSpec2048_2 (x, x,       p4a[0], p4a[1]);
            PolarSpec1024_2 (x, x + 576, p2a[0], p2a[1], poa[0], poa[1]);
            fprintf (stderr, "c6\n");
            mpc_psy_set_impl (MPC_PSY_SIMD);
                PowSpec256_4 (x, x + 576, x, x + 576, w4b[0], w4b[1], w4b[2], w4b[3]);
            PowSpec1024_2 (x, x + 576, p2b[0], p2b[1]);
            PowSpec2048_2 (x, x,       p4b[0], p4b[1]);
            PolarSpec1024_2 (x, x + 576, p2b[0], p2b[1], pob[0], pob[1]);

        for ( l = 0; l < 4 && !failures; l++ )
            for ( i = 0; i < 128 && !failures; i++ )
                if ( !same_bits (w4a[l][i], w4b[l][i]) ) report_div ("PowSpec256_4", l * 128 + i, w4a[l][i], w4b[l][i]);
        for ( i = 0; i < 512 && !failures; i++ ) {
            if ( !same_bits (p2a[0][i], p2b[0][i]) ) report_div ("PowSpec1024_2", i, p2a[0][i], p2b[0][i]);
            if ( !same_bits (p4a[0][i], p4b[0][i]) ) report_div ("PowSpec2048_2", i, p4a[0][i], p4b[0][i]);
            if ( !same_bits (poa[0][i], pob[0][i]) ) report_div ("PolarSpec1024_2.p", i, poa[0][i], pob[0][i]);
        }
    }

    if ( failures ) {
        printf ( "psy_ab: kernel-level divergence\n" );
        return 1;
    }
    printf ( "psy_ab: kernel level bit-identical (PowSpec256/1024/2048, PolarSpec1024+phs, batches)\n" );

    fprintf (stderr, "c7\n");
    fprintf (stderr, "c7\n");
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
            if ( !same_bits (Sm_a[f].L[i], Sm_b[f].L[i]) ) { report_div ("SMR.L", f*32+i, Sm_a[f].L[i], Sm_b[f].L[i]); break; }
            if ( !same_bits (Sm_a[f].R[i], Sm_b[f].R[i]) ) { report_div ("SMR.R", f*32+i, Sm_a[f].R[i], Sm_b[f].R[i]); break; }
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
