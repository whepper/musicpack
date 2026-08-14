/*
 * Musepack audio compression
 * Copyright (c) 2005-2009, The Musepack Development Team
 * Copyright (C) 1999-2004 Buschmann/Klemm/Piecha/Wolf
 * Copyright (c) 2026, The MusicPack Development Team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Modified by the MusicPack Development Team, 2026.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Bit-exact SIMD128 analysis filterbank kernels (ARM64 NEON / x86-64 SSE2)
 * via the shared common/mpc_simd.h abstraction.
 *
 * Two kernels:
 *   - Vectoring: 32 outputs per call, each a 16-term dot product (8+8 taps).
 *     Lane = output index; each lane accumulates its taps in the exact scalar
 *     order (first product, then sequential adds) with -ffp-contract=off, so
 *     per-lane results are bit-identical to the scalar reference. Coefficient
 *     loads come from the transposed CiC table (built after Klemm()).
 *   - Matrixing: per band a 32-term dot over y. Lane = band index; y is
 *     shared, coefficients from the transposed MT table.
 *
 * No horizontal reductions, no FMA, no reassociation. Scalar kernels in
 * analy_filter.c remain the bit-exact reference and are used for the outputs
 * the vector kernel does not cover (0, 13-15, 16, 29-31).
 */

#include <mpc/mpcmath.h>
#include <mpc/datatypes.h>

#include "libmpcenc.h"
#include "mpc_simd.h"

#ifdef MPC_FIXED_POINT
#error "analy_filter_simd.c requires float samples (MPC_FIXED_POINT undefined)"
#endif

extern float Ci_opt [512];   // post-Klemm order (analyz_filter.c)
extern float M [1024];       // matrixing coefficients (analyz_filter.c)

// Dense transposed tables so 4 consecutive outputs/bands read contiguous
// coefficients. Built once in mpc_enc_simd_init (must run after Klemm()).
static float  CiC [16] [32];   // CiC[j][k]  = Vectoring coefficient, tap j, output k
static int    Xo  [16] [32];   // Xo[j][k]   = Vectoring x index, tap j, output k
static float  MT  [32] [32];   // MT[j][i]   = M[i*32 + j]

void
mpc_enc_simd_init ( void )
{
    static int done = 0;
    int j, k;

    if ( done )
        return;
    done = 1;

    // Derive the per-output tap tables from the post-Klemm Ci_opt layout,
    // mirroring the scalar Vectoring pointer arithmetic exactly.
    for ( k = 0; k < 32; k++ ) {
        for ( j = 0; j < 16; j++ ) {
            int cidx, xo;

            if ( k == 0 ) {
                if ( j < 8 ) { cidx = 128 + j; xo = 31 + 64 * j; }
                else         { cidx = 0;       xo = 0; }
            } else if ( k <= 15 ) {
                if ( j < 8 ) { cidx = 8 * (k - 1) + j;            xo = 16 - k + 64 * j; }
                else         { cidx = 136 + 8 * (k - 1) + j - 8; xo = 31 - k + 64 * (j - 8); }
            } else if ( k == 16 ) {
                if ( j < 8 ) { cidx = 120 + j;      xo = 64 * j; }
                else         { cidx = 256 + j - 8; xo = 32 + 64 * (j - 8); }
            } else {
                if ( j < 8 ) { cidx = 384 + 8 * (k - 17) + j;        xo = 47 + (k - 16) + 64 * j; }
                else         { cidx = 264 + 8 * (k - 17) + j - 8;   xo = 32 + (k - 16) + 64 * (j - 8); }
            }

            CiC [j] [k] = (cidx < 512) ? Ci_opt [cidx] : 0.0f;
            Xo  [j] [k] = xo;
        }
    }

    for ( j = 0; j < 32; j++ )
        for ( k = 0; k < 32; k++ )
            MT [j] [k] = M [k * 32 + j];
}

// Scalar sum for one output from the tables. The scalar's EXPR macro wraps
// each 8-product group in parentheses, so a 16-tap output is
// sum(c1-chain) + sum(c2-chain) — two separately-folded 8-term sums joined
// by one final add (NOT one 16-term chain). Reproduced exactly here.
static float
vectoring_sum ( const float* x, int k, int taps )
{
    int   half = taps == 16 ? 8 : taps;
    float t1 = CiC [0] [k] * x [Xo [0] [k]];
    float t2 = 0.0f;
    int   j;

    for ( j = 1; j < half; j++ )
        t1 = t1 + CiC [j] [k] * x [Xo [j] [k]];
    if ( taps == 16 ) {
        t2 = CiC [half] [k] * x [Xo [half] [k]];
        for ( j = half + 1; j < taps; j++ )
            t2 = t2 + CiC [j] [k] * x [Xo [j] [k]];
        return t1 + t2;
    }
    return t1;
}

// Four consecutive outputs in parallel. dir < 0 : region A (x index falls by
// 1 per output, so the 4 x values load reversed); dir > 0 : region B (rises).
// Two per-lane accumulators (taps 0..7 and 8..15), added at the end to match
// the scalar's EXPR-parenthesized two-sum structure.
static void
vectoring_group ( const float* x, float* y, int k, int dir )
{
    mpc_f32x4 acc1, acc2;
    mpc_f32x4 v;
    int j;

    // First tap of the c1 group initializes its accumulator (matches the
    // scalar's first product, not 0 + product, so signed-zero is identical).
    v = (dir < 0) ? mpc_simd_rev4 (mpc_simd_loadu (&x [Xo [0] [k] - 3]))
                  : mpc_simd_loadu (&x [Xo [0] [k]]);
    acc1 = mpc_simd_mul (v, mpc_simd_loadu (&CiC [0] [k]));
    for ( j = 1; j < 8; j++ ) {
        mpc_f32x4 c = mpc_simd_loadu (&CiC [j] [k]);
        v = (dir < 0) ? mpc_simd_rev4 (mpc_simd_loadu (&x [Xo [j] [k] - 3]))
                      : mpc_simd_loadu (&x [Xo [j] [k]]);
        acc1 = mpc_simd_add (acc1, mpc_simd_mul (c, v));
    }

    // c2 group, independently folded (scalar: EXPR(c2,x2) is a separate sum).
    v = (dir < 0) ? mpc_simd_rev4 (mpc_simd_loadu (&x [Xo [8] [k] - 3]))
                  : mpc_simd_loadu (&x [Xo [8] [k]]);
    acc2 = mpc_simd_mul (v, mpc_simd_loadu (&CiC [8] [k]));
    for ( j = 9; j < 16; j++ ) {
        mpc_f32x4 c = mpc_simd_loadu (&CiC [j] [k]);
        v = (dir < 0) ? mpc_simd_rev4 (mpc_simd_loadu (&x [Xo [j] [k] - 3]))
                      : mpc_simd_loadu (&x [Xo [j] [k]]);
        acc2 = mpc_simd_add (acc2, mpc_simd_mul (c, v));
    }

    mpc_simd_storeu (&y [k], mpc_simd_add (acc1, acc2));
}

void
mpc_vectoring_simd ( const float* x, float* y )
{
    // Output 0: 8 taps, isolated (scalar).
    y [0] = vectoring_sum (x, 0, 8);

    // Outputs 1..12: region A, vectorized in groups of 4.
    for ( int k = 1; k <= 12; k += 4 )
        vectoring_group (x, y, k, -1);

    // Outputs 13..15: scalar tail of region A.
    y [13] = vectoring_sum (x, 13, 16);
    y [14] = vectoring_sum (x, 14, 16);
    y [15] = vectoring_sum (x, 15, 16);

    // Output 16: 16 taps, isolated (scalar).
    y [16] = vectoring_sum (x, 16, 16);

    // Outputs 17..28: region B, vectorized in groups of 4.
    for ( int k = 17; k <= 28; k += 4 )
        vectoring_group (x, y, k, +1);

    // Outputs 29..31: scalar tail of region B.
    y [29] = vectoring_sum (x, 29, 16);
    y [30] = vectoring_sum (x, 30, 16);
    y [31] = vectoring_sum (x, 31, 16);
}

void
mpc_matrixing_simd ( const int MaxBand, const float* mi, const float* y, float* samples )
{
    int i = 0;

    (void) mi;   // coefficients come from the transposed MT table

    // Groups of four bands; each lane is one band with the scalar's 32-term
    // dot order (y[0] first, then j=1..31 ascending).
    for ( ; i + 3 <= MaxBand; i += 4 ) {
        mpc_f32x4 acc = mpc_simd_set1 (y [0]);
        int j;
        for ( j = 1; j < 32; j++ )
            acc = mpc_simd_add (acc, mpc_simd_mul (mpc_simd_loadu (&MT [j] [i]),
                                                   mpc_simd_set1 (y [j])));
        samples [i * 72 + 0]       = mpc_simd_extract_lane (acc, 0);
        samples [(i + 1) * 72 + 0] = mpc_simd_extract_lane (acc, 1);
        samples [(i + 2) * 72 + 0] = mpc_simd_extract_lane (acc, 2);
        samples [(i + 3) * 72 + 0] = mpc_simd_extract_lane (acc, 3);
    }

    // Remaining bands (1..3) scalar, same arithmetic.
    for ( ; i <= MaxBand; i++ ) {
        float t = y [0];
        int j;
        for ( j = 1; j < 32; j++ )
            t = t + MT [j] [i] * y [j];
        samples [i * 72] = t;
    }
}
