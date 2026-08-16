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
/// \file audio.h
/// Narrow lossless-source PCM decoding for the MusicPack authoring pipeline.
///
/// This abstraction feeds the BS.1770 meter and the Musepack encoder from
/// FLAC, WAV and Musepack sources without any external decoder. It is
/// deliberately small: sample-rate/channel information, frame reads in float
/// or left-aligned 32-bit PCM, and EOF-vs-error reporting. It is NOT a
/// general multimedia framework.
///
/// Supported inputs:
///   - FLAC (via vendored dr_flac; any supported bit depth, 1-8 channels)
///   - WAV (RIFF PCM: 8/16/24/32-bit integer and 32-bit float, 1-8 channels;
///     WAVE_FORMAT_EXTENSIBLE PCM/float accepted; other formats rejected)
///   - Musepack SV7/SV8 (via libmusepack; decoded to float)
///
/// float reads (read_frames_f32) return normalized PCM in ~[-1, 1]; integer
/// reads (read_frames_s32) return the source samples left-aligned in the 32
/// bit word (16-bit content in bits 31..16, 24-bit in bits 31..8), so both
/// decode paths are exact for the integer PCM formats MusicPack authors with.
/// End of stream is reported as a successful read of zero frames; I/O and
/// malformed-stream failures are reported with an error status.
#ifndef MUSICPACK_AUDIO_H_
#define MUSICPACK_AUDIO_H_
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <musicpack/error.h>
#include <musicpack/export.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Opaque audio decoder session.
typedef struct musicpack_audio musicpack_audio;

/// Stream properties reported by musicpack_audio_get_format().
typedef struct musicpack_audio_format {
    unsigned sample_rate;     ///< Hz
    unsigned channels;        ///< 1..8
    unsigned bits_per_sample; ///< 8/16/24/32 for FLAC/WAV, 0 for Musepack
    uint64_t total_samples;   ///< sample-frames; 0 when unknown
    char codec[16];           ///< "flac", "wav" or "musepack"
    int is_float;             ///< 1 for IEEE-float WAV (never encodable)
} musicpack_audio_format;

/// Opens an audio source for reading. The codec is selected from the file
/// extension (`.flac`, `.wav`, `.mpc`) and verified from the file content.
///
/// \param path   path to the source file
/// \param status optional error out
/// \return an opaque session, or NULL on failure (unreadable, unsupported
///         or malformed input)
MUSICPACK_API musicpack_audio *musicpack_audio_open(const char *path,
                                                    musicpack_status *status);

/// Copies the stream properties into \p fmt.
MUSICPACK_API musicpack_status musicpack_audio_get_format(
    const musicpack_audio *a, musicpack_audio_format *fmt);

/// Decodes up to \p frames sample-frames of interleaved float PCM.
///
/// \p interleaved must hold at least `frames * channels` floats. On success
/// \p read receives the number of frames produced; zero means end of stream.
MUSICPACK_API musicpack_status musicpack_audio_read_frames_f32(
    musicpack_audio *a, float *interleaved, size_t frames, size_t *read);

/// Decodes up to \p frames sample-frames of interleaved left-aligned 32-bit
/// integer PCM (see the header comment for alignment).
///
/// \p interleaved must hold at least `frames * channels` int32 samples.
/// Unsupported for Musepack sources and 32-bit float WAV (returns
/// MUSICPACK_ERR_INVALID); the encoder path only ever requests it for
/// integer FLAC/WAV sources.
MUSICPACK_API musicpack_status musicpack_audio_read_frames_s32(
    musicpack_audio *a, int32_t *interleaved, size_t frames, size_t *read);

/// Closes a session. NULL is a no-op.
MUSICPACK_API void musicpack_audio_close(musicpack_audio *a);

#ifdef __cplusplus
}
#endif
#endif /* MUSICPACK_AUDIO_H_ */
