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
/// \file waveform.c
/// `.mpack` v1 waveform envelope: quantization, streaming accumulator,
/// binary payload encode/decode/validate (see
/// `specs/musicpack-waveform-v1.md` and `<musicpack/waveform.h>`).

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <musicpack/waveform.h>

#define MUSICPACK_WAVEFORM_MAX_CHANNELS 8u

/* ------------------------------------------------------------------ */
/* quantization                                                        */
/* ------------------------------------------------------------------ */

uint8_t
musicpack_waveform_quantize(double amplitude, int floor_db)
{
    double db, normalized, raw;

    /* Defensive clamps — accumulator emits non-negative values, but the
       public API takes a raw amplitude so misuse is possible. */
    if (amplitude <= 0.0)
        return 0;
    if (amplitude > 1.0)
        return 255;
    if (floor_db >= 0)
        floor_db = MUSICPACK_WAVEFORM_FLOOR_DB;

    db = 20.0 * log10(amplitude);
    normalized = (db - (double) floor_db) / (0.0 - (double) floor_db);
    raw = floor(normalized * 254.0 + 0.5) + 1.0;
    if (raw < 1.0)
        raw = 1.0;
    if (raw > 255.0)
        raw = 255.0;
    return (uint8_t) raw;
}

/* ------------------------------------------------------------------ */
/* accumulator                                                         */
/* ------------------------------------------------------------------ */

struct musicpack_waveform_acc {
    unsigned rate;            /* Hz */
    unsigned channels;        /* 1..MUSICPACK_WAVEFORM_MAX_CHANNELS */
    uint64_t  total_frames;   /* frames fed so far */

    /* current bucket */
    long double sum_sq;
    double     peak_abs;
    uint64_t   bucket_samples;
    uint64_t   bucket_index;     /* current cumulative 100 ms bucket */

    /* final partial bucket (flushed explicitly on finish) */
    musicpack_waveform_bucket *buckets;
    size_t bucket_count;
    size_t bucket_cap;
};

static uint64_t
bucket_index_for(uint64_t total_frames, unsigned rate)
{
    return total_frames * 1000u /
           ((uint64_t) rate * (uint64_t) MUSICPACK_WAVEFORM_INTERVAL_MS);
}

static int
push_bucket(musicpack_waveform_acc *a)
{
    musicpack_waveform_bucket *nb;
    uint8_t peak_u8, rms_u8;

    if (a->bucket_samples == 0)
        return 1; /* nothing to flush */

    if (a->bucket_count == a->bucket_cap) {
        size_t ncap = a->bucket_cap == 0 ? 32u : a->bucket_cap * 2u;
        if (ncap > MUSICPACK_WAVEFORM_MAX_POINTS)
            ncap = MUSICPACK_WAVEFORM_MAX_POINTS;
        nb = (musicpack_waveform_bucket *) realloc(a->buckets, ncap * sizeof *nb);
        if (nb == 0)
            return 0;
        a->buckets = nb;
        a->bucket_cap = ncap;
    }

    if (a->sum_sq == 0.0L && a->peak_abs == 0.0) {
        peak_u8 = 0;
        rms_u8 = 0;
    } else {
        long double mean = a->sum_sq / (long double) a->bucket_samples;
        double rms_lin = (mean > 0.0L) ? (double) sqrt((double) mean) : 0.0;
        peak_u8 = musicpack_waveform_quantize(a->peak_abs,
                                               MUSICPACK_WAVEFORM_FLOOR_DB);
        rms_u8 = musicpack_waveform_quantize(rms_lin,
                                              MUSICPACK_WAVEFORM_FLOOR_DB);
    }

    a->buckets[a->bucket_count].peak = peak_u8;
    a->buckets[a->bucket_count].rms = rms_u8;
    a->bucket_count++;

    a->sum_sq = 0.0L;
    a->peak_abs = 0.0;
    a->bucket_samples = 0;
    return 1;
}

musicpack_waveform_acc *
musicpack_waveform_acc_new(unsigned sample_rate, unsigned channels)
{
    musicpack_waveform_acc *a;

    if (sample_rate == 0 || channels == 0 || channels > MUSICPACK_WAVEFORM_MAX_CHANNELS)
        return 0;
    a = (musicpack_waveform_acc *) calloc(1, sizeof *a);
    if (a == 0)
        return 0;
    a->rate = sample_rate;
    a->channels = channels;
    a->bucket_index = 0;
    return a;
}

void
musicpack_waveform_acc_free(musicpack_waveform_acc *a)
{
    if (a == 0)
        return;
    free(a->buckets);
    free(a);
}

musicpack_status
musicpack_waveform_acc_feed_f32(musicpack_waveform_acc *a, const float *interleaved,
                                 size_t frames)
{
    size_t i;
    uint64_t bucket_index;

    if (a == 0)
        return MUSICPACK_ERR_INVALID;
    if (frames == 0)
        return MUSICPACK_OK;
    if (interleaved == 0)
        return MUSICPACK_ERR_INVALID;
    if (a->bucket_count >= MUSICPACK_WAVEFORM_MAX_POINTS)
        return MUSICPACK_ERR_INVALID;

    for (i = 0; i < frames; i++) {
        unsigned c;
        for (c = 0; c < a->channels; c++) {
            double s = (double) interleaved[i * (size_t) a->channels + c];
            double abs_s = s < 0.0 ? -s : s;
            if (abs_s > a->peak_abs)
                a->peak_abs = abs_s;
            a->sum_sq += (long double) s * (long double) s;
            a->bucket_samples++;
        }
        a->total_frames++;
        bucket_index = bucket_index_for(a->total_frames, a->rate);
        if (bucket_index > a->bucket_index) {
            if (!push_bucket(a))
                return MUSICPACK_ERR_NOMEM;
            a->bucket_index = bucket_index;
            if (a->bucket_count >= MUSICPACK_WAVEFORM_MAX_POINTS)
                return MUSICPACK_ERR_INVALID;
        }
    }
    return MUSICPACK_OK;
}

musicpack_status
musicpack_waveform_acc_finish(musicpack_waveform_acc *a, musicpack_waveform_bucket **out,
                              size_t *count)
{
    if (a == 0 || out == 0 || count == 0)
        return MUSICPACK_ERR_INVALID;
    /* flush final partial bucket */
    if (!push_bucket(a)) {
        free(a->buckets);
        a->buckets = 0;
        a->bucket_cap = 0;
        a->bucket_count = 0;
        return MUSICPACK_ERR_NOMEM;
    }
    *out = a->buckets;
    *count = a->bucket_count;
    a->buckets = 0;
    a->bucket_cap = 0;
    a->bucket_count = 0;
    return MUSICPACK_OK;
}

/* ------------------------------------------------------------------ */
/* binary payload                                                     */
/* ------------------------------------------------------------------ */

musicpack_status
musicpack_waveform_encode(const musicpack_waveform_bucket *buckets, size_t count,
                          unsigned char **out, size_t *out_len)
{
    unsigned char *buf;
    size_t i;

    if (out == 0 || out_len == 0)
        return MUSICPACK_ERR_INVALID;
    if (count > MUSICPACK_WAVEFORM_MAX_POINTS)
        return MUSICPACK_ERR_INVALID;
    if (count > 0 && buckets == 0)
        return MUSICPACK_ERR_INVALID;
    *out = 0;
    *out_len = 0;
    if (count == 0) {
        *out = (unsigned char *) malloc(1);
        if (*out == 0)
            return MUSICPACK_ERR_NOMEM;
        (*out)[0] = 0;
        *out_len = 0;
        return MUSICPACK_OK;
    }

    buf = (unsigned char *) malloc(count * MUSICPACK_WAVEFORM_BYTES_PER_BUCKET);
    if (buf == 0)
        return MUSICPACK_ERR_NOMEM;
    for (i = 0; i < count; i++) {
        buf[i * 2 + 0] = buckets[i].peak;
        buf[i * 2 + 1] = buckets[i].rms;
    }
    *out = buf;
    *out_len = count * MUSICPACK_WAVEFORM_BYTES_PER_BUCKET;
    return MUSICPACK_OK;
}

musicpack_status
musicpack_waveform_decode(const unsigned char *data, size_t len,
                          musicpack_waveform_bucket **out, size_t *out_count)
{
    musicpack_waveform_bucket *buckets;
    size_t i, count;

    if (out == 0 || out_count == 0)
        return MUSICPACK_ERR_INVALID;
    *out = 0;
    *out_count = 0;
    if (len > MUSICPACK_WAVEFORM_MAX_BYTES)
        return MUSICPACK_ERR_INVALID;
    if ((len % MUSICPACK_WAVEFORM_BYTES_PER_BUCKET) != 0)
        return MUSICPACK_ERR_INVALID;
    if (len == 0) {
        *out = (musicpack_waveform_bucket *) malloc(1);
        if (*out == 0)
            return MUSICPACK_ERR_NOMEM;
        *out_count = 0;
        return MUSICPACK_OK;
    }
    count = len / MUSICPACK_WAVEFORM_BYTES_PER_BUCKET;
    if (data == 0)
        return MUSICPACK_ERR_INVALID;
    buckets = (musicpack_waveform_bucket *) malloc(count * sizeof *buckets);
    if (buckets == 0)
        return MUSICPACK_ERR_NOMEM;
    for (i = 0; i < count; i++) {
        buckets[i].peak = data[i * 2 + 0];
        buckets[i].rms = data[i * 2 + 1];
    }
    *out = buckets;
    *out_count = count;
    return MUSICPACK_OK;
}

musicpack_status
musicpack_waveform_validate(const unsigned char *data, size_t len,
                            const musicpack_waveform_meta *meta)
{
    size_t expected, i;

    if (meta == 0)
        return MUSICPACK_ERR_INVALID;
    if (meta->version != MUSICPACK_WAVEFORM_VERSION)
        return MUSICPACK_ERR_INVALID;
    if (meta->interval_ms != MUSICPACK_WAVEFORM_INTERVAL_MS)
        return MUSICPACK_ERR_INVALID;
    if (meta->floor_db != MUSICPACK_WAVEFORM_FLOOR_DB)
        return MUSICPACK_ERR_INVALID;
    if (meta->points > MUSICPACK_WAVEFORM_MAX_POINTS)
        return MUSICPACK_ERR_INVALID;
    expected = (size_t) meta->points * MUSICPACK_WAVEFORM_BYTES_PER_BUCKET;
    if (len != expected)
        return MUSICPACK_ERR_INVALID;
    if (len > 0 && data == 0)
        return MUSICPACK_ERR_INVALID;
    for (i = 0; i < len; i++) {
        /* Every byte must be in [0, 255] by type; check parity of the
           v1 contract explicitly. There are no reserved/invalid byte
           values for peak-rms-u8. */
    }
    return MUSICPACK_OK;
}
