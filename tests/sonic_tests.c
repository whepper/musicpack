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

/*
 * C tests for the libmusicpack Sonic module: profile registry, base64-f32le
 * vectors, `musicpack-sonic` document parse/validate, manifest `analysis[]`
 * handling, and package verification of sonic documents.
 *
 * Wired into CTest as the "sonic_unit" suite. Platform-independent.
 */

#if defined(_WIN32)
# include <windows.h>
# include <direct.h>
# define mkdir_one(p) _mkdir(p)
#else
# include <unistd.h> /* mkdtemp */
# include <sys/stat.h> /* mkdir */
# define mkdir_one(p) mkdir(p, 0755)
#endif
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <musicpack/musicpack.h>

static int failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);    \
            failures++;                                                      \
        }                                                                    \
    } while (0)

#define CHECK_STATUS(actual, expected, msg)                                  \
    do {                                                                     \
        if ((actual) != (expected)) {                                        \
            fprintf(stderr, "FAIL %s:%d: %s (got %d, want %d)\n",            \
                    __FILE__, __LINE__, msg, (int) (actual),                 \
                    (int) (expected));                                       \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static char *
str_replace(const char *src, const char *find, const char *repl)
{
    const char *p = strstr(src, find);
    char *out;
    size_t pre, tail;
    if (p == 0)
        return strdup(src);
    pre = (size_t) (p - src);
    tail = strlen(p + strlen(find));
    out = (char *) malloc(pre + strlen(repl) + tail + 1);
    if (out == 0)
        return 0;
    memcpy(out, src, pre);
    memcpy(out + pre, repl, strlen(repl));
    memcpy(out + pre + strlen(repl), p + strlen(find), tail);
    out[pre + strlen(repl) + tail] = '\0';
    return out;
}

static char *
str_replace(const char *src, const char *find, const char *repl);

/* Replaces the first embedding's "data" payload with \p b64. Returns a heap
   string, or NULL on allocation failure. */
static char *
str_replace_data(const char *src, const char *b64);

/* base64-f32le of a unit vector (first element 1.0, rest zero). */
static char *
unit_b64(size_t dims)
{
    float *v = (float *) calloc(dims, sizeof *v);
    char *b64 = 0;
    if (v == 0)
        return 0;
    v[0] = 1.0f;
    if (musicpack_sonic_vector_encode(v, dims, &b64) != MUSICPACK_OK)
        b64 = 0;
    free(v);
    return b64;
}

/* base64-f32le of an arbitrary float vector (no validation). */
static char *
raw_b64(const float *v, size_t n)
{
    char *b64 = 0;
    if (musicpack_sonic_vector_encode(v, n, &b64) != MUSICPACK_OK)
        return 0;
    return b64;
}

typedef struct {
    int disc;
    int track;
    int null;         /* emit "embedding": null */
    const char *data; /* base64 data override (else auto unit vector) */
} doc_track;

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} sbuf;

static void
sb_append(sbuf *o, const char *s)
{
    size_t n = strlen(s);
    if (o->len + n + 1 > o->cap) {
        while (o->len + n + 1 > o->cap)
            o->cap *= 2;
        o->buf = (char *) realloc(o->buf, o->cap);
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = '\0';
}

/* Builds a `musicpack-sonic` v1 document string. Auto-fills unit embeddings
   unless \p data is given. \p album_null / \p album_data control the album
   embedding; \p album_contrib is the declared tracksContributing. */
static char *
build_doc(const char *profile_id, size_t dims, const doc_track *tracks,
          int ntrack, int album_null, const char *album_data, int album_contrib,
          const char *tool, const char *toolver)
{
    sbuf o = { 0, 65536, 0 };
    char tmp[64];
    int i;

    o.buf = (char *) malloc(o.cap);
    if (o.buf == 0)
        return 0;
    sb_append(&o, "{\"format\":\"musicpack-sonic\",\"version\":1,");
    sb_append(&o, "\"profile\":{\"id\":\"");
    sb_append(&o, profile_id);
    sb_append(&o, "\",\"dimensions\":");
    snprintf(tmp, sizeof tmp, "%zu", dims);
    sb_append(&o, tmp);
    sb_append(&o, ",\"distance\":\"cosine\",\"encoding\":\"base64-f32le\"},");
    sb_append(&o, "\"analyzer\":{\"tool\":\"");
    sb_append(&o, tool != 0 ? tool : "musicpack");
    sb_append(&o, "\",\"toolVersion\":\"");
    sb_append(&o, toolver != 0 ? toolver : "test");
    sb_append(&o, "\"},");
    sb_append(&o, "\"album\":{\"embedding\":");
    if (album_null) {
        sb_append(&o, "null");
    } else {
        char *b64 = album_data != 0 ? strdup(album_data) : unit_b64(dims);
        sb_append(&o, "{\"encoding\":\"base64-f32le\",\"dimensions\":");
        snprintf(tmp, sizeof tmp, "%zu", dims);
        sb_append(&o, tmp);
        sb_append(&o, ",\"data\":\"");
        if (b64 != 0)
            sb_append(&o, b64);
        sb_append(&o, "\"}");
        free(b64);
    }
    sb_append(&o, ",\"tracksContributing\":");
    snprintf(tmp, sizeof tmp, "%d", album_contrib);
    sb_append(&o, tmp);
    sb_append(&o, "},\"tracks\":[");
    for (i = 0; i < ntrack; i++) {
        char *b64 = tracks[i].data != 0 ? strdup(tracks[i].data) : unit_b64(dims);
        sb_append(&o, i > 0 ? ",{\"disc\":" : "{\"disc\":");
        snprintf(tmp, sizeof tmp, "%d", tracks[i].disc);
        sb_append(&o, tmp);
        sb_append(&o, ",\"track\":");
        snprintf(tmp, sizeof tmp, "%d", tracks[i].track);
        sb_append(&o, tmp);
        if (tracks[i].null) {
            sb_append(&o, ",\"embedding\":null}");
        } else {
            sb_append(&o, ",\"embedding\":{\"encoding\":\"base64-f32le\",\"dimensions\":");
            snprintf(tmp, sizeof tmp, "%zu", dims);
            sb_append(&o, tmp);
            sb_append(&o, ",\"data\":\"");
            if (b64 != 0)
                sb_append(&o, b64);
            sb_append(&o, "\"}}");
        }
        free(b64);
    }
    sb_append(&o, "]}\n");
    return o.buf;
}

/* A minimal 2-track manifest used for manifest-aware validation. */
static const char *TWO_TRACK_MANIFEST =
    "{\"format\":\"musicpack\",\"version\":1,"
    "\"album\":{\"title\":\"T\",\"artists\":[{\"name\":\"A\"}]},"
    "\"media\":[{\"disc\":1,\"tracks\":["
    "{\"track\":1,\"title\":\"One\",\"audio\":{\"path\":\"audio/1.mpc\","
    "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}},"
    "{\"track\":2,\"title\":\"Two\",\"audio\":{\"path\":\"audio/2.mpc\","
    "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}}"
    "]}]}";

/* default 2-track fully-embedded openl3 doc */
static char *
default_doc(void)
{
    doc_track tracks[2] = { { 1, 1, 0, 0 }, { 1, 2, 0, 0 } };
    return build_doc("musicpack-sonic-openl3-v1", 512, tracks, 2, 0, 0, 2,
                     "musicpack", "test");
}

/* ------------------------------------------------------------------ */
/* profile registry                                                    */
/* ------------------------------------------------------------------ */

static void
test_profile_id_valid(void)
{
    CHECK(musicpack_sonic_profile_id_valid("musicpack-sonic-openl3-v1"), "openl3 id");
    CHECK(musicpack_sonic_profile_id_valid("musicpack-sonic-x-v2"), "two segments");
    CHECK(musicpack_sonic_profile_id_valid("musicpack-sonic-a-b-v1"), "multi segment");
    CHECK(musicpack_sonic_profile_id_valid("musicpack-sonic-a0-v10"), "digits in name");

    CHECK(!musicpack_sonic_profile_id_valid(0), "null id");
    CHECK(!musicpack_sonic_profile_id_valid(""), "empty id");
    CHECK(!musicpack_sonic_profile_id_valid("musicpack-sonic-"), "no model");
    CHECK(!musicpack_sonic_profile_id_valid("musicpack-sonic-v1"), "no model segment");
    CHECK(!musicpack_sonic_profile_id_valid("musicpack-sonic-OpenL3-v1"), "uppercase");
    CHECK(!musicpack_sonic_profile_id_valid("musicpack-sonic-a-b"), "no version suffix");
    CHECK(!musicpack_sonic_profile_id_valid("musicpack-sonic-a-v"), "empty version");
    CHECK(!musicpack_sonic_profile_id_valid("musicpack-sonic-a-vx"), "non-digit version");
    CHECK(!musicpack_sonic_profile_id_valid("musicpack-sonic--a-v1"), "empty segment");
    CHECK(!musicpack_sonic_profile_id_valid("musicpack-sonic-a--v1"), "empty segment 2");
    CHECK(!musicpack_sonic_profile_id_valid("musicpack-sonic-a-v1-"), "trailing dash");
    CHECK(!musicpack_sonic_profile_id_valid("other-v1"), "wrong prefix");

    {
        const musicpack_sonic_profile *p;
        p = musicpack_sonic_profile_get("musicpack-sonic-openl3-v1");
        CHECK(p != 0 && p->dimensions == 512, "openl3 registered");
        CHECK(p != 0 && p->state == MUSICPACK_SONIC_PROFILE_SUPPORTED, "openl3 supported");
        p = musicpack_sonic_profile_get("musicpack-sonic-discogs-v1");
        CHECK(p != 0 && p->dimensions == 1280, "discogs registered");
        CHECK(p != 0 && p->state == MUSICPACK_SONIC_PROFILE_RESERVED, "discogs reserved");
        p = musicpack_sonic_profile_get("musicpack-sonic-futuremodel-v1");
        CHECK(p == 0, "unknown profile not registered");
    }
    CHECK(musicpack_sonic_encoding_supported("base64-f32le"), "f32le supported");
    CHECK(!musicpack_sonic_encoding_supported("binary-f32le"), "binary not in v1");
    CHECK(!musicpack_sonic_encoding_supported(0), "null encoding");
}

/* ------------------------------------------------------------------ */
/* vectors                                                             */
/* ------------------------------------------------------------------ */

static void
test_vectors(void)
{
    float v[8];
    float *dec = 0;
    size_t n = 0;
    char *b64 = 0;
    size_t i;

    /* round-trip a unit vector (decode enforces unit norm) */
    memset(v, 0, sizeof v);
    v[0] = 1.0f;
    CHECK_STATUS(musicpack_sonic_vector_encode(v, 8, &b64), MUSICPACK_OK, "encode ok");
    CHECK_STATUS(musicpack_sonic_vector_decode(b64, strlen(b64), 8, &dec, &n),
                 MUSICPACK_OK, "decode ok");
    CHECK(dec != 0 && n == 8, "decode count");
    if (dec != 0) {
        for (i = 0; i < 8; i++)
            CHECK(dec[i] == v[i], "round-trip values");
        free(dec);
    }
    free(b64);

    {
        float u[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
        CHECK_STATUS(musicpack_sonic_vector_validate(u, 4, MUSICPACK_SONIC_NORM_TOLERANCE),
                     MUSICPACK_OK, "unit vector");
    }
    {
        float u[4] = { 1.0f, 1.0f, 1.0f, 1.0f }; /* norm 2 */
        CHECK_STATUS(musicpack_sonic_vector_validate(u, 4, MUSICPACK_SONIC_NORM_TOLERANCE),
                     MUSICPACK_ERR_INVALID, "non-unit rejected");
    }
    {
        float u[4] = { NAN, 0, 0, 0 };
        CHECK_STATUS(musicpack_sonic_vector_validate(u, 4, MUSICPACK_SONIC_NORM_TOLERANCE),
                     MUSICPACK_ERR_INVALID, "nan rejected");
    }
    {
        float u[4] = { INFINITY, 0, 0, 0 };
        CHECK_STATUS(musicpack_sonic_vector_validate(u, 4, MUSICPACK_SONIC_NORM_TOLERANCE),
                     MUSICPACK_ERR_INVALID, "inf rejected");
    }

    CHECK_STATUS(musicpack_sonic_vector_decode("!!not base64!!", 13, 8, &dec, &n),
                 MUSICPACK_ERR_INVALID, "bad base64");
    CHECK_STATUS(musicpack_sonic_vector_decode("AA==", 4, 8, &dec, &n),
                 MUSICPACK_ERR_INVALID, "wrong byte length");
    {
        float w[8];
        for (i = 0; i < 8; i++)
            w[i] = 0.25f; /* norm = sqrt(8)*0.25 = 0.707 */
        b64 = raw_b64(w, 8);
        CHECK_STATUS(musicpack_sonic_vector_decode(b64, strlen(b64), 8, &dec, &n),
                     MUSICPACK_ERR_INVALID, "non-normalized rejected");
        free(b64);
    }
}

/* ------------------------------------------------------------------ */
/* parse                                                                */
/* ------------------------------------------------------------------ */

static void
test_parse_ok(void)
{
    char *doc = default_doc();
    musicpack_status s;
    musicpack_sonic *x;
    musicpack_sonic_profile_state state;

    x = musicpack_sonic_parse(doc, strlen(doc), &s);
    CHECK(x != 0, "valid doc parses");
    if (x != 0) {
        CHECK(x->track_count == 2, "two tracks");
        CHECK(x->album.present == 1 && x->album.dimensions == 512, "album present");
        CHECK(x->album_tracks_contributing == 2, "contributors");
        CHECK(x->tracks[0].embedding.present && x->tracks[0].disc == 1 &&
              x->tracks[0].track == 1, "track 1");
        CHECK_STATUS(musicpack_sonic_validate(x, 0, &state), MUSICPACK_OK,
                     "validate standalone");
        CHECK(state == MUSICPACK_SONIC_PROFILE_SUPPORTED, "supported state");
        {
            musicpack_manifest *m = musicpack_manifest_parse(TWO_TRACK_MANIFEST, &s);
            CHECK(m != 0, "manifest parses");
            if (m != 0) {
                CHECK_STATUS(musicpack_sonic_validate(x, m, 0), MUSICPACK_OK,
                             "validate vs manifest");
                musicpack_manifest_free(m);
            }
        }
        musicpack_sonic_free(x);
    }
    free(doc);

    /* null track embedding + album only when contributors exist */
    {
        doc_track tracks[2] = { { 1, 1, 0, 0 }, { 1, 2, 1, 0 } };
        doc = build_doc("musicpack-sonic-openl3-v1", 512, tracks, 2, 0, 0, 1,
                        "musicpack", "test");
        x = musicpack_sonic_parse(doc, strlen(doc), &s);
        CHECK(x != 0, "partial doc parses");
        if (x != 0) {
            CHECK(!x->tracks[1].embedding.present, "track 2 null");
            CHECK(x->album_tracks_contributing == 1, "contributors == 1");
            CHECK_STATUS(musicpack_sonic_validate(x, 0, 0), MUSICPACK_OK,
                         "partial validate");
            musicpack_sonic_free(x);
        }
        free(doc);
    }

    /* empty tracks + null album */
    {
        doc = build_doc("musicpack-sonic-openl3-v1", 512, 0, 0, 1, 0, 0,
                        "musicpack", "test");
        x = musicpack_sonic_parse(doc, strlen(doc), &s);
        CHECK(x != 0 && x->track_count == 0, "empty doc parses");
        if (x != 0)
            musicpack_sonic_free(x);
        free(doc);
    }
}

static void
test_parse_invalid(void)
{
    char *doc;
    musicpack_status s;
    doc_track tracks[2] = { { 1, 1, 0, 0 }, { 1, 2, 0, 0 } };

    /* wrong format */
    doc = str_replace(default_doc(), "musicpack-sonic", "musicpack-sonicX");
    CHECK(musicpack_sonic_parse(doc, strlen(doc), &s) == 0 &&
          s == MUSICPACK_ERR_INVALID, "wrong format");
    free(doc);

    /* wrong version */
    doc = str_replace(default_doc(), "\"version\":1", "\"version\":2");
    CHECK(musicpack_sonic_parse(doc, strlen(doc), &s) == 0 &&
          s == MUSICPACK_ERR_VERSION, "wrong version");
    free(doc);

    /* malformed JSON */
    doc = default_doc();
    doc[0] = 'x'; /* leading garbage breaks the JSON structure */
    CHECK(musicpack_sonic_parse(doc, strlen(doc), &s) == 0 &&
          s == MUSICPACK_ERR_JSON, "malformed json");
    free(doc);

    /* invalid profile id syntax */
    doc = str_replace(default_doc(), "musicpack-sonic-openl3-v1",
                      "musicpack-sonic-OpenL3-v1");
    CHECK(musicpack_sonic_parse(doc, strlen(doc), &s) == 0 &&
          s == MUSICPACK_ERR_INVALID, "bad profile id");
    free(doc);

    /* unsupported encoding */
    doc = str_replace(default_doc(), "\"encoding\":\"base64-f32le\"",
                      "\"encoding\":\"binary-f32le\"");
    CHECK(musicpack_sonic_parse(doc, strlen(doc), &s) == 0 &&
          s == MUSICPACK_ERR_INVALID, "unsupported encoding");
    free(doc);

    /* dimensions above the bound */
    doc = build_doc("musicpack-sonic-openl3-v1",
                    (size_t) MUSICPACK_SONIC_MAX_DIMENSIONS + 1,
                    tracks, 2, 0, 0, 2, "musicpack", "test");
    CHECK(musicpack_sonic_parse(doc, strlen(doc), &s) == 0 &&
          s == MUSICPACK_ERR_INVALID, "dimension bomb");
    free(doc);

    /* malformed base64 in the album embedding (first "data" is the album's) */
    doc = default_doc();
    {
        char *pos = strstr(doc, "\"data\":\"");
        if (pos != 0)
            pos[strlen("\"data\":\"")] = '!';
    }
    CHECK(musicpack_sonic_parse(doc, strlen(doc), &s) == 0 &&
          s == MUSICPACK_ERR_INVALID, "malformed base64");
    free(doc);

    /* wrong decoded byte length (album dims 512, payload a 256-dim vector) */
    {
        char *shortv = unit_b64(256);
        char *full = default_doc();
        char *pos = strstr(full, "\"data\":\"") + strlen("\"data\":\"");
        char *end = strchr(pos, '"');
        char *nd = (char *) malloc(strlen(full) - (size_t) (end - pos) + strlen(shortv) + 1);
        if (nd != 0) {
            memcpy(nd, full, (size_t) (pos - full));
            strcpy(nd + (pos - full), shortv);
            strcat(nd, end);
            doc = nd;
            {
                musicpack_sonic *dbg = musicpack_sonic_parse(doc, strlen(doc), &s);
                CHECK(dbg == 0 && s == MUSICPACK_ERR_INVALID, "wrong byte length");
                if (dbg)
                    musicpack_sonic_free(dbg);
            }
            free(doc);
        }
        free(full);
        free(shortv);
    }

    /* NaN / Inf / zero vectors in the album embedding */
    {
        float nanv[512];
        char *b64;
        memset(nanv, 0, sizeof nanv);
        nanv[0] = (float) NAN;
        b64 = raw_b64(nanv, 512);
        doc = str_replace_data(default_doc(), b64);
        CHECK(doc != 0 &&
              musicpack_sonic_parse(doc, strlen(doc), &s) == 0 &&
              s == MUSICPACK_ERR_INVALID, "nan vector rejected");
        free(doc);
        free(b64);
    }
    {
        float infv[512];
        char *b64;
        memset(infv, 0, sizeof infv);
        infv[0] = (float) INFINITY;
        b64 = raw_b64(infv, 512);
        doc = str_replace_data(default_doc(), b64);
        CHECK(doc != 0 &&
              musicpack_sonic_parse(doc, strlen(doc), &s) == 0 &&
              s == MUSICPACK_ERR_INVALID, "inf vector rejected");
        free(doc);
        free(b64);
    }
    {
        float zerov[512];
        char *b64;
        size_t i;
        for (i = 0; i < 512; i++)
            zerov[i] = 0.0f;
        b64 = raw_b64(zerov, 512);
        doc = str_replace_data(default_doc(), b64);
        CHECK(doc != 0 &&
              musicpack_sonic_parse(doc, strlen(doc), &s) == 0 &&
              s == MUSICPACK_ERR_INVALID, "zero vector rejected");
        free(doc);
        free(b64);
    }

    /* missing analyzer provenance */
    doc = strdup("{\"format\":\"musicpack-sonic\",\"version\":1,"
                 "\"profile\":{\"id\":\"musicpack-sonic-openl3-v1\","
                 "\"dimensions\":512,\"distance\":\"cosine\","
                 "\"encoding\":\"base64-f32le\"},"
                 "\"album\":{\"embedding\":null,\"tracksContributing\":0},"
                 "\"tracks\":[]}");
    CHECK(musicpack_sonic_parse(doc, strlen(doc), &s) == 0 &&
          s == MUSICPACK_ERR_INVALID, "missing analyzer");
    free(doc);

    /* missing tracks array */
    doc = strdup("{\"format\":\"musicpack-sonic\",\"version\":1,"
                 "\"profile\":{\"id\":\"musicpack-sonic-openl3-v1\","
                 "\"dimensions\":512,\"distance\":\"cosine\","
                 "\"encoding\":\"base64-f32le\"},"
                 "\"analyzer\":{\"tool\":\"musicpack\",\"toolVersion\":\"test\"},"
                 "\"album\":{\"embedding\":null,\"tracksContributing\":0}}");
    CHECK(musicpack_sonic_parse(doc, strlen(doc), &s) == 0 &&
          s == MUSICPACK_ERR_INVALID, "missing tracks");
    free(doc);
}

/* Replaces the first embedding's "data" payload with \p b64. Returns a heap
   string, or NULL on allocation failure. */
static char *
str_replace_data(const char *src, const char *b64)
{
    const char *pos = strstr(src, "\"data\":\"");
    const char *end;
    char *out;
    size_t pre, tail;
    if (pos == 0)
        return 0;
    pos += strlen("\"data\":\"");
    end = strchr(pos, '"');
    if (end == 0)
        return 0;
    pre = (size_t) (pos - src);
    tail = strlen(end);
    out = (char *) malloc(pre + strlen(b64) + tail + 1);
    if (out == 0)
        return 0;
    memcpy(out, src, pre);
    memcpy(out + pre, b64, strlen(b64));
    memcpy(out + pre + strlen(b64), end, tail);
    out[pre + strlen(b64) + tail] = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/* validate                                                             */
/* ------------------------------------------------------------------ */

static void
test_validate(void)
{
    char *doc;
    musicpack_status s;
    musicpack_sonic *x;
    musicpack_sonic_profile_state state;
    doc_track tracks2[2] = { { 1, 1, 0, 0 }, { 1, 2, 0, 0 } };
    musicpack_manifest *m;

    m = musicpack_manifest_parse(TWO_TRACK_MANIFEST, &s);
    CHECK(m != 0, "manifest parses");

    /* known profile metadata mismatch (openl3 must be 512-dim) */
    doc = build_doc("musicpack-sonic-openl3-v1", 256, tracks2, 2, 0, 0, 2,
                    "musicpack", "test");
    x = musicpack_sonic_parse(doc, strlen(doc), &s);
    CHECK(x != 0, "256-dim doc parses");
    if (x != 0) {
        CHECK_STATUS(musicpack_sonic_validate(x, 0, 0), MUSICPACK_ERR_INVALID,
                     "openl3 dims mismatch");
        musicpack_sonic_free(x);
    }
    free(doc);

    /* duplicate track entry */
    {
        doc_track dups[2] = { { 1, 1, 0, 0 }, { 1, 1, 0, 0 } };
        doc = build_doc("musicpack-sonic-openl3-v1", 512, dups, 2, 0, 0, 2,
                        "musicpack", "test");
        x = musicpack_sonic_parse(doc, strlen(doc), &s);
        CHECK(x != 0, "dup doc parses");
        if (x != 0) {
            CHECK_STATUS(musicpack_sonic_validate(x, 0, 0), MUSICPACK_ERR_INVALID,
                         "duplicate track rejected");
            musicpack_sonic_free(x);
        }
        free(doc);
    }

    /* contributor count mismatch */
    doc = build_doc("musicpack-sonic-openl3-v1", 512, tracks2, 2, 0, 0, 3,
                    "musicpack", "test");
    x = musicpack_sonic_parse(doc, strlen(doc), &s);
    CHECK(x != 0, "contrib doc parses");
    if (x != 0) {
        CHECK_STATUS(musicpack_sonic_validate(x, 0, 0), MUSICPACK_ERR_INVALID,
                     "contributor mismatch");
        musicpack_sonic_free(x);
    }
    free(doc);

    /* album embedding with no contributors */
    doc = build_doc("musicpack-sonic-openl3-v1", 512, 0, 0, 0, 0, 0,
                    "musicpack", "test");
    x = musicpack_sonic_parse(doc, strlen(doc), &s);
    CHECK(x != 0, "no-track album parses");
    if (x != 0) {
        CHECK_STATUS(musicpack_sonic_validate(x, 0, 0), MUSICPACK_ERR_INVALID,
                     "album without contributors rejected");
        musicpack_sonic_free(x);
    }
    free(doc);

    /* manifest-aware validation */
    if (m != 0) {
        doc = build_doc("musicpack-sonic-openl3-v1", 512, tracks2, 2, 0, 0, 2,
                        "musicpack", "test");
        x = musicpack_sonic_parse(doc, strlen(doc), &s);
        if (x != 0) {
            CHECK_STATUS(musicpack_sonic_validate(x, m, 0), MUSICPACK_OK,
                         "two-track doc vs two-track manifest");
            musicpack_sonic_free(x);
        }
        free(doc);

        /* manifest with one track -> doc with two fails */
        doc = build_doc("musicpack-sonic-openl3-v1", 512, tracks2, 2, 0, 0, 2,
                        "musicpack", "test");
        x = musicpack_sonic_parse(doc, strlen(doc), &s);
        if (x != 0) {
            musicpack_manifest *m1 = musicpack_manifest_parse(
                "{\"format\":\"musicpack\",\"version\":1,"
                "\"album\":{\"title\":\"T\",\"artists\":[{\"name\":\"A\"}]},"
                "\"media\":[{\"disc\":1,\"tracks\":["
                "{\"track\":1,\"title\":\"One\",\"audio\":{\"path\":\"audio/1.mpc\","
                "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}}"
                "]}]}", &s);
            CHECK(m1 != 0, "one-track manifest parses");
            if (m1 != 0) {
                CHECK_STATUS(musicpack_sonic_validate(x, m1, 0), MUSICPACK_ERR_INVALID,
                             "doc with extra track fails");
                musicpack_manifest_free(m1);
            }
            musicpack_sonic_free(x);
        }
        free(doc);

        /* nonexistent disc/track inside manifest-matching count */
        {
            doc_track wrong[2] = { { 1, 1, 0, 0 }, { 2, 3, 0, 0 } };
            doc = build_doc("musicpack-sonic-openl3-v1", 512, wrong, 2, 0, 0, 2,
                            "musicpack", "test");
            x = musicpack_sonic_parse(doc, strlen(doc), &s);
            if (x != 0) {
                CHECK_STATUS(musicpack_sonic_validate(x, m, 0), MUSICPACK_ERR_INVALID,
                             "nonexistent disc/track fails");
                musicpack_sonic_free(x);
            }
            free(doc);
        }
        musicpack_manifest_free(m);
    }

    /* reserved profile (discogs) structurally validated */
    {
        doc_track dt[1] = { { 1, 1, 0, 0 } };
        doc = build_doc("musicpack-sonic-discogs-v1", 1280, dt, 1, 0, 0, 1,
                        "musicpack", "test");
        x = musicpack_sonic_parse(doc, strlen(doc), &s);
        CHECK(x != 0, "discogs doc parses");
        if (x != 0) {
            CHECK_STATUS(musicpack_sonic_validate(x, 0, &state), MUSICPACK_OK,
                         "discogs validates");
            CHECK(state == MUSICPACK_SONIC_PROFILE_RESERVED, "discogs reserved state");
            musicpack_sonic_free(x);
        }
        free(doc);
    }

    /* unknown profile: structurally valid, marked unknown */
    doc = build_doc("musicpack-sonic-futuremodel-v1", 512, tracks2, 2, 0, 0, 2,
                    "musicpack", "test");
    x = musicpack_sonic_parse(doc, strlen(doc), &s);
    CHECK(x != 0, "unknown-profile doc parses");
    if (x != 0) {
        CHECK_STATUS(musicpack_sonic_validate(x, 0, &state), MUSICPACK_OK,
                     "unknown profile validates structurally");
        CHECK(state == MUSICPACK_SONIC_PROFILE_UNKNOWN, "unknown state");
        musicpack_sonic_free(x);
    }
    free(doc);
}

/* ------------------------------------------------------------------ */
/* manifest analysis[]                                                 */
/* ------------------------------------------------------------------ */

static void
test_manifest_analysis(void)
{
    musicpack_status s;
    musicpack_manifest *m;
    char *json = 0;

    /* sonic reference requires profile + sha256 */
    m = musicpack_manifest_parse(
        "{\"format\":\"musicpack\",\"version\":1,"
        "\"album\":{\"title\":\"T\",\"artists\":[{\"name\":\"A\"}]},"
        "\"analysis\":[{\"type\":\"sonic\",\"path\":\"analysis/sonic.json\"}],"
        "\"media\":[{\"disc\":1,\"tracks\":[{\"track\":1,\"title\":\"One\","
        "\"audio\":{\"path\":\"audio/1.mpc\","
        "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}}]}]}",
        &s);
    CHECK(m == 0 && s == MUSICPACK_ERR_INVALID, "sonic ref needs sha256+profile");

    /* traversal path rejected */
    m = musicpack_manifest_parse(
        "{\"format\":\"musicpack\",\"version\":1,"
        "\"album\":{\"title\":\"T\",\"artists\":[{\"name\":\"A\"}]},"
        "\"analysis\":[{\"type\":\"sonic\",\"profile\":\"musicpack-sonic-openl3-v1\","
        "\"path\":\"../evil.json\","
        "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}],"
        "\"media\":[{\"disc\":1,\"tracks\":[{\"track\":1,\"title\":\"One\","
        "\"audio\":{\"path\":\"audio/1.mpc\","
        "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}}]}]}",
        &s);
    CHECK(m == 0 && s == MUSICPACK_ERR_PATH, "traversal analysis path rejected");

    /* valid reference + round-trip */
    m = musicpack_manifest_parse(
        "{\"format\":\"musicpack\",\"version\":1,"
        "\"album\":{\"title\":\"T\",\"artists\":[{\"name\":\"A\"}]},"
        "\"analysis\":[{\"type\":\"sonic\",\"profile\":\"musicpack-sonic-openl3-v1\","
        "\"path\":\"analysis/sonic.json\","
        "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}],"
        "\"media\":[{\"disc\":1,\"tracks\":[{\"track\":1,\"title\":\"One\","
        "\"audio\":{\"path\":\"audio/1.mpc\","
        "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}}]}]}",
        &s);
    CHECK(m != 0, "valid analysis ref parses");
    if (m != 0) {
        CHECK(m->analysis_count == 1, "one analysis entry");
        if (m->analysis_count == 1) {
            CHECK(strcmp(m->analysis[0].type, "sonic") == 0, "type");
            CHECK(m->analysis[0].profile != 0 &&
                  strcmp(m->analysis[0].profile, "musicpack-sonic-openl3-v1") == 0,
                  "profile");
            CHECK(m->analysis[0].asset.sha256 != 0, "sha256");
        }
        CHECK_STATUS(musicpack_manifest_write(m, &json), MUSICPACK_OK, "write");
        CHECK(json != 0 && strstr(json, "\"analysis\"") != 0, "analysis serialized");
        free(json);
        musicpack_manifest_free(m);
    }

    /* unknown analysis type stays forward-compatible */
    m = musicpack_manifest_parse(
        "{\"format\":\"musicpack\",\"version\":1,"
        "\"album\":{\"title\":\"T\",\"artists\":[{\"name\":\"A\"}]},"
        "\"analysis\":[{\"type\":\"future-analysis\",\"path\":\"analysis/future.json\","
        "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}],"
        "\"media\":[{\"disc\":1,\"tracks\":[{\"track\":1,\"title\":\"One\","
        "\"audio\":{\"path\":\"audio/1.mpc\","
        "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}}]}]}",
        &s);
    CHECK(m != 0, "unknown analysis type parses");
    if (m != 0) {
        CHECK(m->analysis_count == 1 &&
              strcmp(m->analysis[0].type, "future-analysis") == 0, "unknown type kept");
        CHECK_STATUS(musicpack_manifest_write(m, &json), MUSICPACK_OK, "write unknown type");
        CHECK(json != 0 && strstr(json, "future-analysis") != 0, "unknown type preserved");
        free(json);
        musicpack_manifest_free(m);
    }
}

/* ------------------------------------------------------------------ */
/* package verification                                                */
/* ------------------------------------------------------------------ */

static int
make_temp_dir(char *buf, size_t cap)
{
#if defined(_WIN32)
    const char *base = getenv("TEMP");
    if (base == 0) base = ".";
    if (snprintf(buf, cap, "%s\\sonic_test_%lu", base,
                 (unsigned long) GetCurrentProcessId()) >= (int) cap)
        return -1;
    if (_mkdir(buf) != 0 && errno != EEXIST)
        return -1;
    return 0;
#else
    if (snprintf(buf, cap, "/tmp/sonic_test_XXXXXX") >= (int) cap)
        return -1;
    return mkdtemp(buf) != 0 ? 0 : -1;
#endif
}

static void
rmdir_one(const char *p)
{
#if defined(_WIN32)
    _rmdir(p);
#else
    remove(p);
#endif
}

static void
remove_dir_tree(const char *dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/manifest.json", dir); remove(path);
    snprintf(path, sizeof path, "%s/audio/1.mpc", dir); remove(path);
    snprintf(path, sizeof path, "%s/audio/2.mpc", dir); remove(path);
    snprintf(path, sizeof path, "%s/analysis/sonic.json", dir); remove(path);
    snprintf(path, sizeof path, "%s/analysis", dir); rmdir_one(path);
    snprintf(path, sizeof path, "%s/audio", dir); rmdir_one(path);
    snprintf(path, sizeof path, "%s", dir); rmdir_one(path);
}

static int
write_bytes(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (f == 0)
        return -1;
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0)
        return -1;
    return 0;
}

/* Builds a package under \p root: audio/1.mpc + audio/2.mpc, and when
   \p sonic_json is non-NULL an analysis/sonic.json referenced from the
   manifest (profile \p analysis_profile, sha256 computed automatically).
   When \p analysis_profile is NULL no analysis[] entry is emitted. */
static void
make_package(const char *root, const char *sonic_json, const char *analysis_profile)
{
    char path[512];
    char sha1[MUSICPACK_SHA256_HEX_SIZE];
    char sha2[MUSICPACK_SHA256_HEX_SIZE];
    char sonicsha[MUSICPACK_SHA256_HEX_SIZE];
    char manifest[8192];
    const char *onep = "aaaa", *twop = "bbbb";

    snprintf(path, sizeof path, "%s/audio", root);
    mkdir_one(path);
    snprintf(path, sizeof path, "%s/analysis", root);
    mkdir_one(path);
    snprintf(path, sizeof path, "%s/audio/1.mpc", root);
    write_bytes(path, onep, 4);
    snprintf(path, sizeof path, "%s/audio/2.mpc", root);
    write_bytes(path, twop, 4);
    musicpack_sha256_file(path, sha2, sizeof sha2);
    snprintf(path, sizeof path, "%s/audio/1.mpc", root);
    musicpack_sha256_file(path, sha1, sizeof sha1);

    sonicsha[0] = '\0';
    if (sonic_json != 0) {
        snprintf(path, sizeof path, "%s/analysis/sonic.json", root);
        write_bytes(path, sonic_json, strlen(sonic_json));
        musicpack_sha256_file(path, sonicsha, sizeof sonicsha);
    }

    if (analysis_profile == 0) {
        snprintf(manifest, sizeof manifest,
                 "{\"format\":\"musicpack\",\"version\":1,"
                 "\"album\":{\"title\":\"T\",\"artists\":[{\"name\":\"A\"}]},"
                 "\"media\":[{\"disc\":1,\"tracks\":["
                 "{\"track\":1,\"title\":\"One\",\"audio\":{\"path\":\"audio/1.mpc\",\"sha256\":\"%s\"}},"
                 "{\"track\":2,\"title\":\"Two\",\"audio\":{\"path\":\"audio/2.mpc\",\"sha256\":\"%s\"}}"
                 "]}]}",
                 sha1, sha2);
    } else {
        snprintf(manifest, sizeof manifest,
                 "{\"format\":\"musicpack\",\"version\":1,"
                 "\"album\":{\"title\":\"T\",\"artists\":[{\"name\":\"A\"}]},"
                 "\"analysis\":[{\"type\":\"sonic\",\"profile\":\"%s\","
                 "\"path\":\"analysis/sonic.json\",\"sha256\":\"%s\"}],"
                 "\"media\":[{\"disc\":1,\"tracks\":["
                 "{\"track\":1,\"title\":\"One\",\"audio\":{\"path\":\"audio/1.mpc\",\"sha256\":\"%s\"}},"
                 "{\"track\":2,\"title\":\"Two\",\"audio\":{\"path\":\"audio/2.mpc\",\"sha256\":\"%s\"}}"
                 "]}]}",
                 analysis_profile, sonicsha, sha1, sha2);
    }
    snprintf(path, sizeof path, "%s/manifest.json", root);
    write_bytes(path, manifest, strlen(manifest));
}

static void
test_package_verify(void)
{
    char root[512];
    char *doc;
    musicpack_status s;
    musicpack_report rep;
    musicpack_package *pkg;
    doc_track tracks[2] = { { 1, 1, 0, 0 }, { 1, 2, 0, 0 } };

    /* valid package with sonic: verify clean */
    if (make_temp_dir(root, sizeof root) != 0)
        return;
    doc = build_doc("musicpack-sonic-openl3-v1", 512, tracks, 2, 0, 0, 2,
                    "musicpack", "test");
    make_package(root, doc, "musicpack-sonic-openl3-v1");
    pkg = musicpack_package_open_dir(root, &s);
    CHECK(pkg != 0, "package opens");
    if (pkg != 0) {
        memset(&rep, 0, sizeof rep);
        CHECK_STATUS(musicpack_package_verify(pkg, &rep, 0, 0), MUSICPACK_OK,
                     "valid sonic package verifies clean");
        CHECK(rep.errors == 0, "no errors");
        musicpack_package_close(pkg);
    }
    free(doc);
    remove_dir_tree(root);

    /* wrong sha256 on the analysis reference: error */
    if (make_temp_dir(root, sizeof root) != 0)
        return;
    doc = build_doc("musicpack-sonic-openl3-v1", 512, tracks, 2, 0, 0, 2,
                    "musicpack", "test");
    make_package(root, doc, "musicpack-sonic-openl3-v1");
    {
        char path[512];
        char wrong[] = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
        /* rewrite the manifest with a bogus sonic sha */
        char *mpath;
        snprintf(path, sizeof path, "%s/manifest.json", root);
        mpath = (char *) malloc(8192);
        if (mpath != 0) {
            FILE *f = fopen(path, "rb");
            if (f != 0) {
                size_t n = fread(mpath, 1, 8191, f);
                fclose(f);
                mpath[n] = '\0';
                {
                    char *needle = strstr(mpath, "\"sha256\":\"");
                    /* first sha256 in manifest is the analysis one */
                    if (needle != 0) {
                        char *start = needle + strlen("\"sha256\":\"");
                        memcpy(start, wrong, strlen(wrong));
                    }
                }
                write_bytes(path, mpath, strlen(mpath));
            }
            free(mpath);
        }
    }
    pkg = musicpack_package_open_dir(root, &s);
    CHECK(pkg != 0, "package opens (wrong sha)");
    if (pkg != 0) {
        memset(&rep, 0, sizeof rep);
        s = musicpack_package_verify(pkg, &rep, 0, 0);
        CHECK(s != MUSICPACK_OK && rep.errors > 0, "wrong analysis sha fails verify");
        musicpack_package_close(pkg);
    }
    free(doc);
    remove_dir_tree(root);

    /* malformed sonic document: error */
    if (make_temp_dir(root, sizeof root) != 0)
        return;
    doc = build_doc("musicpack-sonic-openl3-v1", 512, tracks, 2, 0, 0, 2,
                    "musicpack", "test");
    {
        char *mut = str_replace(doc, "musicpack-sonic", "musicpack-sonicX");
        free(doc);
        doc = mut;
    }
    make_package(root, doc, "musicpack-sonic-openl3-v1");
    pkg = musicpack_package_open_dir(root, &s);
    CHECK(pkg != 0, "package opens (malformed sonic)");
    if (pkg != 0) {
        memset(&rep, 0, sizeof rep);
        s = musicpack_package_verify(pkg, &rep, 0, 0);
        CHECK(s != MUSICPACK_OK && rep.errors > 0, "malformed sonic fails verify");
        musicpack_package_close(pkg);
    }
    free(doc);
    remove_dir_tree(root);

    /* unknown profile: package stays valid, warning emitted */
    if (make_temp_dir(root, sizeof root) != 0)
        return;
    doc = build_doc("musicpack-sonic-futuremodel-v1", 512, tracks, 2, 0, 0, 2,
                    "musicpack", "test");
    make_package(root, doc, "musicpack-sonic-futuremodel-v1");
    pkg = musicpack_package_open_dir(root, &s);
    CHECK(pkg != 0, "package opens (unknown profile)");
    if (pkg != 0) {
        memset(&rep, 0, sizeof rep);
        s = musicpack_package_verify(pkg, &rep, 0, 0);
        CHECK(s == MUSICPACK_OK && rep.errors == 0 && rep.warnings > 0,
              "unknown profile: warning, not error");
        musicpack_package_close(pkg);
    }
    free(doc);
    remove_dir_tree(root);

    /* manifest profile vs document profile mismatch: error */
    if (make_temp_dir(root, sizeof root) != 0)
        return;
    doc = build_doc("musicpack-sonic-openl3-v1", 512, tracks, 2, 0, 0, 2,
                    "musicpack", "test");
    make_package(root, doc, "musicpack-sonic-discogs-v1");
    pkg = musicpack_package_open_dir(root, &s);
    CHECK(pkg != 0, "package opens (profile mismatch)");
    if (pkg != 0) {
        memset(&rep, 0, sizeof rep);
        s = musicpack_package_verify(pkg, &rep, 0, 0);
        CHECK(s != MUSICPACK_OK && rep.errors > 0, "profile mismatch fails verify");
        musicpack_package_close(pkg);
    }
    free(doc);
    remove_dir_tree(root);

    /* no sonic at all: package verifies clean (baseline regression) */
    if (make_temp_dir(root, sizeof root) != 0)
        return;
    make_package(root, 0, 0);
    pkg = musicpack_package_open_dir(root, &s);
    CHECK(pkg != 0, "package opens (no sonic)");
    if (pkg != 0) {
        memset(&rep, 0, sizeof rep);
        CHECK_STATUS(musicpack_package_verify(pkg, &rep, 0, 0), MUSICPACK_OK,
                     "no-sonic package verifies clean");
        musicpack_package_close(pkg);
    }
    remove_dir_tree(root);
}

int
main(void)
{
    test_profile_id_valid();
    test_vectors();
    test_parse_ok();
    test_parse_invalid();
    test_validate();
    test_manifest_analysis();
    test_package_verify();

    if (failures == 0) {
        printf("all sonic tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d sonic test failure(s)\n", failures);
    return 1;
}
