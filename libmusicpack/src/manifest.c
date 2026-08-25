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
/// \file manifest.c
/// `.mpack` v1 manifest model: parse, validate, free and serialize.

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>

#include "internal.h"
#include <musicpack/path.h>
#include <musicpack/loudness.h>

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static int
get_req_string(cJSON *obj, const char *key, char **field, musicpack_status *status)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(v) || v->valuestring == 0 || *v->valuestring == '\0') {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    *field = strdup(v->valuestring);
    if (*field == 0) {
        *status = MUSICPACK_ERR_NOMEM;
        return 0;
    }
    return 1;
}

static int
get_opt_string(cJSON *obj, const char *key, char **field, musicpack_status *status)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v == 0)
        return 1;
    if (!cJSON_IsString(v)) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    *field = strdup(v->valuestring);
    if (*field == 0) {
        *status = MUSICPACK_ERR_NOMEM;
        return 0;
    }
    return 1;
}

static int
get_req_int(cJSON *obj, const char *key, int *out, musicpack_status *status)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < 1 ||
        v->valuedouble > INT_MAX || v->valuedouble != floor(v->valuedouble)) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    *out = (int) v->valuedouble;
    return 1;
}

static int
get_opt_double(cJSON *obj, const char *key, int *present, double *out,
               musicpack_status (*validate)(double), musicpack_status *status)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v == 0) {
        *present = 0;
        return 1;
    }
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble)) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    if (validate != 0 && validate(v->valuedouble) != MUSICPACK_OK) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    *present = 1;
    *out = v->valuedouble;
    return 1;
}

static int
is_lower_hex(const char *s)
{
    size_t i;
    if (s == 0)
        return 0;
    for (i = 0; s[i] != '\0'; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return 0;
    }
    return i == 64;
}

static int
is_in_string_list(const char *s, const char *const *list, size_t n)
{
    size_t i;
    if (s == 0)
        return 0;
    for (i = 0; i < n; i++)
        if (strcmp(s, list[i]) == 0)
            return 1;
    return 0;
}

/* Closed enums: unknown values are rejected so `other` stays the escape
   hatch. `album.releaseType` and `media[].format` are deliberately not free
   strings. */
static const char *const RELEASE_TYPES[] = {
    "album", "ep", "single", "maxi-single", "compilation", "soundtrack",
    "live-album", "remix-album", "box-set", "other"
};

static const char *const MEDIUM_FORMATS[] = {
    "CD", "SACD", "Vinyl", "Cassette", "Digital", "Blu-ray Audio",
    "DVD-Audio", "Other"
};

static int
get_opt_enum(cJSON *obj, const char *key, char **field, const char *const *list,
             size_t n, musicpack_status *status)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v == 0)
        return 1;
    if (!cJSON_IsString(v) || !is_in_string_list(v->valuestring, list, n)) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    *field = strdup(v->valuestring);
    if (*field == 0) {
        *status = MUSICPACK_ERR_NOMEM;
        return 0;
    }
    return 1;
}

static int
parse_asset(cJSON *o, musicpack_asset *a, musicpack_status *status)
{
    if (!cJSON_IsObject(o)) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    if (!get_req_string(o, "path", &a->path, status))
        return 0;
    if (musicpack_path_validate(a->path) != MUSICPACK_OK) {
        *status = MUSICPACK_ERR_PATH;
        return 0;
    }
    if (!get_req_string(o, "sha256", &a->sha256, status))
        return 0;
    if (!is_lower_hex(a->sha256)) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    return 1;
}

static int
parse_artists(cJSON *arr, musicpack_artist **out, size_t *count, musicpack_status *status)
{
    cJSON *item;
    int i = 0;

    *out = 0;
    *count = 0;
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) == 0) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    if (cJSON_GetArraySize(arr) > MUSICPACK_MANIFEST_MAX_ARTISTS_PER_CREDIT) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    *out = (musicpack_artist *) calloc((size_t) cJSON_GetArraySize(arr), sizeof **out);
    if (*out == 0) {
        *status = MUSICPACK_ERR_NOMEM;
        return 0;
    }
    cJSON_ArrayForEach(item, arr) {
        musicpack_artist *a = &(*out)[i];
        if (!get_req_string(item, "name", &a->name, status))
            return 0;
        if (!get_opt_string(item, "role", &a->role, status))
            return 0;
        if (!get_opt_string(item, "musicbrainzId", &a->musicbrainz_id, status))
            return 0;
        if (!get_opt_string(item, "sortName", &a->sort_name, status))
            return 0;
        i++;
    }
    *count = (size_t) i;
    return 1;
}

static int
parse_loudness(cJSON *o, musicpack_loudness *l, musicpack_status *status)
{
    int have_lufs = 0, have_peak = 0;

    l->present = 0;
    if (o == 0)
        return 1;
    if (!cJSON_IsObject(o)) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    if (!get_opt_double(o, "trackLUFS", &have_lufs, &l->lufs,
                        musicpack_loudness_validate_lufs, status))
        return 0;
    if (!get_opt_double(o, "truePeakDbTP", &have_peak, &l->true_peak_db,
                        musicpack_loudness_validate_true_peak, status))
        return 0;
    if (!have_lufs || !have_peak) {
        *status = MUSICPACK_ERR_INVALID;
        return 0; /* both measured values required when the object is present */
    }
    l->present = 1;
    return 1;
}

/* Per-track waveform envelope reference (optional). Closed enums for v1
   (`version`, `intervalMs`, `encoding`, `floorDb`). `points` is bounded
   (≤ 864000). Path is validated by the canonical rules. `sha256` must be
   64 lowercase hex characters. */
static int
parse_waveform(cJSON *o, musicpack_waveform_ref *w, musicpack_status *status)
{
    cJSON *v;
    int have_version = 0, have_interval = 0, have_floor = 0;
    int have_points = 0;
    double version_d = 0.0, interval_d = 0.0, floor_d = 0.0;
    double points_d = 0.0;

    w->present = 0;
    if (o == 0)
        return 1;
    if (!cJSON_IsObject(o)) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }

    v = cJSON_GetObjectItemCaseSensitive(o, "version");
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) ||
        v->valuedouble != floor(v->valuedouble)) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    version_d = v->valuedouble;
    have_version = 1;

    if (!get_req_string(o, "path", &w->path, status))
        return 0;
    if (musicpack_path_validate(w->path) != MUSICPACK_OK) {
        *status = MUSICPACK_ERR_PATH;
        return 0;
    }

    if (!get_req_string(o, "sha256", &w->sha256, status))
        return 0;
    if (!is_lower_hex(w->sha256)) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }

    v = cJSON_GetObjectItemCaseSensitive(o, "intervalMs");
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) ||
        v->valuedouble != floor(v->valuedouble)) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    interval_d = v->valuedouble;
    have_interval = 1;

    if (!get_req_string(o, "encoding", &w->encoding, status))
        return 0;
    if (strcmp(w->encoding, "peak-rms-u8") != 0) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }

    v = cJSON_GetObjectItemCaseSensitive(o, "floorDb");
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) ||
        v->valuedouble != floor(v->valuedouble)) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    floor_d = v->valuedouble;
    have_floor = 1;

    v = cJSON_GetObjectItemCaseSensitive(o, "points");
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) ||
        v->valuedouble != floor(v->valuedouble) || v->valuedouble < 0.0 ||
        v->valuedouble > 864000.0) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    points_d = v->valuedouble;
    have_points = 1;

    if (!have_version || !have_interval || !have_floor || !have_points) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }

    /* v1 closed-enum checks */
    if (version_d != 1.0 || interval_d != 100.0 || floor_d != -60.0) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }

    w->version = 1;
    w->interval_ms = 100;
    w->floor_db = -60;
    w->points = (unsigned long) points_d;
    w->present = 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/* parse                                                               */
/* ------------------------------------------------------------------ */

static int
parse_representation(cJSON *o, musicpack_representation *r,
                     musicpack_status *status)
{
    /* A representation is a referenced asset (path+sha256 required, path
       validated) plus optional display/hint strings. The asset part parses
       straight into the leading members of the struct (same layout as
       musicpack_asset). */
    if (!parse_asset(o, (musicpack_asset *) r, status))
        return 0;
    if (!get_opt_string(o, "label", &r->label, status))
        return 0;
    if (!get_opt_string(o, "codec", &r->codec, status))
        return 0;
    return 1;
}

static int
parse_track(cJSON *o, musicpack_track *t, musicpack_status *status)
{
    cJSON *v;

    if (!cJSON_IsObject(o)) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    if (!get_req_int(o, "track", &t->number, status))
        return 0;
    if (!get_req_string(o, "title", &t->title, status))
        return 0;

    v = cJSON_GetObjectItemCaseSensitive(o, "artists");
    if (v != 0 && !parse_artists(v, &t->artists, &t->artist_count, status))
        return 0;

    v = cJSON_GetObjectItemCaseSensitive(o, "identifiers");
    if (v != 0) {
        if (!cJSON_IsObject(v)) {
            *status = MUSICPACK_ERR_INVALID;
            return 0;
        }
        if (!get_opt_string(v, "isrc", &t->isrc, status))
            return 0;
        if (!get_opt_string(v, "musicbrainzTrackId", &t->musicbrainz_track_id, status))
            return 0;
        if (!get_opt_string(v, "musicbrainzRecordingId", &t->musicbrainz_recording_id, status))
            return 0;
    }
    v = cJSON_GetObjectItemCaseSensitive(o, "source");
    if (v != 0) {
        if (!cJSON_IsObject(v)) {
            *status = MUSICPACK_ERR_INVALID;
            return 0;
        }
        if (!get_opt_string(v, "store", &t->source_store, status))
            return 0;
        if (!get_opt_string(v, "trackId", &t->source_track_id, status))
            return 0;
    }
    v = cJSON_GetObjectItemCaseSensitive(o, "sourceAudio");
    if (v != 0) {
        if (!cJSON_IsObject(v)) {
            *status = MUSICPACK_ERR_INVALID;
            return 0;
        }
        if (!get_opt_string(v, "codec", &t->source_audio_codec, status))
            return 0;
        if (!get_opt_string(v, "md5", &t->source_audio_md5, status))
            return 0;
    }

    if (!get_opt_double(o, "duration", &t->has_duration, &t->duration, 0, status))
        return 0;
    if (t->has_duration && t->duration <= 0.0) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }

    v = cJSON_GetObjectItemCaseSensitive(o, "loudness");
    if (v != 0 && !parse_loudness(v, &t->loudness, status))
        return 0;

    v = cJSON_GetObjectItemCaseSensitive(o, "audio");
    if (v == 0 || !parse_asset(v, &t->audio, status))
        return 0;
    if (t->audio.sha256 == 0) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    if (!get_opt_string(v, "codec", &t->audio_codec, status))
        return 0;

    v = cJSON_GetObjectItemCaseSensitive(o, "waveform");
    if (v != 0 && !parse_waveform(v, &t->waveform, status))
        return 0;

    v = cJSON_GetObjectItemCaseSensitive(o, "representations");
    if (v != 0) {
        int i = 0;
        cJSON *ritem;
        if (!cJSON_IsArray(v)) {
            *status = MUSICPACK_ERR_INVALID;
            return 0;
        }
        t->representations = (musicpack_representation *) calloc(
            (size_t) cJSON_GetArraySize(v), sizeof *t->representations);
        if (t->representations == 0) {
            *status = MUSICPACK_ERR_NOMEM;
            return 0;
        }
        cJSON_ArrayForEach(ritem, v) {
            if (!parse_representation(ritem, &t->representations[i], status))
                return 0;
            i++;
        }
        t->representation_count = (size_t) i;
    }

    return 1;
}

static int
parse_disc(cJSON *o, musicpack_disc *d, musicpack_status *status)
{
    cJSON *tracks;
    cJSON *item;
    int i = 0;

    if (!cJSON_IsObject(o)) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    if (!get_req_int(o, "disc", &d->disc, status))
        return 0;
    if (!get_opt_enum(o, "format", &d->format,
                      MEDIUM_FORMATS, sizeof MEDIUM_FORMATS / sizeof *MEDIUM_FORMATS, status))
        return 0;
    if (!get_opt_string(o, "title", &d->title, status))
        return 0;
    tracks = cJSON_GetObjectItemCaseSensitive(o, "tracks");
    if (!cJSON_IsArray(tracks) || cJSON_GetArraySize(tracks) == 0) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    if (cJSON_GetArraySize(tracks) > MUSICPACK_MANIFEST_MAX_TRACKS_PER_DISC) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    d->tracks = (musicpack_track *) calloc((size_t) cJSON_GetArraySize(tracks), sizeof *d->tracks);
    if (d->tracks == 0) {
        *status = MUSICPACK_ERR_NOMEM;
        return 0;
    }
    cJSON_ArrayForEach(item, tracks) {
        if (!parse_track(item, &d->tracks[i], status))
            return 0;
        i++;
    }
    d->track_count = (size_t) i;
    return 1;
}

static int
check_dup_paths(const musicpack_manifest *m, musicpack_status *status)
{
    /* Collect every referenced asset path and reject duplicates. */
    const char **paths;
    size_t count = m->artwork_count + m->booklet_count + m->lyrics_count +
                   m->extras_count + m->analysis_count;
    size_t d, t, a, wf;

    if (count < m->artwork_count || count < m->booklet_count ||
        count < m->lyrics_count || count < m->extras_count ||
        count < m->analysis_count) {
        *status = MUSICPACK_ERR_NOMEM;
        return 0;
    }
    for (d = 0; d < m->disc_count; d++) {
        if (m->discs[d].track_count > SIZE_MAX - count) {
            *status = MUSICPACK_ERR_NOMEM;
            return 0;
        }
        count += m->discs[d].track_count;
        for (t = 0; t < m->discs[d].track_count; t++) {
            if (m->discs[d].tracks[t].waveform.present)
                count++;
            /* overflow-safe: representation_count is bounded by the
               manifest's own array sizes, far below SIZE_MAX */
            if (m->discs[d].tracks[t].representation_count >
                SIZE_MAX - count) {
                *status = MUSICPACK_ERR_NOMEM;
                return 0;
            }
            count += m->discs[d].tracks[t].representation_count;
        }
    }
    if (count > MUSICPACK_MANIFEST_MAX_REFERENCED_ASSETS) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    paths = (const char **) calloc(count, sizeof *paths);
    if (paths == 0) {
        *status = MUSICPACK_ERR_NOMEM;
        return 0;
    }
    count = 0;
    for (d = 0; d < m->disc_count; d++)
        for (t = 0; t < m->discs[d].track_count; t++)
            paths[count++] = m->discs[d].tracks[t].audio.path;
    for (d = 0; d < m->disc_count; d++)
        for (t = 0; t < m->discs[d].track_count; t++) {
            if (m->discs[d].tracks[t].waveform.present)
                paths[count++] = m->discs[d].tracks[t].waveform.path;
            {
                size_t r;
                for (r = 0; r < m->discs[d].tracks[t].representation_count; r++)
                    paths[count++] = m->discs[d].tracks[t].representations[r].path;
            }
        }
    for (a = 0; a < m->artwork_count; a++) paths[count++] = m->artwork[a].asset.path;
    for (a = 0; a < m->booklet_count; a++) paths[count++] = m->booklet[a].path;
    for (a = 0; a < m->lyrics_count; a++) paths[count++] = m->lyrics[a].path;
    for (a = 0; a < m->extras_count; a++) paths[count++] = m->extras[a].path;
    for (a = 0; a < m->analysis_count; a++) paths[count++] = m->analysis[a].asset.path;
    (void) wf;

    for (a = 0; a < count; a++) {
        size_t b;
        for (b = a + 1; b < count; b++) {
            if (strcmp(paths[a], paths[b]) == 0) {
                *status = MUSICPACK_ERR_INVALID;
                free(paths);
                return 0;
            }
        }
    }
    free(paths);
    return 1;
}

static int
has_duplicate_keys(const cJSON *item)
{
    const cJSON *child;

    if (cJSON_IsObject(item)) {
        for (child = item->child; child != 0; child = child->next) {
            const cJSON *other;
            for (other = child->next; other != 0; other = other->next)
                if (child->string != 0 && other->string != 0 &&
                    strcmp(child->string, other->string) == 0)
                    return 1;
            if (has_duplicate_keys(child))
                return 1;
        }
    } else if (cJSON_IsArray(item)) {
        cJSON_ArrayForEach(child, item)
            if (has_duplicate_keys(child))
                return 1;
    }
    return 0;
}

musicpack_status
musicpack_manifest_parse_tree(const cJSON *root, musicpack_manifest *m)
{
    musicpack_status status = MUSICPACK_OK;
    cJSON *v, *item;
    int i, n;
    size_t d;

    if (!cJSON_IsObject(root) || has_duplicate_keys(root))
        return MUSICPACK_ERR_INVALID;

    /* format + version */
    v = cJSON_GetObjectItemCaseSensitive(root, "format");
    if (!cJSON_IsString(v) || strcmp(v->valuestring, MUSICPACK_FORMAT) != 0)
        return MUSICPACK_ERR_INVALID;
    v = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) ||
        v->valuedouble != (double) MUSICPACK_VERSION_SCHEMA)
        return MUSICPACK_ERR_VERSION;

    /* album */
    v = cJSON_GetObjectItemCaseSensitive(root, "album");
    if (!cJSON_IsObject(v))
        return MUSICPACK_ERR_INVALID;
    if (!get_req_string(v, "title", &m->album_title, &status))
        return status;
    {
        cJSON *artists = cJSON_GetObjectItemCaseSensitive(v, "artists");
        if (!parse_artists(artists, &m->album_artists, &m->album_artist_count, &status))
            return status;
    }
    if (!get_opt_enum(v, "releaseType", &m->release_type,
                      RELEASE_TYPES, sizeof RELEASE_TYPES / sizeof *RELEASE_TYPES, &status))
        return status;
    if (!get_opt_string(v, "originalReleaseDate", &m->original_release_date, &status))
        return status;
    {
        cJSON *genres = cJSON_GetObjectItemCaseSensitive(v, "genres");
        if (genres != 0) {
            if (!cJSON_IsArray(genres))
                return MUSICPACK_ERR_INVALID;
            n = cJSON_GetArraySize(genres);
            if (n > MUSICPACK_MANIFEST_MAX_GENRES)
                return MUSICPACK_ERR_INVALID;
            if (n > 0) {
                m->genres = (char **) calloc((size_t) n, sizeof *m->genres);
                if (m->genres == 0)
                    return MUSICPACK_ERR_NOMEM;
                i = 0;
                cJSON_ArrayForEach(item, genres) {
                    if (!cJSON_IsString(item))
                        return MUSICPACK_ERR_INVALID;
                    m->genres[i] = strdup(item->valuestring);
                    if (m->genres[i] == 0)
                        return MUSICPACK_ERR_NOMEM;
                    i++;
                }
                m->genre_count = (size_t) i;
            }
        }
    }

    /* release (specific release/edition) */
    v = cJSON_GetObjectItemCaseSensitive(root, "release");
    if (v != 0) {
        if (!cJSON_IsObject(v))
            return MUSICPACK_ERR_INVALID;
        if (!get_opt_string(v, "releaseDate", &m->release.release_date, &status))
            return status;
        if (!get_opt_string(v, "edition", &m->release.edition, &status))
            return status;
        if (!get_opt_string(v, "country", &m->release.country, &status))
            return status;
        if (!get_opt_string(v, "label", &m->release.label, &status))
            return status;
        if (!get_opt_string(v, "catalogueNumber", &m->release.catalogue_number, &status))
            return status;
        if (!get_opt_string(v, "notes", &m->release.notes, &status))
            return status;
        if (m->release.release_date != 0 || m->release.edition != 0 ||
            m->release.country != 0 || m->release.label != 0 ||
            m->release.catalogue_number != 0 || m->release.notes != 0)
            m->release.present = 1;
    }

    /* identifiers + identity */
    v = cJSON_GetObjectItemCaseSensitive(root, "identifiers");
    if (v != 0) {
        if (!cJSON_IsObject(v))
            return MUSICPACK_ERR_INVALID;
        if (!get_opt_string(v, "musicbrainzReleaseGroupId", &m->musicbrainz_release_group_id, &status))
            return status;
        if (!get_opt_string(v, "musicbrainzReleaseId", &m->musicbrainz_release_id, &status))
            return status;
        if (!get_opt_string(v, "barcode", &m->barcode, &status))
            return status;
    }
    v = cJSON_GetObjectItemCaseSensitive(root, "identity");
    if (v != 0) {
        if (!cJSON_IsObject(v))
            return MUSICPACK_ERR_INVALID;
        static const char *const IDENTITY_SOURCES[] = { "musicbrainz", "store", "local" };
        static const char *const IDENTITY_CONFIDENCES[] = { "exact", "confirmed", "probable", "none" };
        if (!get_opt_enum(v, "source", &m->identity_source, IDENTITY_SOURCES,
                          sizeof IDENTITY_SOURCES / sizeof *IDENTITY_SOURCES, &status))
            return status;
        if (!get_opt_enum(v, "confidence", &m->identity_confidence, IDENTITY_CONFIDENCES,
                          sizeof IDENTITY_CONFIDENCES / sizeof *IDENTITY_CONFIDENCES, &status))
            return status;
    }

    /* source */
    v = cJSON_GetObjectItemCaseSensitive(root, "source");
    if (v != 0) {
        if (!cJSON_IsObject(v))
            return MUSICPACK_ERR_INVALID;
        if (!get_opt_string(v, "type", &m->source_type, &status))
            return status;
        if (!get_opt_string(v, "store", &m->source_store, &status))
            return status;
        if (!get_opt_string(v, "sourceId", &m->source_id, &status))
            return status;
    }

    /* media */
    v = cJSON_GetObjectItemCaseSensitive(root, "media");
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) == 0)
        return MUSICPACK_ERR_INVALID;
    n = cJSON_GetArraySize(v);
    if (n > MUSICPACK_MANIFEST_MAX_DISCS)
        return MUSICPACK_ERR_INVALID;
    m->discs = (musicpack_disc *) calloc((size_t) n, sizeof *m->discs);
    if (m->discs == 0)
        return MUSICPACK_ERR_NOMEM;
    i = 0;
    cJSON_ArrayForEach(item, v) {
        if (!parse_disc(item, &m->discs[i], &status))
            return status;
        i++;
    }
    m->disc_count = (size_t) i;

    /* unique disc / track numbering */
    for (d = 0; d < m->disc_count; d++) {
        size_t t, u;
        for (t = 0; t < m->discs[d].track_count; t++) {
            for (u = t + 1; u < m->discs[d].track_count; u++)
                if (m->discs[d].tracks[t].number == m->discs[d].tracks[u].number)
                    return MUSICPACK_ERR_INVALID;
        }
        for (u = d + 1; u < m->disc_count; u++)
            if (m->discs[d].disc == m->discs[u].disc)
                return MUSICPACK_ERR_INVALID;
    }

    /* artwork / booklet / lyrics / extras */
    v = cJSON_GetObjectItemCaseSensitive(root, "artwork");
    if (v != 0) {
        if (!cJSON_IsArray(v))
            return MUSICPACK_ERR_INVALID;
        n = cJSON_GetArraySize(v);
        if (n > MUSICPACK_MANIFEST_MAX_ARTWORK)
            return MUSICPACK_ERR_INVALID;
        m->artwork = (musicpack_artwork *) calloc((size_t) n, sizeof *m->artwork);
        if (m->artwork == 0)
            return MUSICPACK_ERR_NOMEM;
        i = 0;
        cJSON_ArrayForEach(item, v) {
            if (!cJSON_IsObject(item))
                return MUSICPACK_ERR_INVALID;
            if (!get_req_string(item, "role", &m->artwork[i].role, &status))
                return status;
            if (!parse_asset(item, &m->artwork[i].asset, &status))
                return status;
            i++;
        }
        m->artwork_count = (size_t) i;
    }
#define PARSE_ASSET_ARRAY(key, field, countfield, maxfield)                      \
    v = cJSON_GetObjectItemCaseSensitive(root, key);                           \
    if (v != 0) {                                                              \
        if (!cJSON_IsArray(v))                                                 \
            return MUSICPACK_ERR_INVALID;                                      \
        n = cJSON_GetArraySize(v);                                             \
        if (n > maxfield)                                                      \
            return MUSICPACK_ERR_INVALID;                                      \
        m->field = (musicpack_asset *) calloc((size_t) n, sizeof *m->field);   \
        if (m->field == 0)                                                     \
            return MUSICPACK_ERR_NOMEM;                                        \
        i = 0;                                                                 \
        cJSON_ArrayForEach(item, v) {                                          \
            if (!parse_asset(item, &m->field[i], &status))                     \
                return status;                                                 \
            i++;                                                               \
        }                                                                      \
        m->countfield = (size_t) i;                                            \
    }
    PARSE_ASSET_ARRAY("booklet", booklet, booklet_count, MUSICPACK_MANIFEST_MAX_BOOKLET);
    PARSE_ASSET_ARRAY("lyrics", lyrics, lyrics_count, MUSICPACK_MANIFEST_MAX_LYRICS);
    PARSE_ASSET_ARRAY("extras", extras, extras_count, MUSICPACK_MANIFEST_MAX_EXTRAS);
#undef PARSE_ASSET_ARRAY

    /* analysis: optional typed references; unknown types stay forward-
       compatible (structurally validated only). */
    v = cJSON_GetObjectItemCaseSensitive(root, "analysis");
    if (v != 0) {
        cJSON *item;
        if (!cJSON_IsArray(v))
            return MUSICPACK_ERR_INVALID;
        n = cJSON_GetArraySize(v);
        if (n > MUSICPACK_MANIFEST_MAX_ANALYSIS)
            return MUSICPACK_ERR_INVALID;
        m->analysis = (musicpack_analysis *) calloc((size_t) n, sizeof *m->analysis);
        if (m->analysis == 0)
            return MUSICPACK_ERR_NOMEM;
        i = 0;
        cJSON_ArrayForEach(item, v) {
            musicpack_analysis *a = &m->analysis[i];
            if (!cJSON_IsObject(item))
                return MUSICPACK_ERR_INVALID;
            if (!get_req_string(item, "type", &a->type, &status))
                return status;
            if (!get_opt_string(item, "profile", &a->profile, &status))
                return status;
            if (!get_req_string(item, "path", &a->asset.path, &status))
                return status;
            if (musicpack_path_validate(a->asset.path) != MUSICPACK_OK) {
                status = MUSICPACK_ERR_PATH;
                return status;
            }
            if (!get_req_string(item, "sha256", &a->asset.sha256, &status))
                return status;
            if (!is_lower_hex(a->asset.sha256)) {
                status = MUSICPACK_ERR_INVALID;
                return status;
            }
            if (strcmp(a->type, "sonic") == 0 && a->profile == 0) {
                status = MUSICPACK_ERR_INVALID;
                return status;
            }
            i++;
        }
        m->analysis_count = (size_t) i;
    }

    /* album loudness */
    v = cJSON_GetObjectItemCaseSensitive(root, "loudness");
    if (v != 0) {
        int have_lufs = 0, have_peak = 0;
        if (!cJSON_IsObject(v))
            return MUSICPACK_ERR_INVALID;
        if (!get_opt_string(v, "algorithm", &m->loudness_algorithm, &status))
            return status;
        if (!get_opt_double(v, "albumLUFS", &have_lufs, &m->album_loudness.lufs,
                            musicpack_loudness_validate_lufs, &status))
            return status;
        if (!get_opt_double(v, "albumTruePeakDbTP", &have_peak,
                            &m->album_loudness.true_peak_db,
                            musicpack_loudness_validate_true_peak, &status))
            return status;
        if (have_lufs != have_peak) {
            status = MUSICPACK_ERR_INVALID;
            return status;
        }
        if (have_lufs)
            m->has_album_loudness = 1;
    }

    /* provenance */
    v = cJSON_GetObjectItemCaseSensitive(root, "provenance");
    if (v != 0) {
        if (!cJSON_IsObject(v))
            return MUSICPACK_ERR_INVALID;
        if (!get_opt_string(v, "tool", &m->provenance_tool, &status))
            return status;
        if (!get_opt_string(v, "toolVersion", &m->provenance_tool_version, &status))
            return status;
    }

    if (!check_dup_paths(m, &status))
        return status;

    return MUSICPACK_OK;
}

musicpack_manifest *
musicpack_manifest_parse(const char *json, musicpack_status *status)
{
    musicpack_manifest *m;
    cJSON *root;
    musicpack_status local = MUSICPACK_OK;
    size_t len;

    if (status == 0)
        status = &local;
    *status = MUSICPACK_OK;
    if (json == 0) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    len = strlen(json);
    if (len > 16u * 1024u * 1024u) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    root = cJSON_ParseWithLengthOpts(json, len + 1, 0, 1);
    if (root == 0) {
        *status = MUSICPACK_ERR_JSON;
        return 0;
    }

    m = (musicpack_manifest *) calloc(1, sizeof *m);
    if (m == 0) {
        cJSON_Delete(root);
        *status = MUSICPACK_ERR_NOMEM;
        return 0;
    }
    *status = musicpack_manifest_parse_tree(root, m);
    cJSON_Delete(root);
    if (*status != MUSICPACK_OK) {
        musicpack_manifest_free(m);
        return 0;
    }
    return m;
}

void
musicpack_manifest_clear(musicpack_manifest *m)
{
    size_t i;

    if (m == 0)
        return;
    free(m->album_title);
    for (i = 0; i < m->album_artist_count; i++) {
        free(m->album_artists[i].name);
        free(m->album_artists[i].role);
        free(m->album_artists[i].musicbrainz_id);
        free(m->album_artists[i].sort_name);
    }
    free(m->album_artists);
    free(m->release_type);
    free(m->original_release_date);
    for (i = 0; i < m->genre_count; i++)
        free(m->genres[i]);
    free(m->genres);
    free(m->release.release_date);
    free(m->release.edition);
    free(m->release.country);
    free(m->release.label);
    free(m->release.catalogue_number);
    free(m->release.notes);
    free(m->musicbrainz_release_group_id);
    free(m->musicbrainz_release_id);
    free(m->barcode);
    free(m->identity_source);
    free(m->identity_confidence);
    free(m->source_type);
    free(m->source_store);
    free(m->source_id);
    free(m->loudness_algorithm);

    for (i = 0; i < m->disc_count; i++) {
        musicpack_disc *d = &m->discs[i];
        size_t t;
        free(d->format);
        free(d->title);
        for (t = 0; t < d->track_count; t++) {
            musicpack_track *tr = &d->tracks[t];
            size_t a;
            free(tr->title);
            for (a = 0; a < tr->artist_count; a++) {
                free(tr->artists[a].name);
                free(tr->artists[a].role);
                free(tr->artists[a].musicbrainz_id);
                free(tr->artists[a].sort_name);
            }
            free(tr->artists);
            free(tr->isrc);
            free(tr->musicbrainz_track_id);
            free(tr->musicbrainz_recording_id);
            free(tr->source_store);
            free(tr->source_track_id);
            free(tr->source_audio_codec);
            free(tr->source_audio_md5);
            free(tr->audio.path);
            free(tr->audio.sha256);
            free(tr->audio_codec);
            free(tr->waveform.path);
            free(tr->waveform.sha256);
            free(tr->waveform.encoding);
            if (tr->representations != 0) {
                size_t r;
                for (r = 0; r < tr->representation_count; r++) {
                    free(tr->representations[r].path);
                    free(tr->representations[r].sha256);
                    free(tr->representations[r].label);
                    free(tr->representations[r].codec);
                }
            }
            free(tr->representations);
        }
        free(d->tracks);
    }
    free(m->discs);

    for (i = 0; i < m->artwork_count; i++) {
        free(m->artwork[i].role);
        free(m->artwork[i].asset.path);
        free(m->artwork[i].asset.sha256);
    }
    free(m->artwork);
#define FREE_ASSETS(field, count)                                              \
    for (i = 0; i < m->count; i++) { free(m->field[i].path); free(m->field[i].sha256); } \
    free(m->field);
    FREE_ASSETS(booklet, booklet_count);
    FREE_ASSETS(lyrics, lyrics_count);
    FREE_ASSETS(extras, extras_count);
#undef FREE_ASSETS

    for (i = 0; i < m->analysis_count; i++) {
        free(m->analysis[i].type);
        free(m->analysis[i].profile);
        free(m->analysis[i].asset.path);
        free(m->analysis[i].asset.sha256);
    }
    free(m->analysis);

    free(m->provenance_tool);
    free(m->provenance_tool_version);
}

void
musicpack_manifest_free(musicpack_manifest *m)
{
    if (m == 0)
        return;
    musicpack_manifest_clear(m);
    free(m);
}

/* ------------------------------------------------------------------ */
/* serialize                                                           */
/* ------------------------------------------------------------------ */

static cJSON *
artists_to_json(const musicpack_artist *artists, size_t count)
{
    cJSON *arr = cJSON_CreateArray();
    size_t i;
    for (i = 0; i < count; i++) {
        cJSON *o = cJSON_CreateObject();
        /* canonical credit key order is alphabetical:
           musicbrainzId, name, role, sortName */
        if (artists[i].musicbrainz_id != 0)
            cJSON_AddStringToObject(o, "musicbrainzId",
                                    artists[i].musicbrainz_id);
        cJSON_AddStringToObject(o, "name", artists[i].name);
        if (artists[i].role != 0)
            cJSON_AddStringToObject(o, "role", artists[i].role);
        if (artists[i].sort_name != 0)
            cJSON_AddStringToObject(o, "sortName", artists[i].sort_name);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

static cJSON *
asset_to_json(const musicpack_asset *a)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "path", a->path);
    cJSON_AddStringToObject(o, "sha256", a->sha256);
    return o;
}

static cJSON *
build_tree(const musicpack_manifest *m)
{
    cJSON *root, *o, *arr, *item;
    size_t i;

    root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "format", MUSICPACK_FORMAT);
    cJSON_AddNumberToObject(root, "version", MUSICPACK_VERSION_SCHEMA);

    o = cJSON_AddObjectToObject(root, "album");
    cJSON_AddStringToObject(o, "title", m->album_title);
    cJSON_AddItemToObject(o, "artists", artists_to_json(m->album_artists, m->album_artist_count));
    if (m->release_type != 0)
        cJSON_AddStringToObject(o, "releaseType", m->release_type);
    if (m->original_release_date != 0)
        cJSON_AddStringToObject(o, "originalReleaseDate", m->original_release_date);
    if (m->genre_count > 0) {
        cJSON *g = cJSON_AddArrayToObject(o, "genres");
        for (i = 0; i < m->genre_count; i++)
            cJSON_AddItemToArray(g, cJSON_CreateString(m->genres[i]));
    }

    if (m->release.present) {
        o = cJSON_AddObjectToObject(root, "release");
        if (m->release.release_date != 0)
            cJSON_AddStringToObject(o, "releaseDate", m->release.release_date);
        if (m->release.edition != 0)
            cJSON_AddStringToObject(o, "edition", m->release.edition);
        if (m->release.country != 0)
            cJSON_AddStringToObject(o, "country", m->release.country);
        if (m->release.label != 0)
            cJSON_AddStringToObject(o, "label", m->release.label);
        if (m->release.catalogue_number != 0)
            cJSON_AddStringToObject(o, "catalogueNumber", m->release.catalogue_number);
        if (m->release.notes != 0)
            cJSON_AddStringToObject(o, "notes", m->release.notes);
    }

    if (m->musicbrainz_release_group_id != 0 || m->musicbrainz_release_id != 0 || m->barcode != 0) {
        o = cJSON_AddObjectToObject(root, "identifiers");
        if (m->musicbrainz_release_group_id != 0)
            cJSON_AddStringToObject(o, "musicbrainzReleaseGroupId", m->musicbrainz_release_group_id);
        if (m->musicbrainz_release_id != 0)
            cJSON_AddStringToObject(o, "musicbrainzReleaseId", m->musicbrainz_release_id);
        if (m->barcode != 0)
            cJSON_AddStringToObject(o, "barcode", m->barcode);
    }
    if (m->identity_source != 0 || m->identity_confidence != 0) {
        o = cJSON_AddObjectToObject(root, "identity");
        if (m->identity_source != 0)
            cJSON_AddStringToObject(o, "source", m->identity_source);
        if (m->identity_confidence != 0)
            cJSON_AddStringToObject(o, "confidence", m->identity_confidence);
    }
    if (m->source_type != 0 || m->source_store != 0 || m->source_id != 0) {
        o = cJSON_AddObjectToObject(root, "source");
        if (m->source_type != 0)
            cJSON_AddStringToObject(o, "type", m->source_type);
        if (m->source_store != 0)
            cJSON_AddStringToObject(o, "store", m->source_store);
        if (m->source_id != 0)
            cJSON_AddStringToObject(o, "sourceId", m->source_id);
    }

    arr = cJSON_AddArrayToObject(root, "media");
    for (i = 0; i < m->disc_count; i++) {
        const musicpack_disc *d = &m->discs[i];
        size_t t;
        o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "disc", d->disc);
        if (d->format != 0)
            cJSON_AddStringToObject(o, "format", d->format);
        if (d->title != 0)
            cJSON_AddStringToObject(o, "title", d->title);
        item = cJSON_AddArrayToObject(o, "tracks");
        for (t = 0; t < d->track_count; t++) {
            const musicpack_track *tr = &d->tracks[t];
            cJSON *to = cJSON_CreateObject();
            cJSON_AddNumberToObject(to, "track", tr->number);
            cJSON_AddStringToObject(to, "title", tr->title);
            if (tr->artist_count > 0)
                cJSON_AddItemToObject(to, "artists",
                                      artists_to_json(tr->artists, tr->artist_count));
            if (tr->isrc != 0 || tr->musicbrainz_track_id != 0 ||
                tr->musicbrainz_recording_id != 0) {
                cJSON *id = cJSON_AddObjectToObject(to, "identifiers");
                if (tr->isrc != 0)
                    cJSON_AddStringToObject(id, "isrc", tr->isrc);
                if (tr->musicbrainz_track_id != 0)
                    cJSON_AddStringToObject(id, "musicbrainzTrackId", tr->musicbrainz_track_id);
                if (tr->musicbrainz_recording_id != 0)
                    cJSON_AddStringToObject(id, "musicbrainzRecordingId", tr->musicbrainz_recording_id);
            }
            if (tr->source_store != 0 || tr->source_track_id != 0) {
                cJSON *src = cJSON_AddObjectToObject(to, "source");
                if (tr->source_store != 0)
                    cJSON_AddStringToObject(src, "store", tr->source_store);
                if (tr->source_track_id != 0)
                    cJSON_AddStringToObject(src, "trackId", tr->source_track_id);
            }
            if (tr->source_audio_codec != 0 || tr->source_audio_md5 != 0) {
                cJSON *sa = cJSON_AddObjectToObject(to, "sourceAudio");
                if (tr->source_audio_codec != 0)
                    cJSON_AddStringToObject(sa, "codec", tr->source_audio_codec);
                if (tr->source_audio_md5 != 0)
                    cJSON_AddStringToObject(sa, "md5", tr->source_audio_md5);
            }
            if (tr->has_duration)
                cJSON_AddNumberToObject(to, "duration", tr->duration);
            if (tr->loudness.present) {
                cJSON *lo = cJSON_AddObjectToObject(to, "loudness");
                cJSON_AddNumberToObject(lo, "trackLUFS", tr->loudness.lufs);
                cJSON_AddNumberToObject(lo, "truePeakDbTP", tr->loudness.true_peak_db);
            }
            cJSON_AddItemToObject(to, "audio", asset_to_json(&tr->audio));
            if (tr->audio_codec != 0)
                cJSON_AddStringToObject(cJSON_GetObjectItemCaseSensitive(to, "audio"),
                                        "codec", tr->audio_codec);
            if (tr->waveform.present) {
                cJSON *wf = cJSON_AddObjectToObject(to, "waveform");
                cJSON_AddNumberToObject(wf, "version", tr->waveform.version);
                cJSON_AddStringToObject(wf, "path", tr->waveform.path);
                cJSON_AddStringToObject(wf, "sha256", tr->waveform.sha256);
                cJSON_AddNumberToObject(wf, "intervalMs", tr->waveform.interval_ms);
                cJSON_AddStringToObject(wf, "encoding", tr->waveform.encoding);
                cJSON_AddNumberToObject(wf, "floorDb", tr->waveform.floor_db);
                cJSON_AddNumberToObject(wf, "points", (double) tr->waveform.points);
            }
            if (tr->representation_count > 0) {
                size_t r;
                cJSON *rarr = cJSON_AddArrayToObject(to, "representations");
                for (r = 0; r < tr->representation_count; r++) {
                    const musicpack_representation *rep = &tr->representations[r];
                    cJSON *ro = cJSON_CreateObject();
                    cJSON_AddItemToObject(ro, "path", cJSON_CreateString(rep->path));
                    cJSON_AddItemToObject(ro, "sha256", cJSON_CreateString(rep->sha256));
                    if (rep->label != 0)
                        cJSON_AddStringToObject(ro, "label", rep->label);
                    if (rep->codec != 0)
                        cJSON_AddStringToObject(ro, "codec", rep->codec);
                    cJSON_AddItemToArray(rarr, ro);
                }
            }
            cJSON_AddItemToArray(item, to);
        }
        cJSON_AddItemToArray(arr, o);
    }

    if (m->artwork_count > 0) {
        arr = cJSON_AddArrayToObject(root, "artwork");
        for (i = 0; i < m->artwork_count; i++) {
            cJSON *w = cJSON_CreateObject();
            cJSON_AddStringToObject(w, "role", m->artwork[i].role);
            cJSON_AddItemToObject(w, "path", cJSON_CreateString(m->artwork[i].asset.path));
            cJSON_AddStringToObject(w, "sha256", m->artwork[i].asset.sha256);
            cJSON_AddItemToArray(arr, w);
        }
    }
#define ADD_ASSET_ARRAY(key, field, count)                                     \
    if (m->count > 0) {                                                        \
        arr = cJSON_AddArrayToObject(root, key);                               \
        for (i = 0; i < m->count; i++)                                         \
            cJSON_AddItemToArray(arr, asset_to_json(&m->field[i]));            \
    }
    ADD_ASSET_ARRAY("booklet", booklet, booklet_count);
    ADD_ASSET_ARRAY("lyrics", lyrics, lyrics_count);
    ADD_ASSET_ARRAY("extras", extras, extras_count);
#undef ADD_ASSET_ARRAY

    if (m->analysis_count > 0) {
        arr = cJSON_AddArrayToObject(root, "analysis");
        for (i = 0; i < m->analysis_count; i++) {
            cJSON *an = cJSON_CreateObject();
            cJSON_AddStringToObject(an, "type", m->analysis[i].type);
            if (m->analysis[i].profile != 0)
                cJSON_AddStringToObject(an, "profile", m->analysis[i].profile);
            cJSON_AddStringToObject(an, "path", m->analysis[i].asset.path);
            cJSON_AddStringToObject(an, "sha256", m->analysis[i].asset.sha256);
            cJSON_AddItemToArray(arr, an);
        }
    }

    if (m->has_album_loudness) {
        o = cJSON_AddObjectToObject(root, "loudness");
        if (m->loudness_algorithm != 0)
            cJSON_AddStringToObject(o, "algorithm", m->loudness_algorithm);
        cJSON_AddNumberToObject(o, "albumLUFS", m->album_loudness.lufs);
        cJSON_AddNumberToObject(o, "albumTruePeakDbTP", m->album_loudness.true_peak_db);
    }
    if (m->provenance_tool != 0 || m->provenance_tool_version != 0) {
        o = cJSON_AddObjectToObject(root, "provenance");
        if (m->provenance_tool != 0)
            cJSON_AddStringToObject(o, "tool", m->provenance_tool);
        if (m->provenance_tool_version != 0)
            cJSON_AddStringToObject(o, "toolVersion", m->provenance_tool_version);
    }

    return root;
}

static musicpack_status
validate_for_write(const musicpack_manifest *m)
{
    cJSON *tree;
    musicpack_manifest checked;
    musicpack_status status;
    size_t i, d, t;

    if (m->album_title == 0 || m->album_artists == 0 || m->album_artist_count == 0 ||
        m->discs == 0 || m->disc_count == 0)
        return MUSICPACK_ERR_INVALID;
    for (i = 0; i < m->album_artist_count; i++)
        if (m->album_artists[i].name == 0)
            return MUSICPACK_ERR_INVALID;
    for (i = 0; i < m->genre_count; i++)
        if (m->genres == 0 || m->genres[i] == 0)
            return MUSICPACK_ERR_INVALID;
    for (d = 0; d < m->disc_count; d++) {
        const musicpack_disc *disc = &m->discs[d];
        if (disc->tracks == 0 || disc->track_count == 0)
            return MUSICPACK_ERR_INVALID;
        for (t = 0; t < disc->track_count; t++) {
            const musicpack_track *track = &disc->tracks[t];
            size_t r;
            if (track->title == 0 || track->audio.path == 0 || track->audio.sha256 == 0)
                return MUSICPACK_ERR_INVALID;
            for (i = 0; i < track->artist_count; i++)
                if (track->artists == 0 || track->artists[i].name == 0)
                    return MUSICPACK_ERR_INVALID;
            for (r = 0; r < track->representation_count; r++) {
                const musicpack_representation *rep = &track->representations[r];
                if (track->representations == 0 || rep->path == 0 ||
                    rep->sha256 == 0)
                    return MUSICPACK_ERR_INVALID;
            }
        }
    }
    for (i = 0; i < m->artwork_count; i++)
        if (m->artwork == 0 || m->artwork[i].role == 0 ||
            m->artwork[i].asset.path == 0 || m->artwork[i].asset.sha256 == 0)
            return MUSICPACK_ERR_INVALID;
#define VALIDATE_ASSETS(field, count)                                          \
    for (i = 0; i < m->count; i++)                                             \
        if (m->field == 0 || m->field[i].path == 0 || m->field[i].sha256 == 0) \
            return MUSICPACK_ERR_INVALID;
    VALIDATE_ASSETS(booklet, booklet_count);
    VALIDATE_ASSETS(lyrics, lyrics_count);
    VALIDATE_ASSETS(extras, extras_count);
#undef VALIDATE_ASSETS
    for (i = 0; i < m->analysis_count; i++)
        if (m->analysis == 0 || m->analysis[i].type == 0 ||
            m->analysis[i].asset.path == 0 || m->analysis[i].asset.sha256 == 0)
            return MUSICPACK_ERR_INVALID;

    tree = build_tree(m);
    if (tree == 0)
        return MUSICPACK_ERR_NOMEM;
    memset(&checked, 0, sizeof checked);
    status = musicpack_manifest_parse_tree(tree, &checked);
    musicpack_manifest_clear(&checked);
    cJSON_Delete(tree);
    return status;
}

/* ------------------------------------------------------------------ */
/* canonical JSON printer (2-space indent, compact numbers)           */
/* ------------------------------------------------------------------ */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} json_out;

static int
json_put(json_out *o, const char *s, size_t n)
{
    if (o->len + n + 1 > o->cap) {
        size_t newcap = o->cap == 0 ? 256 : o->cap * 2;
        while (newcap < o->len + n + 1)
            newcap *= 2;
        {
            char *nb = (char *) realloc(o->buf, newcap);
            if (nb == 0)
                return -1;
            o->buf = nb;
            o->cap = newcap;
        }
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = '\0';
    return 0;
}

static int
json_indent(json_out *o, int depth)
{
    int i;
    for (i = 0; i < depth; i++)
        if (json_put(o, "  ", 2) != 0)
            return -1;
    return 0;
}

static void
json_number(json_out *o, double v)
{
    char tmp[64];
    if (v == (double) (long long) v && v >= -9.2e18 && v <= 9.2e18) {
        snprintf(tmp, sizeof tmp, "%lld", (long long) v);
    } else {
        snprintf(tmp, sizeof tmp, "%.8g", v);
    }
    json_put(o, tmp, strlen(tmp));
}

static void
json_string(json_out *o, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    json_put(o, "\"", 1);
    if (s != 0) {
        const unsigned char *p = (const unsigned char *) s;
        while (*p != '\0') {
            unsigned char c = *p;
            if (c == '"' || c == '\\') {
                char esc[2] = { '\\', (char) c };
                json_put(o, esc, 2);
            } else if (c < 0x20) {
                char esc[6];
                esc[0] = '\\'; esc[1] = 'u'; esc[2] = '0'; esc[3] = '0';
                esc[4] = hex[c >> 4]; esc[5] = hex[c & 0xF];
                json_put(o, esc, 6);
            } else {
                json_put(o, (const char *) p, 1);
            }
            p++;
        }
    }
    json_put(o, "\"", 1);
}

static void
json_print_value(json_out *o, const cJSON *item, int depth)
{
    const cJSON *child;
    switch (item->type & 0xFF) {
    case cJSON_NULL: json_put(o, "null", 4); break;
    case cJSON_False: json_put(o, "false", 5); break;
    case cJSON_True: json_put(o, "true", 4); break;
    case cJSON_Number: json_number(o, item->valuedouble); break;
    case cJSON_String: json_string(o, item->valuestring); break;
    case cJSON_Array:
        if (cJSON_GetArraySize(item) == 0) {
            json_put(o, "[]", 2);
            break;
        }
        json_put(o, "[\n", 2);
        cJSON_ArrayForEach(child, item) {
            json_indent(o, depth + 1);
            json_print_value(o, child, depth + 1);
            json_put(o, child->next != 0 ? ",\n" : "\n", child->next != 0 ? 2 : 1);
        }
        json_indent(o, depth);
        json_put(o, "]", 1);
        break;
    case cJSON_Object:
        if (cJSON_GetArraySize(item) == 0) {
            json_put(o, "{}", 2);
            break;
        }
        json_put(o, "{\n", 2);
        cJSON_ArrayForEach(child, item) {
            json_indent(o, depth + 1);
            json_string(o, child->string);
            json_put(o, ": ", 2);
            json_print_value(o, child, depth + 1);
            json_put(o, child->next != 0 ? ",\n" : "\n", child->next != 0 ? 2 : 1);
        }
        json_indent(o, depth);
        json_put(o, "}", 1);
        break;
    default:
        json_put(o, "null", 4);
        break;
    }
}

static char *
musicpack_json_print(const cJSON *root)
{
    json_out o = { 0, 0, 0 };
    if (root == 0)
        return 0;
    json_print_value(&o, root, 0);
    json_put(&o, "\n", 1);
    return o.buf;
}

static int
is_package_field(const char *key)
{
    static const char *const fields[] = {
        "format", "version", "album", "release", "identifiers", "identity",
        "source", "media", "artwork", "booklet", "lyrics", "extras", "analysis",
        "loudness", "provenance"
    };
    return is_in_string_list(key, fields, sizeof fields / sizeof *fields);
}

static int
copy_package_extensions(cJSON *dst, const cJSON *original)
{
    const cJSON *child;

    if (!cJSON_IsObject(original))
        return 1;
    cJSON_ArrayForEach(child, original) {
        cJSON *copy;
        if (is_package_field(child->string))
            continue;
        copy = cJSON_Duplicate(child, 1);
        if (copy == 0 || !cJSON_AddItemToObject(dst, child->string, copy)) {
            cJSON_Delete(copy);
            return 0;
        }
    }
    return 1;
}

musicpack_status
musicpack_manifest_write_with_original(const musicpack_manifest *m,
                                       const cJSON *original, char **json_out)
{
    cJSON *tree;

    if (m == 0 || json_out == 0)
        return MUSICPACK_ERR_INVALID;
    *json_out = 0;
    {
        musicpack_status status = validate_for_write(m);
        if (status != MUSICPACK_OK)
            return status;
    }
    tree = build_tree(m);
    if (tree == 0)
        return MUSICPACK_ERR_NOMEM;

    if (original != 0) {
        /* Only root extensions are retained. Nested extensions cannot be
           safely associated after callers shrink or reorder typed arrays. */
        if (!copy_package_extensions(tree, original)) {
            cJSON_Delete(tree);
            return MUSICPACK_ERR_NOMEM;
        }
    }

    *json_out = musicpack_json_print(tree);
    cJSON_Delete(tree);
    return *json_out != 0 ? MUSICPACK_OK : MUSICPACK_ERR_NOMEM;
}

musicpack_status
musicpack_manifest_write(const musicpack_manifest *m, char **json_out)
{
    return musicpack_manifest_write_with_original(m, 0, json_out);
}
