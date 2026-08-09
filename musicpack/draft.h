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
/// \file draft.h
/// Authoring-draft JSON helpers for the MusicPack Author GUI.
///
/// The GUI keeps an editable in-memory draft (application state, not a
/// public MusicPack format) and serializes it to a `musicpack-draft` JSON
/// only to cross the `musicpack` CLI boundary. This module (re)shapes that
/// JSON; every piece of package semantics stays in libmusicpack.
#ifndef MUSICPACK_DRAFT_H_
#define MUSICPACK_DRAFT_H_
#pragma once

#include <musicpack/manifest.h>

#include <cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Probed audio-stream info carried by the draft for display (not written
/// to the package manifest; the .mpack model keeps `audio.codec` at the
/// presentation layer only).
typedef struct mpc_stream_info {
    char codec[32];       ///< "musepack-sv8", "flac", ...
    int stream_version;   ///< 8 for SV8 Musepack, 0 otherwise
    long sample_rate;
    long channels;
    double duration;      ///< seconds, 0 when unknown
} mpc_stream_info;

/// Reads and parses a JSON file (bounded). Returns an owned cJSON root, or
/// NULL with a message in \p err (when \p err_cap > 0).
cJSON *draft_read_json(const char *path, char *err, size_t err_cap);

/// Serializes a manifest into a `musicpack-draft` JSON object. \p source_root
/// is stored verbatim. \p streams is an optional parallel array over the
/// manifest's tracks in media/track order (may be NULL). artwork/booklet/
/// lyrics/extras start as empty arrays for the caller (inspect) to fill.
cJSON *draft_from_manifest(const musicpack_manifest *m, const char *source_root,
                           const mpc_stream_info *streams);

/// Converts a parsed draft into a manifest: album/release/identifiers/
/// identity/source/media, plus file-based artwork/booklet/lyrics/extras
/// (embedded entries — those without a `path` — are skipped). Track
/// `audio.path` is set to the draft's `audioPath`. \p m must be zeroed
/// first; release with musicpack_manifest_clear(). Returns 1 on success.
int draft_to_manifest(cJSON *draft, musicpack_manifest *m);

/// Pushes manifest fields that MusicBrainz matching can fill back into an
/// existing draft JSON (album/release/identifiers/identity and per-track
/// identifiers matched by disc+track). Draft-only fields are preserved.
int draft_apply_manifest(cJSON *draft, const musicpack_manifest *m);

/// Extracts MusicBrainz candidate records from a search envelope for display:
/// [{releaseId, releaseGroupId, title, artist, date, country, barcode,
///   confidence}] with per-release confidence computed against \p m.
/// Returns an owned cJSON array, or NULL when the envelope cannot be parsed.
cJSON *mb_candidates(const char *search_json, const musicpack_manifest *m);

/// Prints a cJSON document (formatted) to stdout.
void draft_print(cJSON *root);

#ifdef __cplusplus
}
#endif
#endif /* MUSICPACK_DRAFT_H_ */
