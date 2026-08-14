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
/// \file sonic.c
/// `musicpack-sonic` container document v1: profile registry, base64-f32le
/// vector handling, parse and validate. The single authority for Sonic
/// semantics in MusicPack (see `specs/musicpack-sonic-v1.md`).

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include <musicpack/sonic.h>

/* ------------------------------------------------------------------ */
/* profile registry                                                    */
/* ------------------------------------------------------------------ */

static const musicpack_sonic_profile REGISTRY[] = {
    { "musicpack-sonic-openl3-v1", 512, "cosine", "base64-f32le",
      MUSICPACK_SONIC_PROFILE_SUPPORTED },
    { "musicpack-sonic-discogs-v1", 1280, "cosine", "base64-f32le",
      MUSICPACK_SONIC_PROFILE_RESERVED },
    { "musicpack-sonic-clap-v1", 512, "cosine", "base64-f32le",
      MUSICPACK_SONIC_PROFILE_RESERVED },
};
#define REGISTRY_COUNT (sizeof REGISTRY / sizeof *REGISTRY)

const musicpack_sonic_profile *
musicpack_sonic_profile_get(const char *id)
{
    size_t i;
    if (id == 0)
        return 0;
    for (i = 0; i < REGISTRY_COUNT; i++)
        if (strcmp(REGISTRY[i].id, id) == 0)
            return &REGISTRY[i];
    return 0;
}

int
musicpack_sonic_profile_id_valid(const char *id)
{
    const char *prefix = "musicpack-sonic-";
    const char *p, *seg_start, *dash, *last;
    size_t plen = strlen(prefix);
    int segments = 0;

    if (id == 0 || strncmp(id, prefix, plen) != 0)
        return 0;
    p = id + plen;
    if (*p == '\0')
        return 0;
    for (;;) {
        seg_start = p;
        while (*p != '\0' && *p != '-') {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')))
                return 0;
            p++;
        }
        if (p == seg_start)
            return 0; /* empty segment ("--" or trailing "-") */
        segments++;
        if (*p == '\0')
            break;
        p++;
        if (*p == '\0')
            return 0; /* trailing '-' */
    }
    if (segments < 2)
        return 0;

    /* final segment must be v<digits> */
    dash = strrchr(id + plen, '-');
    last = dash != 0 ? dash + 1 : id + plen;
    if (last[0] != 'v' || last[1] == '\0')
        return 0;
    for (p = last + 1; *p != '\0'; p++)
        if (!(*p >= '0' && *p <= '9'))
            return 0;
    return 1;
}

int
musicpack_sonic_encoding_supported(const char *encoding)
{
    return encoding != 0 && strcmp(encoding, MUSICPACK_SONIC_ENCODING) == 0;
}

/* ------------------------------------------------------------------ */
/* vectors                                                             */
/* ------------------------------------------------------------------ */

musicpack_status
musicpack_sonic_vector_validate(const float *v, size_t n, double tolerance)
{
    size_t i;
    double sum = 0.0;

    if (v == 0)
        return MUSICPACK_ERR_INVALID;
    for (i = 0; i < n; i++) {
        double d = (double) v[i];
        if (!isfinite(d))
            return MUSICPACK_ERR_INVALID;
        sum += d * d;
    }
    if (fabs(sqrt(sum) - 1.0) > tolerance)
        return MUSICPACK_ERR_INVALID;
    return MUSICPACK_OK;
}

musicpack_status
musicpack_sonic_vector_decode(const char *base64, size_t n, size_t dimensions,
                              float **out, size_t *out_count)
{
    unsigned char *raw;
    size_t raw_len;
    float *v;
    musicpack_status s;

    if (base64 == 0 || out == 0 || dimensions == 0 ||
        dimensions > MUSICPACK_SONIC_MAX_DIMENSIONS)
        return MUSICPACK_ERR_INVALID;
    if (!musicpack_base64_decode(base64, n, &raw, &raw_len))
        return MUSICPACK_ERR_INVALID;
    if (raw_len != dimensions * sizeof(float)) {
        free(raw);
        return MUSICPACK_ERR_INVALID;
    }
    v = (float *) malloc(raw_len);
    if (v == 0) {
        free(raw);
        return MUSICPACK_ERR_NOMEM;
    }
    memcpy(v, raw, raw_len);
    free(raw);
    s = musicpack_sonic_vector_validate(v, dimensions, MUSICPACK_SONIC_NORM_TOLERANCE);
    if (s != MUSICPACK_OK) {
        free(v);
        return s;
    }
    *out = v;
    if (out_count != 0)
        *out_count = dimensions;
    return MUSICPACK_OK;
}

musicpack_status
musicpack_sonic_vector_encode(const float *v, size_t n, char **out)
{
    unsigned char *raw;
    char *b64;

    if (v == 0 || out == 0)
        return MUSICPACK_ERR_INVALID;
    raw = (unsigned char *) malloc(n * sizeof(float));
    if (raw == 0)
        return MUSICPACK_ERR_NOMEM;
    memcpy(raw, v, n * sizeof(float));
    if (!musicpack_base64_encode(raw, n * sizeof(float), &b64)) {
        free(raw);
        return MUSICPACK_ERR_NOMEM;
    }
    free(raw);
    *out = b64;
    return MUSICPACK_OK;
}

/* ------------------------------------------------------------------ */
/* document parse                                                      */
/* ------------------------------------------------------------------ */

static void
embedding_clear(musicpack_sonic_embedding *e)
{
    free(e->data);
    e->data = 0;
    e->dimensions = 0;
    e->present = 0;
}

/* Parses an embedding value: NULL => null (present=0); object => validate
   encoding/dimensions/data against the document profile. */
static int
parse_embedding(cJSON *v, const char *profile_encoding, size_t profile_dims,
                musicpack_sonic_embedding *out, musicpack_status *status)
{
    cJSON *enc, *dims, *data;

    memset(out, 0, sizeof *out);
    if (v == 0 || cJSON_IsNull(v))
        return 1; /* null embedding: valid */
    if (!cJSON_IsObject(v))
        return 0;

    enc = cJSON_GetObjectItemCaseSensitive(v, "encoding");
    dims = cJSON_GetObjectItemCaseSensitive(v, "dimensions");
    data = cJSON_GetObjectItemCaseSensitive(v, "data");
    if (!cJSON_IsString(enc) || !cJSON_IsNumber(dims) || !cJSON_IsString(data))
        return 0;
    if (strcmp(enc->valuestring, profile_encoding) != 0)
        return 0;
    if ((size_t) dims->valuedouble != profile_dims ||
        dims->valuedouble != (double) (size_t) dims->valuedouble)
        return 0;

    *status = musicpack_sonic_vector_decode(data->valuestring,
                                            strlen(data->valuestring),
                                            profile_dims, &out->data, 0);
    if (*status != MUSICPACK_OK)
        return 0;
    out->present = 1;
    out->dimensions = profile_dims;
    return 1;
}

static int
get_required_string(cJSON *o, const char *key, char **field, musicpack_status *status)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
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
get_required_int(cJSON *o, const char *key, int *out, int minimum, musicpack_status *status)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v) || v->valuedouble < (double) minimum ||
        v->valuedouble != (double) (int) v->valuedouble) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    *out = (int) v->valuedouble;
    return 1;
}

static musicpack_sonic *
parse_tree(const cJSON *root, musicpack_status *status)
{
    cJSON *v, *profile, *analyzer, *album, *tracks;
    musicpack_sonic *s;
    musicpack_status local = MUSICPACK_OK;
    size_t i = 0;
    int dims = 0;

    if (status == 0)
        status = &local;

    v = cJSON_GetObjectItemCaseSensitive(root, "format");
    if (!cJSON_IsString(v) || strcmp(v->valuestring, MUSICPACK_SONIC_FORMAT) != 0) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    v = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (!cJSON_IsNumber(v) || (int) v->valuedouble != MUSICPACK_SONIC_VERSION ||
        v->valuedouble != (double) (int) v->valuedouble) {
        *status = MUSICPACK_ERR_VERSION;
        return 0;
    }

    s = (musicpack_sonic *) calloc(1, sizeof *s);
    if (s == 0) {
        *status = MUSICPACK_ERR_NOMEM;
        return 0;
    }

    profile = cJSON_GetObjectItemCaseSensitive(root, "profile");
    if (!cJSON_IsObject(profile)) {
        *status = MUSICPACK_ERR_INVALID;
        goto fail;
    }
    if (!get_required_string(profile, "id", &s->profile_id, status))
        goto fail;
    if (!musicpack_sonic_profile_id_valid(s->profile_id)) {
        *status = MUSICPACK_ERR_INVALID;
        goto fail;
    }
    if (!get_required_string(profile, "distance", &s->distance, status))
        goto fail;
    if (!get_required_string(profile, "encoding", &s->encoding, status))
        goto fail;
    if (!musicpack_sonic_encoding_supported(s->encoding)) {
        *status = MUSICPACK_ERR_INVALID;
        goto fail;
    }
    if (!get_required_int(profile, "dimensions", &dims, 1, status))
        goto fail;
    s->dimensions = (size_t) dims;
    if (s->dimensions > MUSICPACK_SONIC_MAX_DIMENSIONS) {
        *status = MUSICPACK_ERR_INVALID;
        goto fail;
    }

    analyzer = cJSON_GetObjectItemCaseSensitive(root, "analyzer");
    if (!cJSON_IsObject(analyzer)) {
        *status = MUSICPACK_ERR_INVALID;
        goto fail;
    }
    if (!get_required_string(analyzer, "tool", &s->analyzer_tool, status))
        goto fail;
    if (!get_required_string(analyzer, "toolVersion", &s->analyzer_tool_version, status))
        goto fail;

    album = cJSON_GetObjectItemCaseSensitive(root, "album");
    if (!cJSON_IsObject(album)) {
        *status = MUSICPACK_ERR_INVALID;
        goto fail;
    }
    if (!get_required_int(album, "tracksContributing", &s->album_tracks_contributing, 0, status))
        goto fail;
    {
        cJSON *emb = cJSON_GetObjectItemCaseSensitive(album, "embedding");
        if (!parse_embedding(emb, s->encoding, s->dimensions, &s->album, status))
            goto fail;
    }

    tracks = cJSON_GetObjectItemCaseSensitive(root, "tracks");
    if (!cJSON_IsArray(tracks)) {
        *status = MUSICPACK_ERR_INVALID;
        goto fail;
    }
    if (cJSON_GetArraySize(tracks) > MUSICPACK_SONIC_MAX_TRACKS) {
        *status = MUSICPACK_ERR_INVALID;
        goto fail;
    }
    if (cJSON_GetArraySize(tracks) > 0) {
        cJSON *item;
        s->tracks = (musicpack_sonic_track *) calloc(
            (size_t) cJSON_GetArraySize(tracks), sizeof *s->tracks);
        if (s->tracks == 0) {
            *status = MUSICPACK_ERR_NOMEM;
            goto fail;
        }
        cJSON_ArrayForEach(item, tracks) {
            cJSON *emb;
            if (!cJSON_IsObject(item)) {
                *status = MUSICPACK_ERR_INVALID;
                goto fail;
            }
            if (!get_required_int(item, "disc", &s->tracks[i].disc, 1, status))
                goto fail;
            if (!get_required_int(item, "track", &s->tracks[i].track, 1, status))
                goto fail;
            emb = cJSON_GetObjectItemCaseSensitive(item, "embedding");
            if (!parse_embedding(emb, s->encoding, s->dimensions,
                                 &s->tracks[i].embedding, status))
                goto fail;
            i++;
        }
        s->track_count = i;
    }
    return s;

fail:
    musicpack_sonic_free(s);
    return 0;
}

musicpack_sonic *
musicpack_sonic_parse(const char *json, size_t len, musicpack_status *status)
{
    musicpack_status local = MUSICPACK_OK;
    cJSON *root;
    musicpack_sonic *s;

    if (status == 0)
        status = &local;
    *status = MUSICPACK_OK;
    if (json == 0) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    if (len > MUSICPACK_SONIC_DOC_MAX) {
        *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    root = cJSON_ParseWithLength(json, len);
    if (root == 0) {
        *status = MUSICPACK_ERR_JSON;
        return 0;
    }
    s = parse_tree(root, status);
    cJSON_Delete(root);
    return s;
}

void
musicpack_sonic_free(musicpack_sonic *s)
{
    size_t i;
    if (s == 0)
        return;
    free(s->profile_id);
    free(s->distance);
    free(s->encoding);
    free(s->analyzer_tool);
    free(s->analyzer_tool_version);
    embedding_clear(&s->album);
    for (i = 0; i < s->track_count; i++)
        embedding_clear(&s->tracks[i].embedding);
    free(s->tracks);
    free(s);
}

/* ------------------------------------------------------------------ */
/* document validation                                                 */
/* ------------------------------------------------------------------ */

musicpack_status
musicpack_sonic_validate(const musicpack_sonic *s, const musicpack_manifest *m,
                         musicpack_sonic_profile_state *profile_state)
{
    const musicpack_sonic_profile *reg;
    musicpack_sonic_profile_state state;
    size_t i, j, contributing = 0;

    if (s == 0)
        return MUSICPACK_ERR_INVALID;

    reg = musicpack_sonic_profile_get(s->profile_id);
    state = reg != 0 ? reg->state : MUSICPACK_SONIC_PROFILE_UNKNOWN;
    if (profile_state != 0)
        *profile_state = state;

    /* registered profiles must match their registry metadata */
    if (reg != 0) {
        if (s->dimensions != reg->dimensions ||
            strcmp(s->distance, reg->distance) != 0 ||
            strcmp(s->encoding, reg->encoding) != 0)
            return MUSICPACK_ERR_INVALID;
    }

    /* internal consistency: no duplicate (disc, track); contributor count;
       album embedding exactly when contributors exist */
    for (i = 0; i < s->track_count; i++) {
        for (j = i + 1; j < s->track_count; j++)
            if (s->tracks[i].disc == s->tracks[j].disc &&
                s->tracks[i].track == s->tracks[j].track)
                return MUSICPACK_ERR_INVALID;
        if (s->tracks[i].embedding.present)
            contributing++;
    }
    if ((size_t) s->album_tracks_contributing != contributing)
        return MUSICPACK_ERR_INVALID;
    if (contributing == 0 && s->album.present)
        return MUSICPACK_ERR_INVALID;
    if (contributing > 0 && !s->album.present)
        return MUSICPACK_ERR_INVALID;

    /* against the manifest: exactly one entry per manifest track */
    if (m != 0) {
        size_t mt = 0, d;
        for (d = 0; d < m->disc_count; d++)
            mt += m->discs[d].track_count;
        if (s->track_count != mt)
            return MUSICPACK_ERR_INVALID;
        for (d = 0; d < m->disc_count; d++) {
            const musicpack_disc *disc = &m->discs[d];
            size_t t;
            for (t = 0; t < disc->track_count; t++) {
                const musicpack_track *tr = &disc->tracks[t];
                int found = 0;
                for (i = 0; i < s->track_count; i++) {
                    if (s->tracks[i].disc == disc->disc &&
                        s->tracks[i].track == tr->number) {
                        found = 1;
                        break;
                    }
                }
                if (!found)
                    return MUSICPACK_ERR_INVALID;
            }
        }
    }
    return MUSICPACK_OK;
}
