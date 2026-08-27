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
/// \file sonic.h
/// MusicPack Sonic analysis — the `musicpack-sonic` container document v1.
///
/// The container is model-independent (see `specs/musicpack-sonic-v1.md`).
/// A document carries a model-scoped profile id plus track/album embeddings
/// encoded as `base64-f32le` (IEEE-754 float32, little-endian, base64).
/// libmusicpack is the authoritative parser/validator; this module is the
/// only place Sonic semantics live.
///
/// Unknown profile ids keep a document structurally readable: parsing and
/// validation use the parameters the document declares itself, and the
/// caller is told the profile state so it can decide comparison
/// eligibility. A malformed document claiming a known profile always fails.
#ifndef MUSICPACK_SONIC_H_
#define MUSICPACK_SONIC_H_
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <musicpack/error.h>
#include <musicpack/export.h>
#include <musicpack/manifest.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Document format identifier.
#define MUSICPACK_SONIC_FORMAT "musicpack-sonic"
/// Container version (v1).
#define MUSICPACK_SONIC_VERSION 1

/// Maximum sonic document size (same bound as manifest.json).
#define MUSICPACK_SONIC_DOC_MAX (16u * 1024u * 1024u)
/// Maximum vector dimensions (bounds base64 allocation).
#define MUSICPACK_SONIC_MAX_DIMENSIONS (1u << 20)
/// Maximum tracks entries when validating without a manifest.
#define MUSICPACK_SONIC_MAX_TRACKS 4096
/// L2-normalization tolerance for stored vectors.
#define MUSICPACK_SONIC_NORM_TOLERANCE 1e-3

/// Supported v1 vector encoding.
#define MUSICPACK_SONIC_ENCODING "base64-f32le"
/// Cosine distance (the only distance v1 profiles define).
#define MUSICPACK_SONIC_DISTANCE "cosine"

/* ------------------------------------------------------------------ */
/* profile registry                                                    */
/* ------------------------------------------------------------------ */

/// How the profile relates to the v1 registry.
typedef enum musicpack_sonic_profile_state {
    MUSICPACK_SONIC_PROFILE_UNKNOWN = 0,  ///< syntactically valid, not registered
    MUSICPACK_SONIC_PROFILE_RESERVED = 1, ///< registered but research-only / rejected
    MUSICPACK_SONIC_PROFILE_SUPPORTED = 2 ///< registered and supported
} musicpack_sonic_profile_state;

/// Registered profile metadata (validation side only; inference parameters
/// live in the analyzer, never here).
typedef struct musicpack_sonic_profile {
    const char *id;        ///< model-scoped stable id, e.g. "musicpack-sonic-openl3-v1"
    size_t dimensions;     ///< vector dimensions
    const char *distance;  ///< distance metric ("cosine")
    const char *encoding;  ///< vector encoding ("base64-f32le")
    musicpack_sonic_profile_state state; ///< registry status
} musicpack_sonic_profile;

/// Looks up a profile in the v1 registry. Returns NULL when the id is
/// syntactically valid but not registered (the document remains parseable;
/// see musicpack_sonic_validate()).
MUSICPACK_API const musicpack_sonic_profile *musicpack_sonic_profile_get(const char *id);

/// Validates the profile-id syntax (see the sonic spec §6): fixed
/// `musicpack-sonic-` prefix, one or more `[a-z0-9]` segments, `-v<digits>`
/// suffix. Returns 1 when syntactically valid.
MUSICPACK_API int musicpack_sonic_profile_id_valid(const char *id);

/// Returns 1 when `encoding` is a supported v1 vector encoding.
MUSICPACK_API int musicpack_sonic_encoding_supported(const char *encoding);

/* ------------------------------------------------------------------ */
/* vectors                                                             */
/* ------------------------------------------------------------------ */

/// Decodes `base64-f32le` into exactly `dimensions` float32 values and
/// validates them (finiteness + unit L2 norm within
/// MUSICPACK_SONIC_NORM_TOLERANCE). On success \p out is an owned array of
/// `dimensions` floats.
MUSICPACK_API musicpack_status musicpack_sonic_vector_decode(const char *base64, size_t n,
                                                             size_t dimensions,
                                                             float **out, size_t *out_count);

/// Encodes `n` float32 values as `base64-f32le`. \p out receives an owned
/// string (caller frees).
MUSICPACK_API musicpack_status musicpack_sonic_vector_encode(const float *v, size_t n,
                                                             char **out);

/// Validates an in-memory vector: every value finite and unit L2 norm
/// within \p tolerance.
MUSICPACK_API musicpack_status musicpack_sonic_vector_validate(const float *v, size_t n,
                                                               double tolerance);

/* ------------------------------------------------------------------ */
/* document model                                                      */
/* ------------------------------------------------------------------ */

/// An embedding: present=1 with `dimensions` floats, or present=0 (null).
typedef struct musicpack_sonic_embedding {
    int present;       ///< 0 => null (no embedding; never fabricated)
    size_t dimensions; ///< always the document's profile dimensions when present
    float *data;       ///< owned
} musicpack_sonic_embedding;

/// A per-track embedding entry (`disc`+`track` matches the manifest media).
typedef struct musicpack_sonic_track {
    int disc;
    int track;
    musicpack_sonic_embedding embedding;
} musicpack_sonic_track;

/// A parsed `musicpack-sonic` v1 document.
typedef struct musicpack_sonic {
    char *profile_id;            ///< declared profile id
    size_t dimensions;           ///< declared dimensions
    char *distance;              ///< declared distance metric
    char *encoding;              ///< declared encoding
    char *analyzer_tool;         ///< analyzer provenance
    char *analyzer_tool_version; ///< analyzer provenance
    musicpack_sonic_embedding album;        ///< album embedding (may be null)
    int album_tracks_contributing;          ///< declared contributor count
    musicpack_sonic_track *tracks;          ///< owned array
    size_t track_count;
} musicpack_sonic;

/// Parses a `musicpack-sonic` v1 document. Enforces the representation
/// contract: format/version, profile-id syntax, supported encoding,
/// dimension bounds, base64 decode, exact byte length, finiteness, L2 norm
/// and size limits. Returns an owned model, or NULL with \p status set.
MUSICPACK_API musicpack_sonic *musicpack_sonic_parse(const char *json, size_t len,
                                                     musicpack_status *status);

/// Releases a parsed document.
MUSICPACK_API void musicpack_sonic_free(musicpack_sonic *s);

/// Validates a parsed document's semantics. Registered profiles are checked
/// against their registry metadata (dimensions/encoding/distance); every
/// document is checked for internal consistency (no duplicate track entries,
/// `tracksContributing` matches the non-null count, album embedding exists
/// exactly when contributors exist, and the album vector matches the
/// normalized equal-track mean of non-null track vectors within the stored
/// float32 norm tolerance). When \p m is non-NULL, `tracks[]` must contain
/// exactly one entry per manifest track.
///
/// \p profile_state (optional) receives the profile state so the caller can
/// distinguish fully validated (SUPPORTED) from structurally validated
/// (UNKNOWN / RESERVED) documents.
MUSICPACK_API musicpack_status musicpack_sonic_validate(const musicpack_sonic *s,
                                                        const musicpack_manifest *m,
                                                        musicpack_sonic_profile_state *profile_state);

#ifdef __cplusplus
}
#endif
#endif /* MUSICPACK_SONIC_H_ */
