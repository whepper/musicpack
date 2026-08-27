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
/// \file manifest.h
/// The `.mpack` v1 manifest model: what an album IS (independent of storage).
///
/// The model is a plain, owned C structure. All string members are
/// `strdup`'d and released by musicpack_manifest_free(). Unknown source JSON
/// fields are not represented here. Package handles retain only unknown root
/// fields on save; nested fields cannot safely survive model array edits.
#ifndef MUSICPACK_MANIFEST_H_
#define MUSICPACK_MANIFEST_H_
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <musicpack/error.h>
#include <musicpack/export.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Manifes format identifier.
#define MUSICPACK_FORMAT "musicpack"
/// Current schema version.
#define MUSICPACK_VERSION_SCHEMA 1

/// A named credit (artist/author/role).
typedef struct musicpack_artist {
    char *name; ///< required
    char *role; ///< optional ("main", "featuring", ...), may be NULL
    char *musicbrainz_id; ///< optional artist MusicBrainz id (identity hint)
    char *sort_name;      ///< optional sort name (e.g. "Bowie, David")
} musicpack_artist;

/// A referenced object in the package (manifest-relative path + required hash).
typedef struct musicpack_asset {
    char *path;  ///< required, '/'-separated, relative, contained
    char *sha256; ///< required lowercase SHA-256 hex
} musicpack_asset;

/// Measured BS.1770 loudness (gain is derived, never stored).
typedef struct musicpack_loudness {
    int present;
    double lufs;        ///< integrated loudness, dB LUFS
    double true_peak_db; ///< true peak, dBTP
} musicpack_loudness;

/// Per-track waveform envelope reference. Optional. When present, every
/// field is required; the interpretation is fixed for v1 (closed enums on
/// `version`, `interval_ms`, `encoding`, `floor_db`). See
/// `specs/musicpack-waveform-v1.md`.
typedef struct musicpack_waveform_ref {
    int present;       ///< 0 when the track has no waveform declared
    int version;       ///< 1
    char *path;        ///< canonical package-relative path (unique across assets)
    char *sha256;      ///< lowercase hex (64 chars)
    int interval_ms;   ///< 100
    char *encoding;    ///< "peak-rms-u8"
    int floor_db;      ///< -60
    unsigned long points; ///< bucket count, including final partial bucket
} musicpack_waveform_ref;

/// An alternate audio representation for a track (Phase 3). The track's
/// primary/default audio stays `musicpack_track.audio`; this lists
/// alternates only. Each entry is a normal referenced asset: required
/// path+sha256, optional free-text label and optional codec hint.
typedef struct musicpack_representation {
    char *path;   ///< required, package-unique
    char *sha256; ///< required lowercase SHA-256 hex
    char *label;  ///< optional display label ("FLAC 24/96"); may be NULL
    char *codec;  ///< optional codec hint ("flac", "mpc", ...); may be NULL
} musicpack_representation;

/// A single track on a disc.
typedef struct musicpack_track {
    int number;
    char *title;
    musicpack_artist *artists; ///< optional per-track override; may be NULL
    size_t artist_count;
    char *isrc;               ///< optional
    char *musicbrainz_track_id; ///< optional recording-then-release-specific (MB track ID)
    char *musicbrainz_recording_id; ///< optional durable recording identity
    char *source_store;   ///< optional provenance (e.g. "Deezer")
    char *source_track_id; ///< optional provider track id
    char *source_audio_codec; ///< optional pre-encoding codec ("flac")
    char *source_audio_md5;   ///< optional pre-encoding source hash
    int has_duration;     ///< duration is DERIVED, not canonical
    double duration;      ///< seconds
    musicpack_loudness loudness;
    musicpack_asset audio;
    char *audio_codec;       ///< optional encoded audio codec ("mpc", "flac", ...)
    musicpack_waveform_ref waveform; ///< optional per-track waveform reference
    musicpack_representation *representations; ///< optional alternates; may be NULL
    size_t representation_count;
} musicpack_track;

/// A disc / medium.
typedef struct musicpack_disc {
    int disc;
    char *format; ///< optional medium format ("CD", "Digital", ...); may be NULL
    char *title; ///< optional; may be NULL
    musicpack_track *tracks;
    size_t track_count;
} musicpack_disc;

/// Artwork with a role tag ("front", "back", "disc", ...).
typedef struct musicpack_artwork {
    char *role;
    musicpack_asset asset;
} musicpack_artwork;

/// A referenced analysis document (e.g. sonic embeddings). The manifest only
/// references the document; it never embeds its payload.
typedef struct musicpack_analysis {
    char *type;    ///< required; "sonic" is the v1 type, others stay forward-compatible
    char *profile; ///< required for type "sonic"; the sonic profile id
    musicpack_asset asset; ///< package-relative path + sha256 (required for "sonic")
} musicpack_analysis;

/// The specific release/edition this package represents.
///
/// Album-level fields describe the release GROUP (what album this belongs
/// to); this struct describes the exact collectible release/edition (which
/// specific CD, remaster, digital edition, ... the package holds).
typedef struct musicpack_release {
    int present;              ///< 1 when any field is set
    char *release_date;       ///< optional ISO-8601
    char *edition;            ///< optional (e.g. "2016 Remaster")
    char *country;            ///< optional; ISO 3166-1 alpha-2 recommended
    char *label;              ///< optional
    char *catalogue_number;   ///< optional
    char *notes;              ///< optional edition/release notes
} musicpack_release;

/// The parsed v1 manifest.
typedef struct musicpack_manifest {
    char *album_title;
    musicpack_artist *album_artists;
    size_t album_artist_count;
    char *release_type;    ///< optional: album|ep|single|...|other (closed enum)
    char *original_release_date; ///< optional ISO-8601 (release-group first release)
    char **genres;         ///< optional multi-value
    size_t genre_count;
    musicpack_release release;   ///< the specific release/edition
    char *musicbrainz_release_group_id; ///< optional (release-group identity)
    char *musicbrainz_release_id; ///< optional (specific-release identity)
    char *barcode;                ///< optional
    char *identity_source;        ///< optional: musicbrainz|store|local
    char *identity_confidence;    ///< optional: exact|confirmed|probable|none
    char *source_type;            ///< optional: cd-rip|digital-download|...
    char *source_store;           ///< optional
    char *source_id;              ///< optional provider release id
    musicpack_disc *discs;
    size_t disc_count;
    musicpack_artwork *artwork;
    size_t artwork_count;
    musicpack_asset *booklet;
    size_t booklet_count;
    musicpack_asset *lyrics;
    size_t lyrics_count;
    musicpack_asset *extras;
    size_t extras_count;
    musicpack_analysis *analysis;
    size_t analysis_count;
    int has_album_loudness;
    musicpack_loudness album_loudness;
    char *loudness_algorithm;  ///< optional BS.1770 revision (e.g. "ITU-R BS.1770-5")
    char *provenance_tool;       ///< optional
    char *provenance_tool_version; ///< optional
} musicpack_manifest;

/// Maximum total number of referenced assets across all manifest arrays.
#define MUSICPACK_MANIFEST_MAX_REFERENCED_ASSETS 4096

/// Maximum size of a single referenced file (8 GiB).
#define MUSICPACK_MANIFEST_MAX_FILE_SIZE (8ULL * 1024 * 1024 * 1024)
/// Maximum total referenced bytes per package (64 GiB).
#define MUSICPACK_MANIFEST_MAX_TOTAL_BYTES (64ULL * 1024 * 1024 * 1024)

/* Per-array resource budgets, enforced during parsing before typed
   allocations so a hostile manifest cannot cause unbounded allocation
   amplification that the 4096-asset total alone would permit. */
#define MUSICPACK_MANIFEST_MAX_DISCS 32
#define MUSICPACK_MANIFEST_MAX_TRACKS_PER_DISC 512
#define MUSICPACK_MANIFEST_MAX_ARTISTS_PER_CREDIT 64
#define MUSICPACK_MANIFEST_MAX_GENRES 64
#define MUSICPACK_MANIFEST_MAX_ARTWORK 32
#define MUSICPACK_MANIFEST_MAX_BOOKLET 32
#define MUSICPACK_MANIFEST_MAX_LYRICS 512
#define MUSICPACK_MANIFEST_MAX_EXTRAS 256
#define MUSICPACK_MANIFEST_MAX_ANALYSIS 32

/// Parses manifest JSON text into a typed model (validation is structural:
/// format, version, required fields, path rules, numbering, loudness ranges,
/// hashes, closed enums, and the 4096 referenced-asset limit).
///
/// \param json      NUL-terminated JSON text
/// \param status    optional error out
/// \return an owned model, or NULL on failure
MUSICPACK_API musicpack_manifest *musicpack_manifest_parse(const char *json,
                                                           musicpack_status *status);

/// Releases a manifest and all owned strings.
MUSICPACK_API void musicpack_manifest_free(musicpack_manifest *m);

/// Releases all owned strings inside a manifest without freeing the struct
/// itself. For manifests created on the caller's stack.
MUSICPACK_API void musicpack_manifest_clear(musicpack_manifest *m);

/// Serializes a manifest to JSON text in canonical key order.
///
/// \param m        manifest to serialize
/// \param json_out receives a NUL-terminated string (caller frees)
/// \return MUSICPACK_OK or an error
MUSICPACK_API musicpack_status musicpack_manifest_write(const musicpack_manifest *m,
                                                        char **json_out);

#ifdef __cplusplus
}
#endif
#endif /* MUSICPACK_MANIFEST_H_ */
