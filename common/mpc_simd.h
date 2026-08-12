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
/// \file mpc_simd.h
/// Minimal 128-bit float32 SIMD abstraction shared by the decoder SIMD
/// kernels. Maps one logical f32x4 API onto ARM64 NEON, x86-64 SSE2 (the
/// x86-64 baseline; no runtime dispatch) and WebAssembly SIMD128.
///
/// Only the operations the synthesis filter needs are exposed, so the three
/// targets stay structurally identical. Intended for float-synthesis builds
/// only (MPC_SAMPLE_FORMAT is float).
#ifndef _MPC_SIMD_H_
#define _MPC_SIMD_H_
#pragma once

#include <mpc/mpc_types.h>

#if defined(__wasm_simd128__)
# include <wasm_simd128.h>
typedef v128_t mpc_f32x4;
# define MPC_SIMD_WASM 1
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__) || defined(_M_ARM64)
# if defined(_MSC_VER) && defined(_M_ARM64)
#  include <arm64_neon.h>
# else
#  include <arm_neon.h>
# endif
typedef float32x4_t mpc_f32x4;
# define MPC_SIMD_NEON 1
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86) && defined(_M_IX86_FP) && _M_IX86_FP >= 2)
# include <emmintrin.h>
typedef __m128 mpc_f32x4;
# define MPC_SIMD_SSE2 1
#else
# error "mpc_simd.h requires NEON, SSE2 or wasm SIMD128"
#endif

/// Load 4 floats (address may be unaligned).
static mpc_inline mpc_f32x4
mpc_simd_loadu(const float *p)
{
#if defined(MPC_SIMD_WASM)
    return wasm_v128_load(p);
#elif defined(MPC_SIMD_NEON)
    return vld1q_f32(p);
#else
    return _mm_loadu_ps(p);
#endif
}

/// Store 4 floats (address may be unaligned).
static mpc_inline void
mpc_simd_storeu(float *p, mpc_f32x4 v)
{
#if defined(MPC_SIMD_WASM)
    wasm_v128_store(p, v);
#elif defined(MPC_SIMD_NEON)
    vst1q_f32(p, v);
#else
    _mm_storeu_ps(p, v);
#endif
}

/// Broadcast a scalar to all four lanes.
static mpc_inline mpc_f32x4
mpc_simd_set1(float x)
{
#if defined(MPC_SIMD_WASM)
    return wasm_f32x4_splat(x);
#elif defined(MPC_SIMD_NEON)
    return vdupq_n_f32(x);
#else
    return _mm_set1_ps(x);
#endif
}

/// Build a vector from four scalars.
static mpc_inline mpc_f32x4
mpc_simd_set4(float a, float b, float c, float d)
{
#if defined(MPC_SIMD_WASM)
    return wasm_f32x4_make(a, b, c, d);
#elif defined(MPC_SIMD_NEON)
    return (float32x4_t){a, b, c, d};
#else
    return _mm_set_ps(d, c, b, a);
#endif
}

/// Lane-wise addition.
static mpc_inline mpc_f32x4
mpc_simd_add(mpc_f32x4 a, mpc_f32x4 b)
{
#if defined(MPC_SIMD_WASM)
    return wasm_f32x4_add(a, b);
#elif defined(MPC_SIMD_NEON)
    return vaddq_f32(a, b);
#else
    return _mm_add_ps(a, b);
#endif
}

/// Lane-wise subtraction.
static mpc_inline mpc_f32x4
mpc_simd_sub(mpc_f32x4 a, mpc_f32x4 b)
{
#if defined(MPC_SIMD_WASM)
    return wasm_f32x4_sub(a, b);
#elif defined(MPC_SIMD_NEON)
    return vsubq_f32(a, b);
#else
    return _mm_sub_ps(a, b);
#endif
}

/// Lane-wise multiplication.
static mpc_inline mpc_f32x4
mpc_simd_mul(mpc_f32x4 a, mpc_f32x4 b)
{
#if defined(MPC_SIMD_WASM)
    return wasm_f32x4_mul(a, b);
#elif defined(MPC_SIMD_NEON)
    return vmulq_f32(a, b);
#else
    return _mm_mul_ps(a, b);
#endif
}

/// Full 4-lane reversal: (x0,x1,x2,x3) -> (x3,x2,x1,x0).
static mpc_inline mpc_f32x4
mpc_simd_rev4(mpc_f32x4 v)
{
#if defined(MPC_SIMD_WASM)
    return wasm_i32x4_shuffle(v, v, 3, 2, 1, 0);
#elif defined(MPC_SIMD_NEON)
    return vcombine_f32(vget_high_f32(vrev64q_f32(v)),
                        vget_low_f32(vrev64q_f32(v)));
#else
    return _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 1, 2, 3));
#endif
}

/// Swap adjacent lane pairs: (x0,x1,x2,x3) -> (x1,x0,x3,x2).
static mpc_inline mpc_f32x4
mpc_simd_swap_pairs(mpc_f32x4 v)
{
#if defined(MPC_SIMD_WASM)
    return wasm_i32x4_shuffle(v, v, 1, 0, 3, 2);
#elif defined(MPC_SIMD_NEON)
    return vrev64q_f32(v);
#else
    return _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 3, 0, 1));
#endif
}

/// Blend lower halves: (a0,a1,_,_) with (b0,b1,_,_) -> (a0,a1,b0,b1).
static mpc_inline mpc_f32x4
mpc_simd_blend_lo_lo(mpc_f32x4 a, mpc_f32x4 b)
{
#if defined(MPC_SIMD_WASM)
    return wasm_i32x4_shuffle(a, b, 0, 1, 4, 5);
#elif defined(MPC_SIMD_NEON)
    return vcombine_f32(vget_low_f32(a), vget_low_f32(b));
#else
    return _mm_shuffle_ps(_mm_unpacklo_ps(a, b), _mm_unpacklo_ps(a, b),
                          _MM_SHUFFLE(3, 1, 2, 0));
#endif
}

/// Interleaved blend: (a0,a1,a2,a3) with (b0,b1,b2,b3) -> (a0,b0,a2,b2).
static mpc_inline mpc_f32x4
mpc_simd_blend_x(mpc_f32x4 a, mpc_f32x4 b)
{
#if defined(MPC_SIMD_WASM)
    return wasm_i32x4_shuffle(a, b, 0, 4, 2, 6);
#elif defined(MPC_SIMD_NEON)
    return vcombine_f32(vzip_f32(vget_low_f32(a), vget_low_f32(b)).val[0],
                        vzip_f32(vget_high_f32(a), vget_high_f32(b)).val[0]);
#else
    return _mm_shuffle_ps(_mm_unpacklo_ps(a, b), _mm_unpackhi_ps(a, b),
                          _MM_SHUFFLE(1, 0, 1, 0));
#endif
}

/// De-interleave even lanes of two vectors: (a0,a1,a2,a3)+(b0,b1,b2,b3)
/// -> (a0,a2,b0,b2). Used to split interleaved real/imag FFT output.
static mpc_inline mpc_f32x4
mpc_simd_even(mpc_f32x4 a, mpc_f32x4 b)
{
#if defined(MPC_SIMD_WASM)
    return wasm_i32x4_shuffle(a, b, 0, 2, 4, 6);
#elif defined(MPC_SIMD_NEON)
    return vuzp1q_f32(a, b);
#else
    return _mm_shuffle_ps(a, b, _MM_SHUFFLE(2, 0, 2, 0));
#endif
}

/// De-interleave odd lanes of two vectors: (a0,a1,a2,a3)+(b0,b1,b2,b3)
/// -> (a1,a3,b1,b3).
static mpc_inline mpc_f32x4
mpc_simd_odd(mpc_f32x4 a, mpc_f32x4 b)
{
#if defined(MPC_SIMD_WASM)
    return wasm_i32x4_shuffle(a, b, 1, 3, 5, 7);
#elif defined(MPC_SIMD_NEON)
    return vuzp2q_f32(a, b);
#else
    return _mm_shuffle_ps(a, b, _MM_SHUFFLE(3, 1, 3, 1));
#endif
}

/// Extract one lane as a scalar. \p lane must be a compile-time constant
/// (the underlying intrinsics require an immediate).
#if defined(MPC_SIMD_WASM)
# define mpc_simd_extract_lane(v, lane) wasm_f32x4_extract_lane((v), (lane))
#elif defined(MPC_SIMD_NEON)
# define mpc_simd_extract_lane(v, lane) vgetq_lane_f32((v), (lane))
#else
# define mpc_simd_extract_lane(v, lane) \
    _mm_cvtss_f32(_mm_shuffle_ps((v), (v), _MM_SHUFFLE(lane, lane, lane, lane)))
#endif

#endif /* _MPC_SIMD_H_ */
