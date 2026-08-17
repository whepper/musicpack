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
/// \file waveform.h
/// `.mpack` v1 waveform envelope (track-scoped derived package data):
/// quantization formula, streaming accumulator, binary payload
/// encode/decode/validate. See `specs/musicpack-waveform-v1.md`.
#ifndef MUSICPACK_WAVEFORM_H_
#define MUSICPACK_WAVEFORM_H_
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <musicpack/error.h>
#include <musicpack/export.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* format constants (frozen for v1)                                    */
/* ------------------------------------------------------------------ */

#define MUSICPACK_WAVEFORM_VERSION 1
#define MUSICPACK_WAVEFORM_INTERVAL_MS 100
#define MUSICPACK_WAVEFORM_ENCODING "peak-rms-u8"
#define MUSICPACK_WAVEFORM_FLOOR_DB (-60)
#define MUSICPACK_WAVEFORM_BYTES_PER_BUCKET 2
#define MUSICPACK_WAVEFORM_MAX_POINTS 864000u
#define MUSICPACK_WAVEFORM_MAX_BYTES (MUSICPACK_WAVEFORM_MAX_POINTS * MUSICPACK_WAVEFORM_BYTES_PER_BUCKET)

/* ------------------------------------------------------------------ */
/* metadata + per-bucket type                                         */
/* ------------------------------------------------------------------ */

typedef struct musicpack_waveform_meta {
    uint32_t version;       /* 1 */
    uint32_t interval_ms;   /* 100 */
    int32_t  floor_db;      /* -60 */
    uint32_t points;        /* bucket count, including the final partial bucket */
} musicpack_waveform_meta;

typedef struct musicpack_waveform_bucket {
    uint8_t peak;
    uint8_t rms;
} musicpack_waveform_bucket;

/* ------------------------------------------------------------------ */
/* quantization                                                       */
/* ------------------------------------------------------------------ */

/// Quantizes a single linear amplitude to `uint8` using the v1 logarithmic
/// mapping at the given floor (typically `MUSICPACK_WAVEFORM_FLOOR_DB`).
/// `amplitude == 0` returns 0. `amplitude > 1.0` clamps to 255.
/// Deterministic: same inputs produce the same byte.
MUSICPACK_API uint8_t musicpack_waveform_quantize(double amplitude, int floor_db);

/* ------------------------------------------------------------------ */
/* streaming accumulator                                              */
/* ------------------------------------------------------------------ */

/// Constant-memory accumulator over a normalized [-1, 1] float PCM
/// stream. Bucket boundaries are computed from the cumulative sample
/// clock (no per-bucket sample counting drift). Up to MUSICPACK_AUDIO_MAX_CHANNELS.
typedef struct musicpack_waveform_acc musicpack_waveform_acc;

/// Creates an accumulator for the given stream parameters. `channels`
/// must be 1..8 (same range as `musicpack_audio_get_format`). The bucket
/// size is fixed at `MUSICPACK_WAVEFORM_INTERVAL_MS`.
MUSICPACK_API musicpack_waveform_acc *musicpack_waveform_acc_new(unsigned sample_rate,
                                                                 unsigned channels);

/// Feeds `frames` interleaved `float` sample-frames (length must be
/// `frames * channels`). Zero-length input is a no-op. May be called
/// repeatedly across chunked reads. Returns `MUSICPACK_ERR_INVALID` on
/// a NULL or wrong-channel-count input, otherwise `MUSICPACK_OK`.
MUSICPACK_API musicpack_status musicpack_waveform_acc_feed_f32(musicpack_waveform_acc *a,
                                                               const float *interleaved,
                                                               size_t frames);

/// Finalizes accumulation. Returns the owned bucket array (caller frees
/// with `free()`) and the count. Always emits at least one bucket when
/// at least one frame was fed. Returns `MUSICPACK_ERR_NOMEM` on
/// allocation failure.
MUSICPACK_API musicpack_status musicpack_waveform_acc_finish(musicpack_waveform_acc *a,
                                                             musicpack_waveform_bucket **out,
                                                             size_t *count);

/// Releases an accumulator. NULL is a no-op.
MUSICPACK_API void musicpack_waveform_acc_free(musicpack_waveform_acc *a);

/* ------------------------------------------------------------------ */
/* binary payload encode / decode / validate                          */
/* ------------------------------------------------------------------ */

/// Encodes `count` buckets as `peak-rms-u8` bytes. The caller receives
/// an owned buffer of `count * 2` bytes (caller frees with `free()`).
MUSICPACK_API musicpack_status musicpack_waveform_encode(const musicpack_waveform_bucket *buckets,
                                                          size_t count,
                                                          unsigned char **out,
                                                          size_t *out_len);

/// Decodes a `peak-rms-u8` payload. The caller receives an owned bucket
/// array (caller frees with `free()`). `len` must be a multiple of 2
/// and not exceed `MUSICPACK_WAVEFORM_MAX_BYTES`.
MUSICPACK_API musicpack_status musicpack_waveform_decode(const unsigned char *data,
                                                          size_t len,
                                                          musicpack_waveform_bucket **out,
                                                          size_t *out_count);

/// Validates a payload against declared metadata. The metadata
/// `version`, `interval_ms`, `floor_db`, and the implicit encoding are
/// checked against the v1 closed enum; `points` must match `len / 2`;
/// every byte must be in `[0, 255]`. Returns `MUSICPACK_OK` on success.
MUSICPACK_API musicpack_status musicpack_waveform_validate(const unsigned char *data,
                                                           size_t len,
                                                           const musicpack_waveform_meta *meta);

#ifdef __cplusplus
}
#endif
#endif /* MUSICPACK_WAVEFORM_H_ */