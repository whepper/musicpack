/*
 * Musepack audio compression
 * Copyright (c) 2026, The MusicPack Development Team
 * All rights reserved. (BSD-2-Clause; see the top-level headers.)
 *
 * Bit-exact SIMD spectrum kernels (libmpcpsy) via common/mpc_simd.h.
 *
 * Stage 1 (Phase 3): the windowing and power loops of PowSpec256/1024/2048
 * and PolarSpec1024 are vectorized lane=bin; the FFT (rdft) stays scalar
 * until the lane-parallel batch kernels land. Lane=bin preserves the exact
 * per-output arithmetic (mul then add, no FMA, -ffp-contract=off), so each
 * output is bit-identical to the scalar reference.
 *
 * Stage 2 (fft4g_simd.c + the *_4/_2 batch kernels) adds the 4-lane
 * lane=independent-FFT.
 */

#include <string.h>

#include <mpc/mpcmath.h>
#include "libmpcpsy.h"
#include "mpc_simd.h"

extern float a [4096];            // FFT working buffer (fft_routines.c)
extern int   ip [4096];           // bit-reverse table (fft_routines.c)
extern float w  [4096];           // twiddle table (fft_routines.c)
extern float Hann_256  [256];
extern float Hann_1024 [1024];
extern float Hann_1600 [1600];
void rdft ( const int n, float* a, int* ip, float* w );

// window a[i] = x[i] * hann[i] for i in [0,n), lane=bin.
static void
window_simd ( const float* x, const float* hann, float* a, int n )
{
    int i;
    for ( i = 0; i + 4 <= n; i += 4 )
        mpc_simd_storeu (&a[i], mpc_simd_mul (mpc_simd_loadu (&x[i]),
                                              mpc_simd_loadu (&hann[i])));
    for ( ; i < n; i++ )
        a[i] = x[i] * hann[i];
}

// erg[i] = a[2i]^2 + a[2i+1]^2 for i in [0,n/2), lane=bin.
static void
power_simd ( const float* a, float* erg, int half )
{
    int i;
    for ( i = 0; i + 4 <= half; i += 4 ) {
        mpc_f32x4 lo = mpc_simd_loadu (&a[2 * i]);
        mpc_f32x4 hi = mpc_simd_loadu (&a[2 * i + 4]);
        mpc_f32x4 re = mpc_simd_even (lo, hi);
        mpc_f32x4 im = mpc_simd_odd  (lo, hi);
        mpc_simd_storeu (&erg[i], mpc_simd_add (mpc_simd_mul (re, re),
                                                mpc_simd_mul (im, im)));
    }
    for ( ; i < half; i++ )
        erg[i] = a[2*i] * a[2*i] + a[2*i+1] * a[2*i+1];
}

void
mpc_powspec256_simd ( const float* x, float* erg )
{
    window_simd (x, Hann_256, a, 256);
    rdft (256, a, ip, w);
    power_simd (a, erg, 128);
}

void
mpc_powspec1024_simd ( const float* x, float* erg )
{
    window_simd (x, Hann_1024, a, 1024);
    rdft (1024, a, ip, w);
    power_simd (a, erg, 512);
}

void
mpc_powspec2048_simd ( const float* x, float* erg )
{
    int i;
    memset (a, 0, 224 * sizeof *a);
    for ( i = 0; i + 4 <= 1600; i += 4 )
        mpc_simd_storeu (&a[i + 224], mpc_simd_mul (mpc_simd_loadu (&x[i]),
                                                    mpc_simd_loadu (&Hann_1600[i])));
    for ( ; i < 1600; i++ )
        a[i + 224] = x[i] * Hann_1600[i];
    memset (a + 1824, 0, 224 * sizeof *a);
    rdft (2048, a, ip, w);
    power_simd (a, erg, 1024);
}

void
mpc_polarspec1024_simd ( const float* x, float* erg, float* phs )
{
    int i;
    window_simd (x, Hann_1024, a, 1024);
    rdft (1024, a, ip, w);
    power_simd (a, erg, 512);
    for ( i = 0; i < 512; i++ )
        phs[i] = ATAN2F (a[2*i + 1], a[2*i]);
}

// ---- lane-parallel batches (lane = independent FFT) -------------------------
// Interleaved working buffer: A[4*i+l] = FFT l's a[i]. rdft4 (fft4g_simd.c)
// runs four independent real FFTs in SIMD lanes, bit-exact per lane.

void rdft4 ( const int n, float* a, int* ip, float* w );

// Interleaved working buffer for the lane-parallel FFT. 16-byte aligned so
// the (mpc_f32x4*) views (power4_split, polarspec1024_2) use aligned vector
// loads/stores on SSE2 (GCC emits movaps for __m128* dereferences).
static _Alignas(16) float A4buf [4 * 2048];

static void
window4 ( const float* x0, const float* x1, const float* x2, const float* x3,
          const float* hann, int n )
{
    int i;
    for ( i = 0; i < n; i++ )
        mpc_simd_storeu (&A4buf[4 * i], mpc_simd_mul (
            mpc_simd_set4 (x0[i], x1[i], x2[i], x3[i]), mpc_simd_set1 (hann[i])));
}

// power + split into nlanes spectra: e_l[i] = re_l^2 + im_l^2 (lane l).
static void
power4_split ( float* e0, float* e1, float* e2, float* e3, int half, int nlanes )
{
    int i;
    for ( i = 0; i < half; i++ ) {
        mpc_f32x4 re = mpc_simd_loadu (&A4buf[4 * (2 * i)]);
        mpc_f32x4 im = mpc_simd_loadu (&A4buf[4 * (2 * i + 1)]);
        mpc_f32x4 e  = mpc_simd_add (mpc_simd_mul (re, re), mpc_simd_mul (im, im));
        if ( nlanes > 0 ) e0[i] = mpc_simd_extract_lane (e, 0);
        if ( nlanes > 1 ) e1[i] = mpc_simd_extract_lane (e, 1);
        if ( nlanes > 2 ) e2[i] = mpc_simd_extract_lane (e, 2);
        if ( nlanes > 3 ) e3[i] = mpc_simd_extract_lane (e, 3);
    }
}

void
mpc_powspec256_4_simd ( const float* x0, const float* x1, const float* x2, const float* x3,
                        float* e0, float* e1, float* e2, float* e3 )
{
    window4 (x0, x1, x2, x3, Hann_256, 256);
    rdft4 (256, A4buf, ip, w);
    power4_split (e0, e1, e2, e3, 128, 4);
}

void
mpc_powspec1024_2_simd ( const float* x0, const float* x1, float* e0, float* e1 )
{
    window4 (x0, x1, x0, x0, Hann_1024, 1024);   // lanes 2,3 unused
    rdft4 (1024, A4buf, ip, w);
    power4_split (e0, e1, 0, 0, 512, 2);
}

void
mpc_powspec2048_2_simd ( const float* x0, const float* x1, float* e0, float* e1 )
{
    int i;
    memset (A4buf, 0, sizeof A4buf);
    for ( i = 0; i < 1600; i++ )
        mpc_simd_storeu (&A4buf[4 * (i + 224)], mpc_simd_mul (
            mpc_simd_set4 (x0[i], x1[i], 0.0f, 0.0f), mpc_simd_set1 (Hann_1600[i])));
    rdft4 (2048, A4buf, ip, w);
    power4_split (e0, e1, 0, 0, 1024, 2);
}

void
mpc_polarspec1024_2_simd ( const float* x0, const float* x1,
                           float* e0, float* e1, float* p0, float* p1 )
{
    int i;
    window4 (x0, x1, x0, x0, Hann_1024, 1024);
    rdft4 (1024, A4buf, ip, w);
    for ( i = 0; i < 512; i++ ) {
        mpc_f32x4 re = mpc_simd_loadu (&A4buf[4 * (2 * i)]);
        mpc_f32x4 im = mpc_simd_loadu (&A4buf[4 * (2 * i + 1)]);
        mpc_f32x4 e  = mpc_simd_add (mpc_simd_mul (re, re), mpc_simd_mul (im, im));
        e0[i] = mpc_simd_extract_lane (e, 0);
        e1[i] = mpc_simd_extract_lane (e, 1);
        p0[i] = ATAN2F (mpc_simd_extract_lane (im, 0), mpc_simd_extract_lane (re, 0));
        p1[i] = ATAN2F (mpc_simd_extract_lane (im, 1), mpc_simd_extract_lane (re, 1));
    }
}
