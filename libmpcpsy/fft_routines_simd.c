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
