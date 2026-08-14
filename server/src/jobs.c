/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
  (BSD 3-clause, see jobs.h)
*/
#include "jobs.h"
#include "library.h"
#include "log.h"
#include "scanner.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void
now_iso(char *buf, size_t cap)
{
    time_t t = time(0);
    struct tm utc;
    gmtime_r(&t, &utc);
    strftime(buf, cap, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

struct job_arg {
    mp_job_state *st;
    const mp_config *cfg;
};

void
mp_jobs_init(mp_job_state *st)
{
    if (st == 0)
        return;
    memset(st, 0, sizeof *st);
    pthread_mutex_init(&st->lock, 0);
}

void
mp_jobs_snapshot(mp_job_state *st, mp_job_state *out)
{
    if (st == 0 || out == 0)
        return;
    pthread_mutex_lock(&st->lock);
    *out = *st;
    pthread_mutex_unlock(&st->lock);
}

/* progress callbacks run on the worker thread; every field update is guarded
   by the state mutex so the HTTP status handler reads a consistent snapshot. */

static void
scan_progress(void *ctx, const mp_scan_result *r)
{
    mp_job_state *st = (mp_job_state *) ctx;
    pthread_mutex_lock(&st->lock);
    st->packages_scanned = r->total;
    st->added = r->added;
    st->updated = r->updated;
    st->removed = r->removed;
    st->invalid = r->invalid;
    pthread_mutex_unlock(&st->lock);
}

static void
verify_progress(void *ctx, const mp_verify_result *r)
{
    mp_job_state *st = (mp_job_state *) ctx;
    pthread_mutex_lock(&st->lock);
    st->verified_total = r->total;
    st->verified_passed = r->passed;
    st->verified_warnings = r->warnings;
    st->verified_failed = r->failed;
    pthread_mutex_unlock(&st->lock);
}

static void *
job_worker(void *arg)
{
    struct job_arg *p = (struct job_arg *) arg;
    mp_job_state *st = p->st;
    const mp_config *cfg = p->cfg;
    mp_library *lib;
    char err[256];

    free(p);

    lib = mp_library_open(cfg->database, 1, err, sizeof err);
    if (lib == 0) {
        MP_LOGE("job: cannot open database: %s", err);
        goto done;
    }
    if (st->kind == MP_JOB_SCAN) {
        mp_scan_result res;
        MP_LOGI("job: scan started");
        if (mp_scan_library(lib, cfg->library, cfg->verify_on_scan, &res,
                            scan_progress, st) != MUSICPACK_OK) {
            MP_LOGE("job: scan failed");
            pthread_mutex_lock(&st->lock);
            st->failed = 1;
            pthread_mutex_unlock(&st->lock);
        }
    } else {
        mp_verify_result res;
        MP_LOGI("job: verify started");
        if (mp_verify_library(lib, cfg->library, &res, verify_progress, st)
            != MUSICPACK_OK) {
            MP_LOGE("job: verify failed");
            pthread_mutex_lock(&st->lock);
            st->failed = 1;
            pthread_mutex_unlock(&st->lock);
        }
    }
    mp_library_close(lib);
done:
    pthread_mutex_lock(&st->lock);
    st->running = 0;
    st->last_kind = st->kind;
    st->kind = MP_JOB_NONE;
    now_iso(st->finished_at, sizeof st->finished_at);
    pthread_mutex_unlock(&st->lock);
    return 0;
}

int
mp_jobs_start(mp_job_state *st, const mp_config *cfg, int kind)
{
    pthread_t t;
    struct job_arg *p;

    if (st == 0 || cfg == 0 ||
        (kind != MP_JOB_SCAN && kind != MP_JOB_VERIFY))
        return -1;
    pthread_mutex_lock(&st->lock);
    if (st->running) {
        pthread_mutex_unlock(&st->lock);
        return -1;
    }
    st->running = 1;
    st->kind = kind;
    st->failed = 0;
    st->finished_at[0] = '\0';
    now_iso(st->started_at, sizeof st->started_at);
    if (kind == MP_JOB_SCAN) {
        st->packages_scanned = st->added = st->updated = st->removed =
            st->invalid = 0;
    } else {
        st->verified_total = st->verified_passed = st->verified_warnings =
            st->verified_failed = 0;
    }
    pthread_mutex_unlock(&st->lock);
    p = (struct job_arg *) malloc(sizeof *p);
    if (p == 0) {
        pthread_mutex_lock(&st->lock);
        st->running = 0;
        st->kind = MP_JOB_NONE;
        pthread_mutex_unlock(&st->lock);
        return -1;
    }
    p->st = st;
    p->cfg = cfg;
    if (pthread_create(&t, 0, job_worker, p) != 0) {
        free(p);
        pthread_mutex_lock(&st->lock);
        st->running = 0;
        st->kind = MP_JOB_NONE;
        pthread_mutex_unlock(&st->lock);
        return -1;
    }
    pthread_detach(t);
    return 0;
}

void
mp_jobs_wait(mp_job_state *st)
{
    if (st == 0)
        return;
    for (;;) {
        pthread_mutex_lock(&st->lock);
        {
            int running = st->running;
            pthread_mutex_unlock(&st->lock);
            if (!running)
                return;
        }
        sleep(1);
    }
}
