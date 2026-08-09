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
/// \file draft.c
/// Authoring-draft JSON for the MusicPack Author GUI: draft <-> manifest
/// conversion, draft serialization, and MusicBrainz candidate extraction.
///
/// The draft is application state, not a MusicPack format. libmusicpack
/// remains the only authority on .mpack semantics; this module only shapes
/// the JSON that crosses the `musicpack` CLI boundary.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>
#include <musicpack/manifest.h>
#include <musicpack/meta.h>

#include "draft.h"

#define DRAFT_SCHEMA "musicpack-draft"
#define DRAFT_VERSION 1
#define DRAFT_DOC_MAX (16u * 1024u * 1024u)

static const char *
jstr(cJSON *o, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return (v != 0 && cJSON_IsString(v)) ? v->valuestring : 0;
}

static int
jnum(cJSON *o, const char *key, int *out)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (v == 0 || !cJSON_IsNumber(v))
        return 0;
    *out = (int) v->valuedouble;
    return 1;
}

static char *
strdup_opt(const char *s)
{
    return s != 0 ? strdup(s) : 0;
}

static void
set_err(char *err, size_t cap, const char *msg)
{
    if (err != 0 && cap > 0)
        snprintf(err, cap, "%s", msg);
}

/* ------------------------------------------------------------------ */
/* file I/O                                                            */
/* ------------------------------------------------------------------ */

cJSON *
draft_read_json(const char *path, char *err, size_t err_cap)
{
    FILE *f;
    long len;
    char *buf;
    cJSON *root;

    if (err != 0 && err_cap > 0)
        err[0] = '\0';
    f = fopen(path, "rb");
    if (f == 0) {
        char msg[512];
        snprintf(msg, sizeof msg, "cannot read '%s'", path);
        set_err(err, err_cap, msg);
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        set_err(err, err_cap, "cannot size draft file");
        return 0;
    }
    len = ftell(f);
    if (len < 0 || (size_t) len > DRAFT_DOC_MAX) {
        fclose(f);
        set_err(err, err_cap, "draft file too large or unreadable");
        return 0;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        set_err(err, err_cap, "cannot rewind draft file");
        return 0;
    }
    buf = (char *) malloc((size_t) len + 1);
    if (buf == 0) {
        fclose(f);
        set_err(err, err_cap, "out of memory");
        return 0;
    }
    if (len > 0 && fread(buf, 1, (size_t) len, f) != (size_t) len) {
        free(buf);
        fclose(f);
        set_err(err, err_cap, "short read on draft file");
        return 0;
    }
    fclose(f);
    buf[len] = '\0';
    root = cJSON_ParseWithLength(buf, (size_t) len);
    free(buf);
    if (root == 0) {
        set_err(err, err_cap, "draft is not valid JSON");
        return 0;
    }
    return root;
}

void
draft_print(cJSON *root)
{
    char *text = cJSON_Print(root);
    if (text != 0) {
        fputs(text, stdout);
        fputc('\n', stdout);
        free(text);
    }
}

/* ------------------------------------------------------------------ */
/* draft -> manifest                                                   */
/* ------------------------------------------------------------------ */

static int
parse_artists_json(cJSON *arr, musicpack_artist **out, size_t *count)
{
    cJSON *item;
    int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;

    *out = 0;
    *count = 0;
    if (n <= 0)
        return 1;
    *out = (musicpack_artist *) calloc((size_t) n, sizeof **out);
    if (*out == 0)
        return 0;
    cJSON_ArrayForEach(item, arr) {
        if (!cJSON_IsObject(item))
            continue;
        (*out)[*count].name = strdup_opt(jstr(item, "name"));
        (*out)[*count].role = strdup_opt(jstr(item, "role"));
        if ((*out)[*count].name == 0)
            return 0;
        (*count)++;
    }
    return 1;
}

static int
parse_asset_array(cJSON *arr, musicpack_asset **out, size_t *count)
{
    cJSON *item;
    int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;

    *out = 0;
    *count = 0;
    if (n <= 0)
        return 1;
    *out = (musicpack_asset *) calloc((size_t) n, sizeof **out);
    if (*out == 0)
        return 0;
    cJSON_ArrayForEach(item, arr) {
        const char *p = jstr(item, "path");
        if (p == 0 || *p == '\0')
            continue;
        (*out)[*count].path = strdup(p);
        if ((*out)[*count].path == 0)
            return 0;
        (*count)++;
    }
    return 1;
}

int
draft_to_manifest(cJSON *draft, musicpack_manifest *m)
{
    cJSON *album, *v, *item, *media;
    int i;

    if (draft == 0 || m == 0)
        return 0;
    memset(m, 0, sizeof *m);

    album = cJSON_GetObjectItemCaseSensitive(draft, "album");
    if (cJSON_IsObject(album)) {
        m->album_title = strdup_opt(jstr(album, "title"));
        v = cJSON_GetObjectItemCaseSensitive(album, "artists");
        if (v != 0 && !parse_artists_json(v, &m->album_artists, &m->album_artist_count))
            return 0;
        m->release_type = strdup_opt(jstr(album, "releaseType"));
        m->original_release_date = strdup_opt(jstr(album, "originalReleaseDate"));
        v = cJSON_GetObjectItemCaseSensitive(album, "genres");
        if (cJSON_IsArray(v) && cJSON_GetArraySize(v) > 0) {
            cJSON *g;
            int n = cJSON_GetArraySize(v);
            m->genres = (char **) calloc((size_t) n, sizeof *m->genres);
            if (m->genres == 0)
                return 0;
            i = 0;
            cJSON_ArrayForEach(g, v) {
                if (!cJSON_IsString(g))
                    continue;
                m->genres[i] = strdup(g->valuestring);
                if (m->genres[i] == 0)
                    return 0;
                i++;
            }
            m->genre_count = (size_t) i;
        }
    }

    v = cJSON_GetObjectItemCaseSensitive(draft, "release");
    if (cJSON_IsObject(v)) {
        m->release.present = 1;
        m->release.release_date = strdup_opt(jstr(v, "releaseDate"));
        m->release.edition = strdup_opt(jstr(v, "edition"));
        m->release.country = strdup_opt(jstr(v, "country"));
        m->release.label = strdup_opt(jstr(v, "label"));
        m->release.catalogue_number = strdup_opt(jstr(v, "catalogueNumber"));
        m->release.notes = strdup_opt(jstr(v, "notes"));
    }

    v = cJSON_GetObjectItemCaseSensitive(draft, "identifiers");
    if (cJSON_IsObject(v)) {
        m->musicbrainz_release_id = strdup_opt(jstr(v, "musicbrainzReleaseId"));
        m->musicbrainz_release_group_id = strdup_opt(jstr(v, "musicbrainzReleaseGroupId"));
        m->barcode = strdup_opt(jstr(v, "barcode"));
    }
    v = cJSON_GetObjectItemCaseSensitive(draft, "identity");
    if (cJSON_IsObject(v)) {
        m->identity_source = strdup_opt(jstr(v, "source"));
        m->identity_confidence = strdup_opt(jstr(v, "confidence"));
    }
    v = cJSON_GetObjectItemCaseSensitive(draft, "source");
    if (cJSON_IsObject(v)) {
        m->source_type = strdup_opt(jstr(v, "type"));
        m->source_store = strdup_opt(jstr(v, "store"));
        m->source_id = strdup_opt(jstr(v, "sourceId"));
    }

    media = cJSON_GetObjectItemCaseSensitive(draft, "media");
    if (cJSON_IsArray(media)) {
        int n = cJSON_GetArraySize(media);
        if (n > 0) {
            m->discs = (musicpack_disc *) calloc((size_t) n, sizeof *m->discs);
            if (m->discs == 0)
                return 0;
            m->disc_count = (size_t) n;
            i = 0;
            cJSON_ArrayForEach(item, media) {
                musicpack_disc *d = &m->discs[i++];
                cJSON *tracks, *tr;
                if (!cJSON_IsObject(item))
                    continue;
                jnum(item, "disc", &d->disc);
                d->format = strdup_opt(jstr(item, "format"));
                d->title = strdup_opt(jstr(item, "title"));
                tracks = cJSON_GetObjectItemCaseSensitive(item, "tracks");
                if (cJSON_IsArray(tracks)) {
                    int tn = cJSON_GetArraySize(tracks);
                    d->tracks = (musicpack_track *) calloc((size_t) tn, sizeof *d->tracks);
                    if (d->tracks == 0)
                        return 0;
                    cJSON_ArrayForEach(tr, tracks) {
                        musicpack_track *t = &d->tracks[d->track_count++];
                        cJSON *sub;
                        if (!cJSON_IsObject(tr))
                            continue;
                        jnum(tr, "track", &t->number);
                        t->title = strdup_opt(jstr(tr, "title"));
                        sub = cJSON_GetObjectItemCaseSensitive(tr, "artists");
                        if (sub != 0 && !parse_artists_json(sub, &t->artists, &t->artist_count))
                            return 0;
                        sub = cJSON_GetObjectItemCaseSensitive(tr, "identifiers");
                        if (cJSON_IsObject(sub)) {
                            t->isrc = strdup_opt(jstr(sub, "isrc"));
                            t->musicbrainz_track_id = strdup_opt(jstr(sub, "musicbrainzTrackId"));
                            t->musicbrainz_recording_id = strdup_opt(jstr(sub, "musicbrainzRecordingId"));
                        }
                        sub = cJSON_GetObjectItemCaseSensitive(tr, "source");
                        if (cJSON_IsObject(sub)) {
                            t->source_store = strdup_opt(jstr(sub, "store"));
                            t->source_track_id = strdup_opt(jstr(sub, "trackId"));
                        }
                        sub = cJSON_GetObjectItemCaseSensitive(tr, "sourceAudio");
                        if (cJSON_IsObject(sub)) {
                            t->source_audio_codec = strdup_opt(jstr(sub, "codec"));
                            t->source_audio_md5 = strdup_opt(jstr(sub, "md5"));
                        }
                        t->audio.path = strdup_opt(jstr(tr, "audioPath"));
                    }
                }
            }
        }
    }

    /* file-based artwork/booklet/lyrics/extras. Embedded artwork entries
       (no `path`) are skipped here and handled by build-draft directly. */
    v = cJSON_GetObjectItemCaseSensitive(draft, "artwork");
    if (cJSON_IsArray(v)) {
        cJSON *a;
        int n = cJSON_GetArraySize(v), k = 0;
        m->artwork = (musicpack_artwork *) calloc((size_t) n, sizeof *m->artwork);
        if (m->artwork == 0)
            return 0;
        cJSON_ArrayForEach(a, v) {
            const char *p = jstr(a, "path");
            if (p == 0 || *p == '\0')
                continue;
            m->artwork[k].role = strdup_opt(jstr(a, "role"));
            m->artwork[k].asset.path = strdup(p);
            if (m->artwork[k].role == 0 || m->artwork[k].asset.path == 0)
                return 0;
            k++;
        }
        m->artwork_count = (size_t) k;
    }
    v = cJSON_GetObjectItemCaseSensitive(draft, "booklet");
    if (v != 0 && !parse_asset_array(v, &m->booklet, &m->booklet_count))
        return 0;
    v = cJSON_GetObjectItemCaseSensitive(draft, "lyrics");
    if (v != 0 && !parse_asset_array(v, &m->lyrics, &m->lyrics_count))
        return 0;
    v = cJSON_GetObjectItemCaseSensitive(draft, "extras");
    if (v != 0 && !parse_asset_array(v, &m->extras, &m->extras_count))
        return 0;

    return 1;
}

/* ------------------------------------------------------------------ */
/* manifest -> draft (inspect)                                         */
/* ------------------------------------------------------------------ */

static cJSON *
artists_to_json(const musicpack_artist *artists, size_t count)
{
    cJSON *arr = cJSON_CreateArray();
    size_t i;
    for (i = 0; i < count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", artists[i].name);
        if (artists[i].role != 0)
            cJSON_AddStringToObject(o, "role", artists[i].role);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

static void
add_opt(cJSON *o, const char *key, const char *val)
{
    if (val != 0 && *val != '\0')
        cJSON_AddStringToObject(o, key, val);
}

static void
add_opt_array(cJSON *o, const char *key, const char *const *vals, size_t count)
{
    cJSON *arr;
    size_t i;
    if (count == 0)
        return;
    arr = cJSON_AddArrayToObject(o, key);
    for (i = 0; i < count; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateString(vals[i]));
}

static cJSON *
empty_asset_array(void)
{
    return cJSON_CreateArray();
}

cJSON *
draft_from_manifest(const musicpack_manifest *m, const char *source_root,
                    const mpc_stream_info *streams)
{
    cJSON *root, *album, *o, *media, *med, *trk;
    size_t d, t, si = 0, total = 0;
    char tmp[64];

    for (d = 0; d < m->disc_count; d++)
        total += m->discs[d].track_count;

    root = cJSON_CreateObject();
    if (root == 0)
        return 0;
    cJSON_AddStringToObject(root, "schema", DRAFT_SCHEMA);
    cJSON_AddNumberToObject(root, "version", DRAFT_VERSION);
    cJSON_AddStringToObject(root, "sourceRoot", source_root);

    album = cJSON_AddObjectToObject(root, "album");
    cJSON_AddStringToObject(album, "title", m->album_title != 0 ? m->album_title : "");
    cJSON_AddItemToObject(album, "artists",
                          artists_to_json(m->album_artists, m->album_artist_count));
    add_opt(album, "releaseType", m->release_type);
    add_opt(album, "originalReleaseDate", m->original_release_date);
    add_opt_array(album, "genres", (const char *const *) m->genres, m->genre_count);

    if (m->release.present) {
        o = cJSON_AddObjectToObject(root, "release");
        add_opt(o, "releaseDate", m->release.release_date);
        add_opt(o, "edition", m->release.edition);
        add_opt(o, "country", m->release.country);
        add_opt(o, "label", m->release.label);
        add_opt(o, "catalogueNumber", m->release.catalogue_number);
        add_opt(o, "notes", m->release.notes);
    }

    if (m->musicbrainz_release_group_id != 0 || m->musicbrainz_release_id != 0 ||
        m->barcode != 0) {
        o = cJSON_AddObjectToObject(root, "identifiers");
        add_opt(o, "musicbrainzReleaseGroupId", m->musicbrainz_release_group_id);
        add_opt(o, "musicbrainzReleaseId", m->musicbrainz_release_id);
        add_opt(o, "barcode", m->barcode);
    }
    if (m->identity_source != 0 || m->identity_confidence != 0) {
        o = cJSON_AddObjectToObject(root, "identity");
        add_opt(o, "source", m->identity_source);
        add_opt(o, "confidence", m->identity_confidence);
    }
    if (m->source_type != 0 || m->source_store != 0 || m->source_id != 0) {
        o = cJSON_AddObjectToObject(root, "source");
        add_opt(o, "type", m->source_type);
        add_opt(o, "store", m->source_store);
        add_opt(o, "sourceId", m->source_id);
    }

    media = cJSON_AddArrayToObject(root, "media");
    for (d = 0; d < m->disc_count; d++) {
        med = cJSON_CreateObject();
        cJSON_AddNumberToObject(med, "disc", m->discs[d].disc);
        add_opt(med, "format", m->discs[d].format);
        add_opt(med, "title", m->discs[d].title);
        trk = cJSON_AddArrayToObject(med, "tracks");
        for (t = 0; t < m->discs[d].track_count; t++) {
            const musicpack_track *tr = &m->discs[d].tracks[t];
            cJSON *x = cJSON_CreateObject();
            cJSON_AddNumberToObject(x, "track", tr->number);
            cJSON_AddStringToObject(x, "title", tr->title != 0 ? tr->title : "");
            if (tr->artist_count > 0)
                cJSON_AddItemToObject(x, "artists",
                                      artists_to_json(tr->artists, tr->artist_count));
            if (tr->isrc != 0 || tr->musicbrainz_track_id != 0 ||
                tr->musicbrainz_recording_id != 0) {
                o = cJSON_AddObjectToObject(x, "identifiers");
                add_opt(o, "isrc", tr->isrc);
                add_opt(o, "musicbrainzTrackId", tr->musicbrainz_track_id);
                add_opt(o, "musicbrainzRecordingId", tr->musicbrainz_recording_id);
            }
            if (tr->source_store != 0 || tr->source_track_id != 0) {
                o = cJSON_AddObjectToObject(x, "source");
                add_opt(o, "store", tr->source_store);
                add_opt(o, "trackId", tr->source_track_id);
            }
            if (tr->source_audio_codec != 0 || tr->source_audio_md5 != 0) {
                o = cJSON_AddObjectToObject(x, "sourceAudio");
                add_opt(o, "codec", tr->source_audio_codec);
                add_opt(o, "md5", tr->source_audio_md5);
            }
            if (streams != 0 && si < total) {
                const mpc_stream_info *p = &streams[si];
                if (p->codec[0] != '\0')
                    cJSON_AddStringToObject(x, "codec", p->codec);
                if (p->stream_version > 0)
                    cJSON_AddNumberToObject(x, "streamVersion", p->stream_version);
                if (p->sample_rate > 0)
                    cJSON_AddNumberToObject(x, "sampleRate", p->sample_rate);
                if (p->channels > 0)
                    cJSON_AddNumberToObject(x, "channels", p->channels);
                if (p->duration > 0) {
                    snprintf(tmp, sizeof tmp, "%.3f", p->duration);
                    cJSON_AddRawToObject(x, "duration", tmp);
                }
            }
            si++;
            cJSON_AddStringToObject(x, "audioPath",
                                    tr->audio.path != 0 ? tr->audio.path : "");
            cJSON_AddItemToArray(trk, x);
        }
        cJSON_AddItemToArray(media, med);
    }

    cJSON_AddItemToObject(root, "artwork", empty_asset_array());
    cJSON_AddItemToObject(root, "booklet", empty_asset_array());
    cJSON_AddItemToObject(root, "lyrics", empty_asset_array());
    cJSON_AddItemToObject(root, "extras", empty_asset_array());

    return root;
}

/* ------------------------------------------------------------------ */
/* manifest -> existing draft (identify)                               */
/* ------------------------------------------------------------------ */

int
draft_apply_manifest(cJSON *draft, const musicpack_manifest *m)
{
    cJSON *album, *release, *ids, *idn, *media;

    album = cJSON_GetObjectItemCaseSensitive(draft, "album");
    if (cJSON_IsObject(album)) {
        add_opt(album, "title", m->album_title);
        if (m->album_artist_count > 0) {
            cJSON_DeleteItemFromObject(album, "artists");
            cJSON_AddItemToObject(album, "artists",
                                  artists_to_json(m->album_artists, m->album_artist_count));
        }
        add_opt(album, "releaseType", m->release_type);
        add_opt(album, "originalReleaseDate", m->original_release_date);
        if (m->genre_count > 0) {
            cJSON_DeleteItemFromObject(album, "genres");
            add_opt_array(album, "genres",
                          (const char *const *) m->genres, m->genre_count);
        }
    }

    if (m->release.present) {
        release = cJSON_GetObjectItemCaseSensitive(draft, "release");
        if (!cJSON_IsObject(release))
            release = cJSON_AddObjectToObject(draft, "release");
        add_opt(release, "releaseDate", m->release.release_date);
        add_opt(release, "edition", m->release.edition);
        add_opt(release, "country", m->release.country);
        add_opt(release, "label", m->release.label);
        add_opt(release, "catalogueNumber", m->release.catalogue_number);
        add_opt(release, "notes", m->release.notes);
    }

    if (m->musicbrainz_release_group_id != 0 || m->musicbrainz_release_id != 0 ||
        m->barcode != 0) {
        ids = cJSON_GetObjectItemCaseSensitive(draft, "identifiers");
        if (!cJSON_IsObject(ids))
            ids = cJSON_AddObjectToObject(draft, "identifiers");
        add_opt(ids, "musicbrainzReleaseGroupId", m->musicbrainz_release_group_id);
        add_opt(ids, "musicbrainzReleaseId", m->musicbrainz_release_id);
        add_opt(ids, "barcode", m->barcode);
    }
    if (m->identity_source != 0 || m->identity_confidence != 0) {
        idn = cJSON_GetObjectItemCaseSensitive(draft, "identity");
        if (!cJSON_IsObject(idn))
            idn = cJSON_AddObjectToObject(draft, "identity");
        add_opt(idn, "source", m->identity_source);
        add_opt(idn, "confidence", m->identity_confidence);
    }

    media = cJSON_GetObjectItemCaseSensitive(draft, "media");
    if (cJSON_IsArray(media)) {
        cJSON *mi;
        cJSON_ArrayForEach(mi, media) {
            int disc = 0;
            musicpack_disc *md = 0;
            cJSON *tracks;
            cJSON *tr;
            size_t di;
            if (!cJSON_IsObject(mi))
                continue;
            jnum(mi, "disc", &disc);
            for (di = 0; di < m->disc_count; di++)
                if (m->discs[di].disc == disc) {
                    md = &m->discs[di];
                    break;
                }
            if (md == 0)
                continue;
            tracks = cJSON_GetObjectItemCaseSensitive(mi, "tracks");
            if (!cJSON_IsArray(tracks))
                continue;
            cJSON_ArrayForEach(tr, tracks) {
                int num = 0;
                musicpack_track *mt = 0;
                size_t ti;
                cJSON *sub;
                if (!cJSON_IsObject(tr))
                    continue;
                jnum(tr, "track", &num);
                for (ti = 0; ti < md->track_count; ti++)
                    if (md->tracks[ti].number == num) {
                        mt = &md->tracks[ti];
                        break;
                    }
                if (mt == 0)
                    continue;
                sub = cJSON_GetObjectItemCaseSensitive(tr, "identifiers");
                if (!cJSON_IsObject(sub))
                    sub = cJSON_AddObjectToObject(tr, "identifiers");
                add_opt(sub, "isrc", mt->isrc);
                add_opt(sub, "musicbrainzTrackId", mt->musicbrainz_track_id);
                add_opt(sub, "musicbrainzRecordingId", mt->musicbrainz_recording_id);
                add_opt(tr, "title", mt->title);
            }
        }
    }

    return 1;
}

/* ------------------------------------------------------------------ */
/* MusicBrainz candidate extraction                                    */
/* ------------------------------------------------------------------ */

#define MB_DOC_MAX (8u * 1024u * 1024u)

cJSON *
mb_candidates(const char *search_json, const musicpack_manifest *m)
{
    cJSON *root, *releases, *rel, *out;
    int n;

    if (search_json == 0 || m == 0 || strlen(search_json) > MB_DOC_MAX)
        return 0;
    root = cJSON_ParseWithLength(search_json, strlen(search_json));
    if (root == 0)
        return 0;
    releases = cJSON_GetObjectItemCaseSensitive(root, "releases");
    if (!cJSON_IsArray(releases)) {
        cJSON_Delete(root);
        return 0;
    }
    out = cJSON_CreateArray();
    if (out == 0) {
        cJSON_Delete(root);
        return 0;
    }
    n = cJSON_GetArraySize(releases);
    cJSON_ArrayForEach(rel, releases) {
        cJSON *cand, *rg;
        char *doc;
        const char *conf;
        if (!cJSON_IsObject(rel))
            continue;
        doc = cJSON_PrintUnformatted(rel);
        if (doc == 0)
            continue;
        conf = musicpack_mb_match_confidence(doc, m);
        free(doc);

        cand = cJSON_CreateObject();
        add_opt(cand, "releaseId", jstr(rel, "id"));
        rg = cJSON_GetObjectItemCaseSensitive(rel, "release-group");
        if (cJSON_IsObject(rg))
            add_opt(cand, "releaseGroupId", jstr(rg, "id"));
        add_opt(cand, "title", jstr(rel, "title"));
        {
            cJSON *credit = cJSON_GetObjectItemCaseSensitive(rel, "artist-credit");
            if (cJSON_IsArray(credit) && cJSON_GetArraySize(credit) > 0) {
                cJSON *c0 = cJSON_GetArrayItem(credit, 0);
                const char *name = jstr(c0, "name");
                add_opt(cand, "artist", name != 0 ? name : jstr(c0, "artist"));
            }
        }
        {
            const char *date = jstr(rel, "date");
            if (date == 0)
                date = jstr(rel, "first-release-date");
            add_opt(cand, "date", date);
        }
        add_opt(cand, "country", jstr(rel, "country"));
        add_opt(cand, "barcode", jstr(rel, "barcode"));
        cJSON_AddStringToObject(cand, "confidence", conf != 0 ? conf : "none");
        cJSON_AddItemToArray(out, cand);
    }
    (void) n;
    cJSON_Delete(root);
    return out;
}
