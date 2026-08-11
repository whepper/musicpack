/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved. (BSD-2-Clause; see the top-level headers.)

  musicpack-sonic — the Sonic analysis analyzer.

  Reads a job document on stdin (or from a file argument), computes
  per-track embeddings for the pinned profile, writes a validated
  `musicpack-sonic` document atomically, and reports progress as JSON lines
  on stdout. SIGTERM cancels cleanly: the output document is never left
  partially written and completed track-cache entries remain reusable.

  Job document (stdin or first argument):

    {
      "profile": "musicpack-sonic-openl3-v1",
      "modelDir": "...",      // model cache directory
      "cacheDir": "...",      // embedding cache directory
      "outPath":  "...",      // where to write the sonic document
      "tracks": [
        { "disc": 1, "track": 1, "path": "/abs/path/audio.mpc" },
        ...
      ]
    }

  Progress protocol (one JSON object per line on stdout):

    {"event":"model","state":"ready","path":"..."}
    {"event":"track","done":i,"total":n,"disc":d,"track":t,
     "status":"ok|no-embedding|error","message":"..."}
    {"event":"album","contributing":k,"total":n}
    {"event":"done","path":"...","sha256":"...","tracks":n,"contributing":k}
    {"event":"error","message":"..."}          // fatal, exit 1
    {"event":"cancelled"}                       // SIGTERM, exit 130
*/

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <musicpack/musicpack.h>

#include <cJSON.h>

#include "acquire.h"
#include "cache.h"
#include "decode.h"
#include "frontend.h"
#include "model.h"
#include "sonic_profile.h"

static volatile sig_atomic_t g_cancelled = 0;

static void
on_sigterm(int sig)
{
    (void) sig;
    g_cancelled = 1;
}

static void
emit_json(cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    if (s != 0) {
        printf("%s\n", s);
        free(s);
        fflush(stdout);
    }
}

static void
emit_track(int done, int total, int disc, int track, const char *status,
           const char *message)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", "track");
    cJSON_AddNumberToObject(o, "done", done);
    cJSON_AddNumberToObject(o, "total", total);
    cJSON_AddNumberToObject(o, "disc", disc);
    cJSON_AddNumberToObject(o, "track", track);
    cJSON_AddStringToObject(o, "status", status);
    if (message != 0)
        cJSON_AddStringToObject(o, "message", message);
    emit_json(o);
    cJSON_Delete(o);
}

static void
emit_error(const char *message)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", "error");
    cJSON_AddStringToObject(o, "code", "ANALYSIS_FAILED");
    cJSON_AddStringToObject(o, "message", message);
    emit_json(o);
    cJSON_Delete(o);
}

static void
emit_error_code(const char *code, const char *message)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", "error");
    cJSON_AddStringToObject(o, "code", code);
    cJSON_AddStringToObject(o, "message", message);
    emit_json(o);
    cJSON_Delete(o);
}

typedef struct {
    int disc;
    int track;
    char *path;
    int present;
    float *vec; /* dims floats when present */
} job_track;

typedef struct {
    char *model_dir;
    char *cache_dir;
    char *out_path;
    job_track *tracks;
    size_t track_count;
} job;

static int
read_job_file(const char *path, char **out)
{
    FILE *f = fopen(path, "rb");
    long len;
    char *buf;
    if (f == 0)
        return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    if ((size_t) len > MUSICPACK_SONIC_DOC_MAX) {
        fclose(f);
        return 0;
    }
    buf = (char *) malloc((size_t) len + 1);
    if (buf == 0) {
        fclose(f);
        return 0;
    }
    if (len > 0 && fread(buf, 1, (size_t) len, f) != (size_t) len) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    buf[len] = '\0';
    *out = buf;
    return 1;
}

static int
parse_job(const char *json, job *j)
{
    cJSON *root, *tracks, *item;
    int i;
    size_t n;

    memset(j, 0, sizeof *j);
    root = cJSON_Parse(json);
    if (root == 0)
        return 0;
    {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "profile");
        if (!cJSON_IsString(v) || strcmp(v->valuestring, SONIC_PROFILE_ID) != 0) {
            cJSON_Delete(root);
            return 0;
        }
    }
#define JSTR(field, key)                                                       \
    do {                                                                       \
        cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);                \
        if (cJSON_IsString(v) && v->valuestring != 0)                          \
            j->field = strdup(v->valuestring);                                 \
    } while (0)
    JSTR(model_dir, "modelDir");
    JSTR(cache_dir, "cacheDir");
    JSTR(out_path, "outPath");
#undef JSTR
    tracks = cJSON_GetObjectItemCaseSensitive(root, "tracks");
    if (!cJSON_IsArray(tracks))
        goto fail;
    n = (size_t) cJSON_GetArraySize(tracks);
    if (n == 0 || n > MUSICPACK_SONIC_MAX_TRACKS)
        goto fail;
    j->tracks = (job_track *) calloc(n, sizeof *j->tracks);
    if (j->tracks == 0)
        goto fail;
    i = 0;
    cJSON_ArrayForEach(item, tracks) {
        cJSON *d = cJSON_GetObjectItemCaseSensitive(item, "disc");
        cJSON *t = cJSON_GetObjectItemCaseSensitive(item, "track");
        cJSON *p = cJSON_GetObjectItemCaseSensitive(item, "path");
        if (!cJSON_IsNumber(d) || !cJSON_IsNumber(t) || !cJSON_IsString(p)) {
            free(j->tracks);
            j->tracks = 0;
            goto fail;
        }
        j->tracks[i].disc = (int) d->valuedouble;
        j->tracks[i].track = (int) t->valuedouble;
        j->tracks[i].path = strdup(p->valuestring);
        if (j->tracks[i].path == 0) {
            free(j->tracks);
            j->tracks = 0;
            goto fail;
        }
        i++;
    }
    j->track_count = (size_t) i;
    cJSON_Delete(root);
    return 1;
fail:
    cJSON_Delete(root);
    return 0;
}

static void
free_job(job *j)
{
    size_t i;
    for (i = 0; i < j->track_count; i++) {
        free(j->tracks[i].path);
        free(j->tracks[i].vec);
    }
    free(j->tracks);
    free(j->model_dir);
    free(j->cache_dir);
    free(j->out_path);
    memset(j, 0, sizeof *j);
}

/* Embeds one track: cache -> decode -> resample -> window -> mel -> model
   -> pool. Fills t->present/t->vec. Returns 1 on success (even when the
   track has no embedding), 0 on a hard failure. */
static int
embed_track(const job *j, job_track *t, sonic_model *model,
            const char *weights_sha)
{
    char sha[MUSICPACK_SHA256_HEX_SIZE];
    sonic_pcm pcm;
    float *res = 0;
    size_t res_n = 0;
    float *windows = 0;
    int nw = 0;
    float *mels = 0;
    float *emb = 0;
    float vec[SONIC_PROFILE_DIMENSIONS];
    int present = 0;
    int ok = 0;
    int wi;

    if (musicpack_sha256_file(t->path, sha, sizeof sha) != MUSICPACK_OK)
        return 0;

    if (sonic_cache_load(j->cache_dir, SONIC_PROFILE_ID, weights_sha, sha,
                         vec, SONIC_PROFILE_DIMENSIONS, &present)) {
        if (present) {
            t->vec = (float *) malloc(SONIC_PROFILE_DIMENSIONS * sizeof(float));
            if (t->vec == 0)
                return 0;
            memcpy(t->vec, vec, SONIC_PROFILE_DIMENSIONS * sizeof(float));
            t->present = 1;
        }
        return 1;
    }

    if (!sonic_decode(t->path, &pcm))
        return 0;

    /* resample to 48k */
    res_n = (size_t) ((double) pcm.count * SONIC_SAMPLE_RATE / pcm.sample_rate);
    res = (float *) malloc(res_n * sizeof(float));
    if (res == 0) {
        sonic_pcm_free(&pcm);
        return 0;
    }
    res_n = sonic_resample(pcm.samples, pcm.count, pcm.sample_rate, res, res_n);
    if (res_n == 0) {
        free(res);
        sonic_pcm_free(&pcm);
        return 0;
    }

    /* center + window */
    nw = sonic_frame_count(res_n + SONIC_CENTER_PAD);
    windows = (float *) malloc((size_t) nw * SONIC_FRAME * sizeof(float));
    if (windows == 0) {
        free(res);
        sonic_pcm_free(&pcm);
        return 0;
    }
    {
        size_t total = 0;
        nw = sonic_center_window(res, res_n, windows,
                                 (size_t) nw * SONIC_FRAME, &total);
        (void) total;
    }

    /* per-window mel + model */
    mels = (float *) malloc((size_t) nw * SONIC_N_MELS * SONIC_MEL_FRAMES * sizeof(float));
    emb = (float *) malloc((size_t) nw * SONIC_PROFILE_DIMENSIONS * sizeof(float));
    if (mels == 0 || emb == 0) {
        free(mels);
        free(emb);
        free(windows);
        free(res);
        sonic_pcm_free(&pcm);
        return 0;
    }
    for (wi = 0; wi < nw; wi++) {
        float *mel = mels + (size_t) wi * SONIC_N_MELS * SONIC_MEL_FRAMES;
        if (sonic_mel(windows + (size_t) wi * SONIC_FRAME, mel,
                      (size_t) SONIC_N_MELS * SONIC_MEL_FRAMES) == 0) {
            free(mels);
            free(emb);
            free(windows);
            free(res);
            sonic_pcm_free(&pcm);
            return 0;
        }
    }
    if (g_cancelled)
        goto done_cancel;
    if (!sonic_model_run(model, mels, (size_t) nw, emb,
                         (size_t) nw * SONIC_PROFILE_DIMENSIONS)) {
        free(mels);
        free(emb);
        free(windows);
        free(res);
        sonic_pcm_free(&pcm);
        return 0;
    }

    /* mean-norm pool */
    present = sonic_pool_mean_norm(emb, (size_t) nw, SONIC_PROFILE_DIMENSIONS, vec);
    if (present) {
        t->vec = (float *) malloc(SONIC_PROFILE_DIMENSIONS * sizeof(float));
        if (t->vec != 0) {
            memcpy(t->vec, vec, SONIC_PROFILE_DIMENSIONS * sizeof(float));
            t->present = 1;
        }
    }
    ok = 1;
    sonic_cache_store(j->cache_dir, SONIC_PROFILE_ID, weights_sha, sha, vec,
                      SONIC_PROFILE_DIMENSIONS, present);
done_cancel:
    free(mels);
    free(emb);
    free(windows);
    free(res);
    sonic_pcm_free(&pcm);
    (void) ok;
    return ok;
}

/* Writes the sonic document atomically and returns its sha256 (or NULL). */
static char *
write_doc(const job *j, char *sha_out, size_t sha_cap)
{
    cJSON *root, *profile, *analyzer, *album, *tracks;
    size_t i;
    char *tmp_path = 0;
    char *json = 0;
    int ok = 0;
    char *result = 0;

    root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "format", MUSICPACK_SONIC_FORMAT);
    cJSON_AddNumberToObject(root, "version", MUSICPACK_SONIC_VERSION);
    profile = cJSON_AddObjectToObject(root, "profile");
    cJSON_AddStringToObject(profile, "id", SONIC_PROFILE_ID);
    cJSON_AddNumberToObject(profile, "dimensions", SONIC_PROFILE_DIMENSIONS);
    cJSON_AddStringToObject(profile, "distance", SONIC_PROFILE_DISTANCE);
    cJSON_AddStringToObject(profile, "encoding", SONIC_PROFILE_ENCODING);
    analyzer = cJSON_AddObjectToObject(root, "analyzer");
    cJSON_AddStringToObject(analyzer, "tool", "musicpack");
    cJSON_AddStringToObject(analyzer, "toolVersion", MUSICPACK_VERSION);
    album = cJSON_AddObjectToObject(root, "album");
    tracks = cJSON_AddArrayToObject(root, "tracks");

    for (i = 0; i < j->track_count; i++) {
        job_track *t = &j->tracks[i];
        cJSON *to = cJSON_CreateObject();
        cJSON_AddNumberToObject(to, "disc", t->disc);
        cJSON_AddNumberToObject(to, "track", t->track);
        if (t->present && t->vec != 0) {
            cJSON *emb;
            char *b64 = 0;
            if (musicpack_sonic_vector_encode(t->vec, SONIC_PROFILE_DIMENSIONS,
                                              &b64) != MUSICPACK_OK) {
                cJSON_Delete(to);
                goto done;
            }
            emb = cJSON_AddObjectToObject(to, "embedding");
            cJSON_AddStringToObject(emb, "encoding", SONIC_PROFILE_ENCODING);
            cJSON_AddNumberToObject(emb, "dimensions", SONIC_PROFILE_DIMENSIONS);
            cJSON_AddStringToObject(emb, "data", b64);
            free(b64);
        } else {
            cJSON_AddNullToObject(to, "embedding");
        }
        cJSON_AddItemToArray(tracks, to);
    }

    /* album embedding: equal-track mean -> L2 over contributors */
    {
        float *vecs = (float *) malloc(j->track_count * SONIC_PROFILE_DIMENSIONS *
                                       sizeof(float));
        int *present = (int *) calloc(j->track_count, sizeof *present);
        float album_vec[SONIC_PROFILE_DIMENSIONS];
        size_t c = 0;
        for (i = 0; i < j->track_count; i++) {
            if (j->tracks[i].present && j->tracks[i].vec != 0) {
                memcpy(vecs + c * SONIC_PROFILE_DIMENSIONS, j->tracks[i].vec,
                       SONIC_PROFILE_DIMENSIONS * sizeof(float));
                present[c] = 1;
                c++;
            }
        }
        c = sonic_album_equal(vecs, present, c, SONIC_PROFILE_DIMENSIONS, album_vec);
        if (c > 0) {
            char *b64 = 0;
            if (musicpack_sonic_vector_encode(album_vec, SONIC_PROFILE_DIMENSIONS,
                                              &b64) != MUSICPACK_OK) {
                free(vecs);
                free(present);
                goto done;
            }
            {
                cJSON *emb = cJSON_AddObjectToObject(album, "embedding");
                cJSON_AddStringToObject(emb, "encoding", SONIC_PROFILE_ENCODING);
                cJSON_AddNumberToObject(emb, "dimensions", SONIC_PROFILE_DIMENSIONS);
                cJSON_AddStringToObject(emb, "data", b64);
            }
            free(b64);
        } else {
            cJSON_AddNullToObject(album, "embedding");
        }
        cJSON_AddNumberToObject(album, "tracksContributing", (double) c);
        free(vecs);
        free(present);
    }

    json = cJSON_PrintUnformatted(root);
    if (json == 0)
        goto done;

    /* self-validate before writing */
    {
        musicpack_status st = MUSICPACK_OK;
        musicpack_sonic *s = musicpack_sonic_parse(json, strlen(json), &st);
        if (s == 0 || musicpack_sonic_validate(s, 0, 0) != MUSICPACK_OK) {
            musicpack_sonic_free(s);
            goto done;
        }
        musicpack_sonic_free(s);
    }

    /* atomic write: temp file + rename */
    {
        size_t n = strlen(j->out_path) + 8;
        tmp_path = (char *) malloc(n);
        if (tmp_path == 0)
            goto done;
        snprintf(tmp_path, n, "%s.tmp", j->out_path);
        {
            FILE *f = fopen(tmp_path, "wb");
            if (f == 0)
                goto done;
            if (fputs(json, f) == EOF || fclose(f) != 0) {
                remove(tmp_path);
                goto done;
            }
        }
        if (rename(tmp_path, j->out_path) != 0) {
            remove(tmp_path);
            goto done;
        }
        tmp_path = 0;
    }
    if (musicpack_sha256_file(j->out_path, sha_out, sha_cap) == MUSICPACK_OK)
        result = sha_out;
    ok = 1;
done:
    free(tmp_path);
    free(json);
    cJSON_Delete(root);
    if (!ok)
        return 0;
    return result;
}

int
main(int argc, char **argv)
{
    const char *job_path = argc > 1 ? argv[1] : 0;
    char *job_json = 0;
    job j;
    char *model_path = 0;
    sonic_model *model = 0;
    size_t i;
    int ok = 1;
    char doc_sha[MUSICPACK_SHA256_HEX_SIZE];

    signal(SIGTERM, on_sigterm);
#ifdef SIGINT
    signal(SIGINT, on_sigterm);
#endif

    if (job_path != 0) {
        if (!read_job_file(job_path, &job_json)) {
            emit_error("cannot read job file");
            return 1;
        }
    } else {
        /* read all of stdin */
        {
            size_t cap = 65536, len = 0;
            char *buf = (char *) malloc(cap);
            if (buf == 0) {
                emit_error("out of memory");
                return 1;
            }
            for (;;) {
                size_t n = fread(buf + len, 1, 65536, stdin);
                len += n;
                if (n < 65536)
                    break;
                if (len + 65536 + 1 > cap) {
                    char *nb = (char *) realloc(buf, cap * 2);
                    if (nb == 0) {
                        free(buf);
                        emit_error("out of memory");
                        return 1;
                    }
                    buf = nb;
                    cap *= 2;
                }
            }
            buf[len] = '\0';
            job_json = buf;
        }
    }
    if (!parse_job(job_json, &j)) {
        free(job_json);
        emit_error("invalid job document");
        return 1;
    }
    free(job_json);

    model_path = 0;
    {
        sonic_model_status st = sonic_acquire_model(j.model_dir, &model_path);
        if (st != SONIC_MODEL_OK || model_path == 0) {
            free_job(&j);
            switch (st) {
            case SONIC_MODEL_CHECKSUM:
                emit_error_code("MODEL_CHECKSUM_MISMATCH",
                                "the analysis model fails the pinned SHA-256 check");
                break;
            case SONIC_MODEL_UNREADABLE:
                emit_error_code("MODEL_UNREADABLE",
                                "the analysis model cannot be read");
                break;
            default:
                emit_error_code("MODEL_MISSING",
                                "the analysis model is not installed");
                break;
            }
            return 1;
        }
    }
    {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "event", "model");
        cJSON_AddStringToObject(o, "state", "ready");
        cJSON_AddStringToObject(o, "path", model_path);
        emit_json(o);
        cJSON_Delete(o);
    }
    model = sonic_model_open(model_path);
    if (model == 0) {
        free(model_path);
        free_job(&j);
        emit_error_code("RUNTIME_LOAD_FAILED",
                        "the ONNX Runtime model could not be loaded");
        return 1;
    }

    for (i = 0; i < j.track_count && !g_cancelled; i++) {
        if (embed_track(&j, &j.tracks[i], model, SONIC_PROFILE_WEIGHTS_SHA256)) {
            emit_track((int) i + 1, (int) j.track_count, j.tracks[i].disc,
                       j.tracks[i].track,
                       j.tracks[i].present ? "ok" : "no-embedding", 0);
        } else {
            emit_track((int) i + 1, (int) j.track_count, j.tracks[i].disc,
                       j.tracks[i].track, "error",
                       "analysis failed for this track");
            ok = 0;
        }
    }

    if (g_cancelled) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "event", "cancelled");
        emit_json(o);
        cJSON_Delete(o);
        sonic_model_close(model);
        free(model_path);
        free_job(&j);
        return 130;
    }

    {
        size_t contributing = 0;
        cJSON *o;
        for (i = 0; i < j.track_count; i++)
            if (j.tracks[i].present)
                contributing++;
        o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "event", "album");
        cJSON_AddNumberToObject(o, "contributing", (double) contributing);
        cJSON_AddNumberToObject(o, "total", (double) j.track_count);
        emit_json(o);
        cJSON_Delete(o);
    }

    if (ok && write_doc(&j, doc_sha, sizeof doc_sha)) {
        size_t contributing = 0;
        cJSON *o;
        for (i = 0; i < j.track_count; i++)
            if (j.tracks[i].present)
                contributing++;
        o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "event", "done");
        cJSON_AddStringToObject(o, "path", j.out_path);
        cJSON_AddStringToObject(o, "sha256", doc_sha);
        cJSON_AddNumberToObject(o, "tracks", (double) j.track_count);
        cJSON_AddNumberToObject(o, "contributing", (double) contributing);
        emit_json(o);
        cJSON_Delete(o);
    } else {
        emit_error(ok ? "cannot write the sonic document" : "analysis failed");
        ok = 0;
    }

    sonic_model_close(model);
    free(model_path);
    free_job(&j);
    return ok ? 0 : 1;
}
