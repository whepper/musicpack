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
 * Four-lane (lane = independent FFT) real FFT, bit-exact.
 *
 * Ooura's rdft (fft4g.c) is transcribed so that four independent real FFTs
 * run in SIMD lanes sharing the twiddle tables. The input is interleaved:
 * A[4*i + l] holds FFT l's value at index i, so a 4-float load at A[4*i] is
 * the same butterfly operand of all four FFTs. Twiddles broadcast. Each lane
 * executes the identical scalar arithmetic sequence (add/mul, no FMA,
 * -ffp-contract=off), so every lane's result is bit-identical to running the
 * scalar rdft on that buffer.
 *
 * The per-FFT index arithmetic, table layout and op order mirror fft4g.c
 * exactly; only the scalar float ops become lane-wise 4-float ops.
 */

#include "mpc_simd.h"
#include "psy_profile.h"

typedef mpc_f32x4 v4;

#define L4(p,i)        mpc_simd_loadu (&(p)[4 * (i)])
#define S4(p,i,v)      mpc_simd_storeu (&(p)[4 * (i)], (v))
#define B4(t,i)        mpc_simd_set1 ((t)[i])

static v4
vset2 ( float x ) { return mpc_simd_set1 (x); }

static void
bitrv2_4 ( const int n, int* ip, float* a )
{
    int    j, j1, k, k1, l, m, m2;
    v4     xr, xi, yr, yi;

    ip[0] = 0;
    l     = n;
    m     = 1;
    while ( (m << 3) < l ) {
        l >>= 1;
        for ( j = 0; j < m; j++ )
            ip[m + j] = ip[j] + l;
        m <<= 1;
    }
    m2 = 2 * m;
    if ( (m << 3) == l ) {
        for ( k = 0; k < m; k++ ) {
            for ( j = 0; j < k; j++ ) {
                j1        = 2 * j + ip[k];
                k1        = 2 * k + ip[j];
                xr        = L4(a, j1);
                xi        = L4(a, j1 + 1);
                yr        = L4(a, k1);
                yi        = L4(a, k1 + 1);
                S4(a, j1, yr);
                S4(a, j1 + 1, yi);
                S4(a, k1, xr);
                S4(a, k1 + 1, xi);
                j1       += m2;
                k1       += 2 * m2;
                xr        = L4(a, j1);
                xi        = L4(a, j1 + 1);
                yr        = L4(a, k1);
                yi        = L4(a, k1 + 1);
                S4(a, j1, yr);
                S4(a, j1 + 1, yi);
                S4(a, k1, xr);
                S4(a, k1 + 1, xi);
                j1       += m2;
                k1       -= m2;
                xr        = L4(a, j1);
                xi        = L4(a, j1 + 1);
                yr        = L4(a, k1);
                yi        = L4(a, k1 + 1);
                S4(a, j1, yr);
                S4(a, j1 + 1, yi);
                S4(a, k1, xr);
                S4(a, k1 + 1, xi);
                j1       += m2;
                k1       += 2 * m2;
                xr        = L4(a, j1);
                xi        = L4(a, j1 + 1);
                yr        = L4(a, k1);
                yi        = L4(a, k1 + 1);
                S4(a, j1, yr);
                S4(a, j1 + 1, yi);
                S4(a, k1, xr);
                S4(a, k1 + 1, xi);
            }
            j1        = 2 * k + m2 + ip[k];
            k1        = j1 + m2;
            xr        = L4(a, j1);
            xi        = L4(a, j1 + 1);
            yr        = L4(a, k1);
            yi        = L4(a, k1 + 1);
            S4(a, j1, yr);
            S4(a, j1 + 1, yi);
            S4(a, k1, xr);
            S4(a, k1 + 1, xi);
        }
    } else {
        for ( k = 1; k < m; k++ ) {
            for ( j = 0; j < k; j++ ) {
                j1        = 2 * j + ip[k];
                k1        = 2 * k + ip[j];
                xr        = L4(a, j1);
                xi        = L4(a, j1 + 1);
                yr        = L4(a, k1);
                yi        = L4(a, k1 + 1);
                S4(a, j1, yr);
                S4(a, j1 + 1, yi);
                S4(a, k1, xr);
                S4(a, k1 + 1, xi);
                j1       += m2;
                k1       += m2;
                xr        = L4(a, j1);
                xi        = L4(a, j1 + 1);
                yr        = L4(a, k1);
                yi        = L4(a, k1 + 1);
                S4(a, j1, yr);
                S4(a, j1 + 1, yi);
                S4(a, k1, xr);
                S4(a, k1 + 1, xi);
            }
        }
    }
}

static void
cft1st_4 ( const int n, float* a, float* w )
{
    int    j, k1;
    v4     wk1r, wk1i, wk2r, wk2i, wk3r, wk3i;
    v4     x0r, x0i, x1r, x1i, x2r, x2i, x3r, x3i;

    x0r   = mpc_simd_add (L4(a, 0), L4(a, 2));
    x0i   = mpc_simd_add (L4(a, 1), L4(a, 3));
    x1r   = mpc_simd_sub (L4(a, 0), L4(a, 2));
    x1i   = mpc_simd_sub (L4(a, 1), L4(a, 3));
    x2r   = mpc_simd_add (L4(a, 4), L4(a, 6));
    x2i   = mpc_simd_add (L4(a, 5), L4(a, 7));
    x3r   = mpc_simd_sub (L4(a, 4), L4(a, 6));
    x3i   = mpc_simd_sub (L4(a, 5), L4(a, 7));
    S4(a, 0, mpc_simd_add (x0r, x2r));
    S4(a, 1, mpc_simd_add (x0i, x2i));
    S4(a, 4, mpc_simd_sub (x0r, x2r));
    S4(a, 5, mpc_simd_sub (x0i, x2i));
    S4(a, 2, mpc_simd_sub (x1r, x3i));
    S4(a, 3, mpc_simd_add (x1i, x3r));
    S4(a, 6, mpc_simd_add (x1r, x3i));
    S4(a, 7, mpc_simd_sub (x1i, x3r));
    wk1r  = B4(w, 2);
    x0r   = mpc_simd_add (L4(a, 8), L4(a, 10));
    x0i   = mpc_simd_add (L4(a, 9), L4(a, 11));
    x1r   = mpc_simd_sub (L4(a, 8), L4(a, 10));
    x1i   = mpc_simd_sub (L4(a, 9), L4(a, 11));
    x2r   = mpc_simd_add (L4(a, 12), L4(a, 14));
    x2i   = mpc_simd_add (L4(a, 13), L4(a, 15));
    x3r   = mpc_simd_sub (L4(a, 12), L4(a, 14));
    x3i   = mpc_simd_sub (L4(a, 13), L4(a, 15));
    S4(a, 8, mpc_simd_add (x0r, x2r));
    S4(a, 9, mpc_simd_add (x0i, x2i));
    S4(a, 12, mpc_simd_sub (x2i, x0i));
    S4(a, 13, mpc_simd_sub (x0r, x2r));
    x0r   = mpc_simd_sub (x1r, x3i);
    x0i   = mpc_simd_add (x1i, x3r);
    S4(a, 10, mpc_simd_mul (wk1r, mpc_simd_sub (x0r, x0i)));
    S4(a, 11, mpc_simd_mul (wk1r, mpc_simd_add (x0r, x0i)));
    x0r   = mpc_simd_add (x3i, x1r);
    x0i   = mpc_simd_sub (x3r, x1i);
    S4(a, 14, mpc_simd_mul (wk1r, mpc_simd_sub (x0i, x0r)));
    S4(a, 15, mpc_simd_mul (wk1r, mpc_simd_add (x0i, x0r)));

    k1 = 0;
    j  = 16;
    do {
        k1       += 2;
        wk2r      = B4(w, k1);
        wk2i      = B4(w, k1 + 1);
        wk1r      = B4(w, 2 * k1);
        wk1i      = B4(w, 2 * k1 + 1);
        wk3r      = mpc_simd_sub (wk1r, mpc_simd_mul (mpc_simd_mul (vset2 (2.0f), wk2i), wk1i));
        wk3i      = mpc_simd_sub (mpc_simd_mul (mpc_simd_mul (vset2 (2.0f), wk2i), wk1r), wk1i);
        x0r       = mpc_simd_add (L4(a, j), L4(a, j + 2));
        x0i       = mpc_simd_add (L4(a, j + 1), L4(a, j + 3));
        x1r       = mpc_simd_sub (L4(a, j), L4(a, j + 2));
        x1i       = mpc_simd_sub (L4(a, j + 1), L4(a, j + 3));
        x2r       = mpc_simd_add (L4(a, j + 4), L4(a, j + 6));
        x2i       = mpc_simd_add (L4(a, j + 5), L4(a, j + 7));
        x3r       = mpc_simd_sub (L4(a, j + 4), L4(a, j + 6));
        x3i       = mpc_simd_sub (L4(a, j + 5), L4(a, j + 7));
        S4(a, j, mpc_simd_add (x0r, x2r));
        S4(a, j + 1, mpc_simd_add (x0i, x2i));
        x0r      = mpc_simd_sub (x0r, x2r);
        x0i      = mpc_simd_sub (x0i, x2i);
        S4(a, j + 4, mpc_simd_sub (mpc_simd_mul (wk2r, x0r), mpc_simd_mul (wk2i, x0i)));
        S4(a, j + 5, mpc_simd_add (mpc_simd_mul (wk2r, x0i), mpc_simd_mul (wk2i, x0r)));
        x0r       = mpc_simd_sub (x1r, x3i);
        x0i       = mpc_simd_add (x1i, x3r);
        S4(a, j + 2, mpc_simd_sub (mpc_simd_mul (wk1r, x0r), mpc_simd_mul (wk1i, x0i)));
        S4(a, j + 3, mpc_simd_add (mpc_simd_mul (wk1r, x0i), mpc_simd_mul (wk1i, x0r)));
        x0r       = mpc_simd_add (x1r, x3i);
        x0i       = mpc_simd_sub (x1i, x3r);
        S4(a, j + 6, mpc_simd_sub (mpc_simd_mul (wk3r, x0r), mpc_simd_mul (wk3i, x0i)));
        S4(a, j + 7, mpc_simd_add (mpc_simd_mul (wk3r, x0i), mpc_simd_mul (wk3i, x0r)));
        wk1r      = B4(w, 2 * k1 + 2);
        wk1i      = B4(w, 2 * k1 + 3);
        wk3r      = mpc_simd_sub (wk1r, mpc_simd_mul (mpc_simd_mul (vset2 (2.0f), wk2r), wk1i));
        wk3i      = mpc_simd_sub (mpc_simd_mul (mpc_simd_mul (vset2 (2.0f), wk2r), wk1r), wk1i);
        x0r       = mpc_simd_add (L4(a, j +  8), L4(a, j + 10));
        x0i       = mpc_simd_add (L4(a, j +  9), L4(a, j + 11));
        x1r       = mpc_simd_sub (L4(a, j +  8), L4(a, j + 10));
        x1i       = mpc_simd_sub (L4(a, j +  9), L4(a, j + 11));
        x2r       = mpc_simd_add (L4(a, j + 12), L4(a, j + 14));
        x2i       = mpc_simd_add (L4(a, j + 13), L4(a, j + 15));
        x3r       = mpc_simd_sub (L4(a, j + 12), L4(a, j + 14));
        x3i       = mpc_simd_sub (L4(a, j + 13), L4(a, j + 15));
        S4(a, j + 8, mpc_simd_add (x0r, x2r));
        S4(a, j + 9, mpc_simd_add (x0i, x2i));
        x0r      = mpc_simd_sub (x0r, x2r);
        x0i      = mpc_simd_sub (x0i, x2i);
        S4(a, j + 12, mpc_simd_sub (mpc_simd_mul (vset2 (-1.0f), mpc_simd_mul (wk2i, x0r)), mpc_simd_mul (wk2r, x0i)));
        S4(a, j + 13, mpc_simd_add (mpc_simd_mul (vset2 (-1.0f), mpc_simd_mul (wk2i, x0i)), mpc_simd_mul (wk2r, x0r)));
        x0r       = mpc_simd_sub (x1r, x3i);
        x0i       = mpc_simd_add (x1i, x3r);
        S4(a, j + 10, mpc_simd_sub (mpc_simd_mul (wk1r, x0r), mpc_simd_mul (wk1i, x0i)));
        S4(a, j + 11, mpc_simd_add (mpc_simd_mul (wk1r, x0i), mpc_simd_mul (wk1i, x0r)));
        x0r       = mpc_simd_add (x1r, x3i);
        x0i       = mpc_simd_sub (x1i, x3r);
        S4(a, j + 14, mpc_simd_sub (mpc_simd_mul (wk3r, x0r), mpc_simd_mul (wk3i, x0i)));
        S4(a, j + 15, mpc_simd_add (mpc_simd_mul (wk3r, x0i), mpc_simd_mul (wk3i, x0r)));
    } while ( j += 16, j < n );
}

static void
cftmdl_4 ( const int n, const int l, float* a, float* w )
{
    int    j, j1, j2, j3, k, k1, m, m2;
    v4     wk1r, wk1i, wk2r, wk2i, wk3r, wk3i;
    v4     x0r, x0i, x1r, x1i, x2r, x2i, x3r, x3i;

    m = l << 2;

    for ( j = 0; j < l; j += 2 ) {
        j1        = j  + l;
        j2        = j1 + l;
        j3        = j2 + l;
        x0r       = mpc_simd_add (L4(a, j), L4(a, j1));
        x0i       = mpc_simd_add (L4(a, j + 1), L4(a, j1 + 1));
        x1r       = mpc_simd_sub (L4(a, j), L4(a, j1));
        x1i       = mpc_simd_sub (L4(a, j + 1), L4(a, j1 + 1));
        x2r       = mpc_simd_add (L4(a, j2), L4(a, j3));
        x2i       = mpc_simd_add (L4(a, j2 + 1), L4(a, j3 + 1));
        x3r       = mpc_simd_sub (L4(a, j2), L4(a, j3));
        x3i       = mpc_simd_sub (L4(a, j2 + 1), L4(a, j3 + 1));
        S4(a, j, mpc_simd_add (x0r, x2r));
        S4(a, j + 1, mpc_simd_add (x0i, x2i));
        S4(a, j2, mpc_simd_sub (x0r, x2r));
        S4(a, j2 + 1, mpc_simd_sub (x0i, x2i));
        S4(a, j1, mpc_simd_sub (x1r, x3i));
        S4(a, j1 + 1, mpc_simd_add (x1i, x3r));
        S4(a, j3, mpc_simd_add (x1r, x3i));
        S4(a, j3 + 1, mpc_simd_sub (x1i, x3r));
    }

    wk1r = B4(w, 2);
    for ( j = m; j < l + m; j += 2 ) {
        j1        = j  + l;
        j2        = j1 + l;
        j3        = j2 + l;
        x0r       = mpc_simd_add (L4(a, j), L4(a, j1));
        x0i       = mpc_simd_add (L4(a, j + 1), L4(a, j1 + 1));
        x1r       = mpc_simd_sub (L4(a, j), L4(a, j1));
        x1i       = mpc_simd_sub (L4(a, j + 1), L4(a, j1 + 1));
        x2r       = mpc_simd_add (L4(a, j2), L4(a, j3));
        x2i       = mpc_simd_add (L4(a, j2 + 1), L4(a, j3 + 1));
        x3r       = mpc_simd_sub (L4(a, j2), L4(a, j3));
        x3i       = mpc_simd_sub (L4(a, j2 + 1), L4(a, j3 + 1));
        S4(a, j, mpc_simd_add (x0r, x2r));
        S4(a, j + 1, mpc_simd_add (x0i, x2i));
        S4(a, j2, mpc_simd_sub (x2i, x0i));
        S4(a, j2 + 1, mpc_simd_sub (x0r, x2r));
        x0r       = mpc_simd_sub (x1r, x3i);
        x0i       = mpc_simd_add (x1i, x3r);
        S4(a, j1, mpc_simd_mul (wk1r, mpc_simd_sub (x0r, x0i)));
        S4(a, j1 + 1, mpc_simd_mul (wk1r, mpc_simd_add (x0r, x0i)));
        x0r       = mpc_simd_add (x3i, x1r);
        x0i       = mpc_simd_sub (x3r, x1i);
        S4(a, j3, mpc_simd_mul (wk1r, mpc_simd_sub (x0i, x0r)));
        S4(a, j3 + 1, mpc_simd_mul (wk1r, mpc_simd_add (x0i, x0r)));
    }

    k1 = 0;
    m2 = 2 * m;
    for ( k = m2; k < n; k += m2 ) {
        k1  += 2;
        wk2r = B4(w, k1);
        wk2i = B4(w, k1 + 1);
        wk1r = B4(w, 2 * k1);
        wk1i = B4(w, 2 * k1 + 1);
        wk3r = mpc_simd_sub (wk1r, mpc_simd_mul (mpc_simd_mul (vset2 (2.0f), wk2i), wk1i));
        wk3i = mpc_simd_sub (mpc_simd_mul (mpc_simd_mul (vset2 (2.0f), wk2i), wk1r), wk1i);
        j    = k;
        do {
            j1        = j  + l;
            j2        = j1 + l;
            j3        = j2 + l;
            x0r       = mpc_simd_add (L4(a, j), L4(a, j1));
            x0i       = mpc_simd_add (L4(a, j + 1), L4(a, j1 + 1));
            x1r       = mpc_simd_sub (L4(a, j), L4(a, j1));
            x1i       = mpc_simd_sub (L4(a, j + 1), L4(a, j1 + 1));
            x2r       = mpc_simd_add (L4(a, j2), L4(a, j3));
            x2i       = mpc_simd_add (L4(a, j2 + 1), L4(a, j3 + 1));
            x3r       = mpc_simd_sub (L4(a, j2), L4(a, j3));
            x3i       = mpc_simd_sub (L4(a, j2 + 1), L4(a, j3 + 1));
            S4(a, j, mpc_simd_add (x0r, x2r));
            S4(a, j + 1, mpc_simd_add (x0i, x2i));
            x0r      = mpc_simd_sub (x0r, x2r);
            x0i      = mpc_simd_sub (x0i, x2i);
            S4(a, j2, mpc_simd_sub (mpc_simd_mul (wk2r, x0r), mpc_simd_mul (wk2i, x0i)));
            S4(a, j2 + 1, mpc_simd_add (mpc_simd_mul (wk2r, x0i), mpc_simd_mul (wk2i, x0r)));
            x0r       = mpc_simd_sub (x1r, x3i);
            x0i       = mpc_simd_add (x1i, x3r);
            S4(a, j1, mpc_simd_sub (mpc_simd_mul (wk1r, x0r), mpc_simd_mul (wk1i, x0i)));
            S4(a, j1 + 1, mpc_simd_add (mpc_simd_mul (wk1r, x0i), mpc_simd_mul (wk1i, x0r)));
            x0r       = mpc_simd_add (x1r, x3i);
            x0i       = mpc_simd_sub (x1i, x3r);
            S4(a, j3, mpc_simd_sub (mpc_simd_mul (wk3r, x0r), mpc_simd_mul (wk3i, x0i)));
            S4(a, j3 + 1, mpc_simd_add (mpc_simd_mul (wk3r, x0i), mpc_simd_mul (wk3i, x0r)));
        } while ( j += 2, j < l + k );

        wk1r = B4(w, 2 * k1 + 2);
        wk1i = B4(w, 2 * k1 + 3);
        wk3r = mpc_simd_sub (wk1r, mpc_simd_mul (mpc_simd_mul (vset2 (2.0f), wk2r), wk1i));
        wk3i = mpc_simd_sub (mpc_simd_mul (mpc_simd_mul (vset2 (2.0f), wk2r), wk1r), wk1i);
        j    = k + m;
        do {
            j1        = j  + l;
            j2        = j1 + l;
            j3        = j2 + l;
            x0r       = mpc_simd_add (L4(a, j), L4(a, j1));
            x0i       = mpc_simd_add (L4(a, j + 1), L4(a, j1 + 1));
            x1r       = mpc_simd_sub (L4(a, j), L4(a, j1));
            x1i       = mpc_simd_sub (L4(a, j + 1), L4(a, j1 + 1));
            x2r       = mpc_simd_add (L4(a, j2), L4(a, j3));
            x2i       = mpc_simd_add (L4(a, j2 + 1), L4(a, j3 + 1));
            x3r       = mpc_simd_sub (L4(a, j2), L4(a, j3));
            x3i       = mpc_simd_sub (L4(a, j2 + 1), L4(a, j3 + 1));
            S4(a, j, mpc_simd_add (x0r, x2r));
            S4(a, j + 1, mpc_simd_add (x0i, x2i));
            x0r      = mpc_simd_sub (x0r, x2r);
            x0i      = mpc_simd_sub (x0i, x2i);
            S4(a, j2, mpc_simd_sub (mpc_simd_mul (vset2 (-1.0f), mpc_simd_mul (wk2i, x0r)), mpc_simd_mul (wk2r, x0i)));
            S4(a, j2 + 1, mpc_simd_add (mpc_simd_mul (vset2 (-1.0f), mpc_simd_mul (wk2i, x0i)), mpc_simd_mul (wk2r, x0r)));
            x0r       = mpc_simd_sub (x1r, x3i);
            x0i       = mpc_simd_add (x1i, x3r);
            S4(a, j1, mpc_simd_sub (mpc_simd_mul (wk1r, x0r), mpc_simd_mul (wk1i, x0i)));
            S4(a, j1 + 1, mpc_simd_add (mpc_simd_mul (wk1r, x0i), mpc_simd_mul (wk1i, x0r)));
            x0r       = mpc_simd_add (x1r, x3i);
            x0i       = mpc_simd_sub (x1i, x3r);
            S4(a, j3, mpc_simd_sub (mpc_simd_mul (wk3r, x0r), mpc_simd_mul (wk3i, x0i)));
            S4(a, j3 + 1, mpc_simd_add (mpc_simd_mul (wk3r, x0i), mpc_simd_mul (wk3i, x0r)));
        } while ( j += 2, j < l + k + m );
    }
}

static void
cftfsub_4 ( const int n, float* a, float* w )
{
    int    j, j1, j2, j3, l;
    v4     x0r, x0i, x1r, x1i, x2r, x2i, x3r, x3i;

    l = 2;
    if ( n > 8 ) {
        cft1st_4 ( n, a, w );
        l = 8;
        while ( (l << 2) < n ) {
            cftmdl_4 ( n, l, a, w );
            l <<= 2;
        }
    }
    if ( (l << 2) == n ) {
        j = 0;
        do {
            j1        = j  + l;
            j2        = j1 + l;
            j3        = j2 + l;
            x0r       = mpc_simd_add (L4(a, j), L4(a, j1));
            x0i       = mpc_simd_add (L4(a, j + 1), L4(a, j1 + 1));
            x1r       = mpc_simd_sub (L4(a, j), L4(a, j1));
            x1i       = mpc_simd_sub (L4(a, j + 1), L4(a, j1 + 1));
            x2r       = mpc_simd_add (L4(a, j2), L4(a, j3));
            x2i       = mpc_simd_add (L4(a, j2 + 1), L4(a, j3 + 1));
            x3r       = mpc_simd_sub (L4(a, j2), L4(a, j3));
            x3i       = mpc_simd_sub (L4(a, j2 + 1), L4(a, j3 + 1));
            S4(a, j, mpc_simd_add (x0r, x2r));
            S4(a, j + 1, mpc_simd_add (x0i, x2i));
            S4(a, j2, mpc_simd_sub (x0r, x2r));
            S4(a, j2 + 1, mpc_simd_sub (x0i, x2i));
            S4(a, j1, mpc_simd_sub (x1r, x3i));
            S4(a, j1 + 1, mpc_simd_add (x1i, x3r));
            S4(a, j3, mpc_simd_add (x1r, x3i));
            S4(a, j3 + 1, mpc_simd_sub (x1i, x3r));
        } while ( j += 2, j < l );
    } else {
        j = 0;
        do {
            j1        = j + l;
            x0r       = mpc_simd_sub (L4(a, j), L4(a, j1));
            x0i       = mpc_simd_sub (L4(a, j + 1), L4(a, j1 + 1));
            S4(a, j, mpc_simd_add (L4(a, j), L4(a, j1)));
            S4(a, j + 1, mpc_simd_add (L4(a, j + 1), L4(a, j1 + 1)));
            S4(a, j1, x0r);
            S4(a, j1 + 1, x0i);
        } while ( j += 2, j < l );
    }
}

static void
rftfsub_4 ( const int n, float* a, int nc, float* c )
{
    int    j, k, kk, ks, m;
    v4     wkr, wki, xr, xi, yr, yi;

    m  = n >> 1;
    ks = 2 * nc / m;
    kk = ks;
    j  = 2;
    k  = n;
    do {
        k        -= 2;
        nc       -= ks;
        wkr       = mpc_simd_sub (vset2 (0.5f), B4(c, nc));
        wki       = B4(c, kk);
        xr        = mpc_simd_sub (L4(a, j), L4(a, k));
        xi        = mpc_simd_add (L4(a, j + 1), L4(a, k + 1));
        yr        = mpc_simd_sub (mpc_simd_mul (wkr, xr), mpc_simd_mul (wki, xi));
        yi        = mpc_simd_add (mpc_simd_mul (wkr, xi), mpc_simd_mul (wki, xr));
        S4(a, j, mpc_simd_sub (L4(a, j), yr));
        S4(a, j + 1, mpc_simd_sub (L4(a, j + 1), yi));
        S4(a, k, mpc_simd_add (L4(a, k), yr));
        S4(a, k + 1, mpc_simd_sub (L4(a, k + 1), yi));
        kk       += ks;
    } while ( j += 2, j < m );
}

// Four-lane real FFT. A is interleaved: A[4*i+l] = FFT l's a[i]; n is the
// per-FFT size; ip/w are the shared tables (see fft4g.c rdft).
void
rdft4 ( const int n, float* a, int* ip, float* w )
{
    v4 xi;
#ifdef MPC_ENABLE_PSY_PROFILE
    uint64_t profile_start = mpc_psy_profile_now ();
#endif

    if ( n > 4 ) {
        bitrv2_4  ( n, ip + 2, a );
        cftfsub_4 ( n, a, w );
        rftfsub_4 ( n, a, ip[1], w + ip[0] );
    } else if ( n == 4 ) {
        cftfsub_4 ( n, a, w );
    }
    xi        = mpc_simd_sub (L4(a, 0), L4(a, 1));
    S4(a, 0, mpc_simd_add (L4(a, 0), L4(a, 1)));
    S4(a, 1, xi);
#ifdef MPC_ENABLE_PSY_PROFILE
    mpc_psy_profile_add_fft (mpc_psy_profile_now () - profile_start);
#endif
}
