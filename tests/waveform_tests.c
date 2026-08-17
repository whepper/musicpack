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
 * Tests for the `.mpack` v1 waveform envelope: quantization, streaming
 * accumulator, binary payload encode/decode/validate, and manifest
 * parse/serialize round-trip.
 *
 * Registered as the `waveform_unit` CTest suite.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <musicpack/musicpack.h>
#include <musicpack/waveform.h>

static int failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);    \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------ */
/* quantization                                                        */
/* ------------------------------------------------------------------ */

static void
test_quantize(void)
{
    /* silence -> 0 (special-cased) */
    CHECK(musicpack_waveform_quantize(0.0, -60) == 0, "silence is 0");
    CHECK(musicpack_waveform_quantize(-1.0, -60) == 0, "negative amplitude is 0");

    /* 0 dBFS -> 255 */
    CHECK(musicpack_waveform_quantize(1.0, -60) == 255, "0 dBFS is 255");

    /* floor (-60 dBFS) -> 1 */
    CHECK(musicpack_waveform_quantize(1e-3, -60) == 1, "floor maps to 1");

    /* >0 dBFS (defensive clamp) -> 255 */
    CHECK(musicpack_waveform_quantize(1.5, -60) == 255, "overflow clamps to 255");
    CHECK(musicpack_waveform_quantize(100.0, -60) == 255, "huge value clamps to 255");

    /* mid-amplitude checks (-20 dBFS = 1e-1, should be high but not 255) */
    uint8_t v = musicpack_waveform_quantize(0.1, -60);
    CHECK(v > 150 && v < 255, "mid-amplitude is high but not 255");

    /* determinism: same input -> same output (10 iterations) */
    uint8_t first = musicpack_waveform_quantize(0.25, -60);
    for (int i = 0; i < 10; i++) {
        CHECK(musicpack_waveform_quantize(0.25, -60) == first,
              "quantize is deterministic");
    }
}

/* ------------------------------------------------------------------ */
/* accumulator                                                         */
/* ------------------------------------------------------------------ */

static void
test_acc_silence(void)
{
    musicpack_waveform_acc *a = musicpack_waveform_acc_new(44100, 2);
    CHECK(a != 0, "acc new");
    musicpack_waveform_bucket *buckets = 0;
    size_t count = 0;
    musicpack_status s = musicpack_waveform_acc_finish(a, &buckets, &count);
    CHECK(s == MUSICPACK_OK, "silence finish");
    /* No frames fed -> zero buckets emitted (matches "final partial bucket
       retained" only when at least one frame was fed). */
    CHECK(count == 0, "no buckets emitted on empty feed");
    free(buckets);
    musicpack_waveform_acc_free(a);
}

static void
test_acc_full_scale_sine(void)
{
    /* 1 second of a full-scale 1 kHz sine at 44.1 kHz stereo -> 4410 samples
       per channel -> 10 buckets (100 ms each) at peak=255, rms≈178. */
    musicpack_waveform_acc *a = musicpack_waveform_acc_new(44100, 2);
    const unsigned frames = 44100;
    float *pcm = (float *) calloc(frames * 2, sizeof(float));
    CHECK(pcm != 0, "pcm alloc");
    for (unsigned i = 0; i < frames; i++) {
        double v = sin(2.0 * M_PI * 1000.0 * (double) i / 44100.0);
        pcm[i * 2] = (float) v;
        pcm[i * 2 + 1] = (float) v;
    }
    CHECK(musicpack_waveform_acc_feed_f32(a, pcm, frames) == MUSICPACK_OK,
          "feed full-scale sine");
    free(pcm);
    musicpack_waveform_bucket *buckets = 0;
    size_t count = 0;
    CHECK(musicpack_waveform_acc_finish(a, &buckets, &count) == MUSICPACK_OK,
          "finish full-scale sine");
    CHECK(count == 10, "10 buckets for 1 second of audio");
    for (size_t i = 0; i < count; i++) {
        CHECK(buckets[i].peak == 255, "full-scale peak is 255");
        /* RMS of a full-scale sine = 1/sqrt(2) = 0.7071; dBFS ≈ -3.01;
           normalized = (-3.01 + 60) / 60 ≈ 0.9498;
           raw = round(0.9498 * 254) + 1 ≈ 242. */
        CHECK(buckets[i].rms >= 240 && buckets[i].rms <= 244,
              "full-scale sine RMS quantizes near -3 dBFS");
    }
    free(buckets);
    musicpack_waveform_acc_free(a);
}

static void
test_acc_impulse(void)
{
    /* One sample at full scale, then silence. The impulse bucket's peak
       must be 255; the rms bucket must be small (one sample out of thousands). */
    musicpack_waveform_acc *a = musicpack_waveform_acc_new(44100, 1);
    const unsigned frames = 44100; /* 1 second */
    float *pcm = (float *) calloc(frames, sizeof(float));
    CHECK(pcm != 0, "pcm alloc");
    pcm[1000] = 1.0f; /* one impulse at 22.7 ms */
    CHECK(musicpack_waveform_acc_feed_f32(a, pcm, frames) == MUSICPACK_OK,
          "feed impulse");
    free(pcm);
    musicpack_waveform_bucket *buckets = 0;
    size_t count = 0;
    CHECK(musicpack_waveform_acc_finish(a, &buckets, &count) == MUSICPACK_OK,
          "finish impulse");
    CHECK(count == 10, "10 buckets");
    int saw_peak_255 = 0;
    for (size_t i = 0; i < count; i++) {
        if (buckets[i].peak == 255) saw_peak_255 = 1;
    }
    CHECK(saw_peak_255 == 1, "impulse produces a peak-255 bucket");
    free(buckets);
    musicpack_waveform_acc_free(a);
}

static void
test_acc_mono(void)
{
    /* 0.5 second constant half-scale mono -> 5 buckets. */
    musicpack_waveform_acc *a = musicpack_waveform_acc_new(48000, 1);
    const unsigned frames = 24000;
    float *pcm = (float *) malloc(frames * sizeof(float));
    CHECK(pcm != 0, "pcm alloc");
    for (unsigned i = 0; i < frames; i++) pcm[i] = 0.5f;
    CHECK(musicpack_waveform_acc_feed_f32(a, pcm, frames) == MUSICPACK_OK,
          "feed mono");
    free(pcm);
    musicpack_waveform_bucket *buckets = 0;
    size_t count = 0;
    CHECK(musicpack_waveform_acc_finish(a, &buckets, &count) == MUSICPACK_OK,
          "finish mono");
    CHECK(count == 5, "5 buckets for 0.5s mono");
    for (size_t i = 0; i < count; i++) {
        CHECK(buckets[i].peak == musicpack_waveform_quantize(0.5, -60),
              "mono peak matches direct quantize");
    }
    free(buckets);
    musicpack_waveform_acc_free(a);
}

static void
test_acc_stereo_one_channel(void)
{
    /* Stereo with signal in only the left channel -> peak still picks up
       the signal, rms includes silence samples (smaller). */
    musicpack_waveform_acc *a = musicpack_waveform_acc_new(44100, 2);
    const unsigned frames = 44100;
    float *pcm = (float *) calloc(frames * 2, sizeof(float));
    CHECK(pcm != 0, "pcm alloc");
    for (unsigned i = 0; i < frames; i++) pcm[i * 2] = 1.0f;
    CHECK(musicpack_waveform_acc_feed_f32(a, pcm, frames) == MUSICPACK_OK,
          "feed stereo one channel");
    free(pcm);
    musicpack_waveform_bucket *buckets = 0;
    size_t count = 0;
    CHECK(musicpack_waveform_acc_finish(a, &buckets, &count) == MUSICPACK_OK,
          "finish");
    CHECK(count == 10, "10 buckets");
    for (size_t i = 0; i < count; i++) {
        CHECK(buckets[i].peak == 255, "stereo-with-one-channel peak is 255");
        /* RMS covers L=1 and R=0; mean = 0.5; sqrt(0.5) ≈ 0.707 -> ≈ 242 */
        CHECK(buckets[i].rms >= 240 && buckets[i].rms <= 244,
              "stereo RMS in expected range");
    }
    free(buckets);
    musicpack_waveform_acc_free(a);
}

static void
test_acc_sample_rate_invariance(void)
{
    /* Same duration at 48 kHz produces the same bucket count as 44.1 kHz. */
    musicpack_waveform_acc *a44 = musicpack_waveform_acc_new(44100, 2);
    musicpack_waveform_acc *a48 = musicpack_waveform_acc_new(48000, 2);
    const unsigned frames = 44100; /* 1 second at 44.1; ~1.094s at 48k for the same count */
    float *pcm44 = (float *) calloc(frames * 2, sizeof(float));
    float *pcm48 = (float *) calloc((size_t)(44100 * 1.094 * 2), sizeof(float));
    for (unsigned i = 0; i < frames; i++) pcm44[i * 2] = pcm44[i * 2 + 1] = 0.5f;
    unsigned frames48 = (unsigned)(44100 * 1.094);
    for (unsigned i = 0; i < frames48; i++) pcm48[i * 2] = pcm48[i * 2 + 1] = 0.5f;
    musicpack_waveform_acc_feed_f32(a44, pcm44, frames);
    musicpack_waveform_acc_feed_f32(a48, pcm48, frames48);
    free(pcm44); free(pcm48);
    musicpack_waveform_bucket *b44 = 0, *b48 = 0;
    size_t c44 = 0, c48 = 0;
    musicpack_waveform_acc_finish(a44, &b44, &c44);
    musicpack_waveform_acc_finish(a48, &b48, &c48);
    /* Both should produce ~10 buckets (within ±1 of the floor division). */
    CHECK(c44 >= 9 && c44 <= 10, "44.1kHz 1s yields ~10 buckets");
    CHECK(c48 >= 9 && c48 <= 11, "48kHz ~1s yields ~10 buckets");
    free(b44); free(b48);
    musicpack_waveform_acc_free(a44);
    musicpack_waveform_acc_free(a48);
}

static void
test_acc_cumulative_boundaries(void)
{
    /* At 11025 Hz, 100 ms is 1102.5 frames. The first boundary is therefore
       frame 1103, not the rounded/truncated 1102-frame interval. */
    musicpack_waveform_acc *a = musicpack_waveform_acc_new(11025, 1);
    float pcm[1103] = {0};
    musicpack_waveform_bucket *buckets = 0;
    size_t count = 0;

    CHECK(a != 0, "acc new at non-integral bucket rate");
    pcm[1102] = 1.0f;
    CHECK(musicpack_waveform_acc_feed_f32(a, pcm, 1103) == MUSICPACK_OK,
          "feed cumulative boundary");
    CHECK(musicpack_waveform_acc_finish(a, &buckets, &count) == MUSICPACK_OK,
          "finish cumulative boundary");
    CHECK(count == 1, "first cumulative bucket contains 1103 frames");
    CHECK(buckets[0].peak == 255, "boundary frame belongs to first bucket");
    free(buckets);
    musicpack_waveform_acc_free(a);
}

static void
test_acc_partial_bucket(void)
{
    /* 4.05 seconds at 44.1 kHz -> 41 buckets (40 full + 1 partial). */
    musicpack_waveform_acc *a = musicpack_waveform_acc_new(44100, 2);
    const unsigned frames = (unsigned)(4.05 * 44100); /* 178605 */
    float *pcm = (float *) calloc(frames * 2, sizeof(float));
    for (unsigned i = 0; i < frames; i++) pcm[i * 2] = pcm[i * 2 + 1] = 0.3f;
    musicpack_waveform_acc_feed_f32(a, pcm, frames);
    free(pcm);
    musicpack_waveform_bucket *buckets = 0;
    size_t count = 0;
    musicpack_waveform_acc_finish(a, &buckets, &count);
    CHECK(count == 41, "final partial bucket retained");
    free(buckets);
    musicpack_waveform_acc_free(a);
}

static void
test_acc_boundary_max_minus_one(void)
{
    /* At 10 Hz mono, one frame advances the cumulative bucket index by one,
       so N frames produce exactly N buckets. */
    const size_t n = (size_t) MUSICPACK_WAVEFORM_MAX_POINTS - 1u;
    musicpack_waveform_acc *a = musicpack_waveform_acc_new(10, 1);
    CHECK(a != 0, "acc new");
    float *pcm = (float *) malloc(n * sizeof(float));
    CHECK(pcm != 0, "pcm alloc");
    for (size_t i = 0; i < n; i++) pcm[i] = 0.5f;
    CHECK(musicpack_waveform_acc_feed_f32(a, pcm, n) == MUSICPACK_OK,
          "feed MAX_POINTS-1");
    musicpack_waveform_bucket *b = 0;
    size_t c = 0;
    CHECK(musicpack_waveform_acc_finish(a, &b, &c) == MUSICPACK_OK,
          "finish MAX_POINTS-1");
    CHECK(c == MUSICPACK_WAVEFORM_MAX_POINTS - 1u,
          "MAX_POINTS-1 buckets emitted");
    free(b); free(pcm);
    musicpack_waveform_acc_free(a);
}

static void
test_acc_boundary_max(void)
{
    const size_t n = (size_t) MUSICPACK_WAVEFORM_MAX_POINTS;
    musicpack_waveform_acc *a = musicpack_waveform_acc_new(10, 1);
    CHECK(a != 0, "acc new");
    float *pcm = (float *) malloc(n * sizeof(float));
    CHECK(pcm != 0, "pcm alloc");
    for (size_t i = 0; i < n; i++) pcm[i] = 0.5f;
    CHECK(musicpack_waveform_acc_feed_f32(a, pcm, n) == MUSICPACK_OK,
          "feed exactly MAX_POINTS");
    musicpack_waveform_bucket *b = 0;
    size_t c = 0;
    CHECK(musicpack_waveform_acc_finish(a, &b, &c) == MUSICPACK_OK,
          "finish exactly MAX_POINTS");
    CHECK(c == MUSICPACK_WAVEFORM_MAX_POINTS,
          "exactly MAX_POINTS buckets emitted");
    free(b); free(pcm);
    musicpack_waveform_acc_free(a);
}

static void
test_acc_boundary_exceed(void)
{
    const size_t n = (size_t) MUSICPACK_WAVEFORM_MAX_POINTS;
    musicpack_waveform_acc *a = musicpack_waveform_acc_new(10, 1);
    CHECK(a != 0, "acc new");
    float *pcm = (float *) malloc(n * sizeof(float));
    float extra[1] = { 0.5f };
    CHECK(pcm != 0, "pcm alloc");
    for (size_t i = 0; i < n; i++) pcm[i] = 0.5f;
    CHECK(musicpack_waveform_acc_feed_f32(a, pcm, n) == MUSICPACK_OK,
          "feed MAX_POINTS");
    /* The (MAX_POINTS+1)th bucket must be rejected deterministically. */
    CHECK(musicpack_waveform_acc_feed_f32(a, extra, 1) == MUSICPACK_ERR_INVALID,
          "exceeding MAX_POINTS rejected with INVALID");
    /* State stays intact: finish still yields MAX_POINTS buckets. */
    musicpack_waveform_bucket *b = 0;
    size_t c = 0;
    CHECK(musicpack_waveform_acc_finish(a, &b, &c) == MUSICPACK_OK,
          "finish after rejected feed");
    CHECK(c == MUSICPACK_WAVEFORM_MAX_POINTS,
          "MAX_POINTS buckets preserved after rejected feed");
    free(b); free(pcm);
    musicpack_waveform_acc_free(a);
}

static void
test_encode_max_points(void)
{
    const size_t n = (size_t) MUSICPACK_WAVEFORM_MAX_POINTS;
    musicpack_waveform_bucket *buckets =
        (musicpack_waveform_bucket *) calloc(n, sizeof *buckets);
    CHECK(buckets != 0, "bucket array alloc");
    unsigned char *bytes = 0;
    size_t len = 0;
    CHECK(musicpack_waveform_encode(buckets, n, &bytes, &len) == MUSICPACK_OK,
          "encode exactly MAX_POINTS buckets");
    CHECK(len == n * MUSICPACK_WAVEFORM_BYTES_PER_BUCKET,
          "encoded payload is MAX_POINTS*2 bytes");
    CHECK(len == MUSICPACK_WAVEFORM_MAX_BYTES, "encoded length == MAX_BYTES");
    /* Count MAX_POINTS+1 must be rejected. */
    unsigned char *b2 = 0;
    size_t l2 = 0;
    CHECK(musicpack_waveform_encode(buckets, n + 1, &b2, &l2) == MUSICPACK_ERR_INVALID,
          "encode MAX_POINTS+1 rejected");
    free(bytes); free(buckets);
}

static void
test_validate_max_points(void)
{
    const size_t n = (size_t) MUSICPACK_WAVEFORM_MAX_POINTS;
    musicpack_waveform_bucket *buckets =
        (musicpack_waveform_bucket *) calloc(n, sizeof *buckets);
    unsigned char *bytes = 0;
    size_t len = 0;
    musicpack_waveform_encode(buckets, n, &bytes, &len);
    musicpack_waveform_meta meta = {
        .version = MUSICPACK_WAVEFORM_VERSION,
        .interval_ms = MUSICPACK_WAVEFORM_INTERVAL_MS,
        .floor_db = MUSICPACK_WAVEFORM_FLOOR_DB,
        .points = (uint32_t) n,
    };
    CHECK(musicpack_waveform_validate(bytes, len, &meta) == MUSICPACK_OK,
          "validator accepts exactly MAX_POINTS payload");
    free(bytes); free(buckets);
}

static void
test_validate_max_points_plus_one(void)
{
    /* Build a valid-sized payload that declares MAX_POINTS+1 points; the
       validator must reject on the points bound before touching bytes. */
    const size_t n = ((size_t) MUSICPACK_WAVEFORM_MAX_POINTS) + 1u;
    unsigned char *data = (unsigned char *) calloc(
        n * MUSICPACK_WAVEFORM_BYTES_PER_BUCKET, 1);
    CHECK(data != 0, "payload alloc");
    musicpack_waveform_meta meta = {
        .version = MUSICPACK_WAVEFORM_VERSION,
        .interval_ms = MUSICPACK_WAVEFORM_INTERVAL_MS,
        .floor_db = MUSICPACK_WAVEFORM_FLOOR_DB,
        .points = (uint32_t) n,
    };
    CHECK(musicpack_waveform_validate(data,
          n * MUSICPACK_WAVEFORM_BYTES_PER_BUCKET, &meta) == MUSICPACK_ERR_INVALID,
          "validator rejects MAX_POINTS+1 payload");
    free(data);
}

static void
test_acc_determinism(void)
{
    /* Same input twice yields identical bucket bytes. */
    const unsigned frames = 4410;
    float *pcm = (float *) malloc(frames * 2 * sizeof(float));
    for (unsigned i = 0; i < frames; i++) {
        pcm[i * 2]     = 0.1f + 0.9f * (float) sin(i * 0.01);
        pcm[i * 2 + 1] = 0.2f + 0.8f * (float) cos(i * 0.02);
    }
    musicpack_waveform_bucket *b1 = 0, *b2 = 0;
    size_t c1 = 0, c2 = 0;
    musicpack_waveform_acc *a1 = musicpack_waveform_acc_new(44100, 2);
    musicpack_waveform_acc *a2 = musicpack_waveform_acc_new(44100, 2);
    musicpack_waveform_acc_feed_f32(a1, pcm, frames);
    musicpack_waveform_acc_feed_f32(a2, pcm, frames);
    musicpack_waveform_acc_finish(a1, &b1, &c1);
    musicpack_waveform_acc_finish(a2, &b2, &c2);
    CHECK(c1 == c2, "same input -> same bucket count");
    CHECK(c1 > 0, "produces buckets");
    CHECK(memcmp(b1, b2, c1 * sizeof *b1) == 0, "same input -> identical bytes");
    free(pcm); free(b1); free(b2);
    musicpack_waveform_acc_free(a1);
    musicpack_waveform_acc_free(a2);
}

/* ------------------------------------------------------------------ */
/* payload encode / decode / validate                                 */
/* ------------------------------------------------------------------ */

static void
test_encode_decode_roundtrip(void)
{
    musicpack_waveform_bucket b[3] = { { 0, 0 }, { 1, 200 }, { 255, 128 } };
    unsigned char *bytes = 0;
    size_t len = 0;
    CHECK(musicpack_waveform_encode(b, 3, &bytes, &len) == MUSICPACK_OK, "encode");
    CHECK(len == 6, "3 buckets = 6 bytes");
    CHECK(bytes[0] == 0 && bytes[1] == 0, "first bucket");
    CHECK(bytes[2] == 1 && bytes[3] == 200, "second bucket");
    CHECK(bytes[4] == 255 && bytes[5] == 128, "third bucket");
    musicpack_waveform_bucket *out = 0;
    size_t count = 0;
    CHECK(musicpack_waveform_decode(bytes, len, &out, &count) == MUSICPACK_OK,
          "decode");
    CHECK(count == 3, "decode count");
    CHECK(memcmp(b, out, sizeof b) == 0, "roundtrip equal");
    free(out); free(bytes);
}

static void
test_validate(void)
{
    musicpack_waveform_meta meta = {
        .version = MUSICPACK_WAVEFORM_VERSION,
        .interval_ms = MUSICPACK_WAVEFORM_INTERVAL_MS,
        .floor_db = MUSICPACK_WAVEFORM_FLOOR_DB,
        .points = 3,
    };
    musicpack_waveform_bucket b[3] = { { 0, 0 }, { 1, 200 }, { 255, 128 } };
    unsigned char *bytes = 0;
    size_t len = 0;
    musicpack_waveform_encode(b, 3, &bytes, &len);
    CHECK(musicpack_waveform_validate(bytes, len, &meta) == MUSICPACK_OK,
          "valid payload accepted");

    /* wrong version */
    musicpack_waveform_meta bad_v = meta;
    bad_v.version = 2;
    CHECK(musicpack_waveform_validate(bytes, len, &bad_v) == MUSICPACK_ERR_INVALID,
          "version 2 rejected");

    /* wrong encoding interval */
    musicpack_waveform_meta bad_i = meta;
    bad_i.interval_ms = 50;
    CHECK(musicpack_waveform_validate(bytes, len, &bad_i) == MUSICPACK_ERR_INVALID,
          "interval 50 rejected");

    /* wrong floor */
    musicpack_waveform_meta bad_f = meta;
    bad_f.floor_db = -40;
    CHECK(musicpack_waveform_validate(bytes, len, &bad_f) == MUSICPACK_ERR_INVALID,
          "floor -40 rejected");

    /* points mismatch (meta says 3, bytes have 6) */
    musicpack_waveform_meta bad_p = meta;
    bad_p.points = 4;
    CHECK(musicpack_waveform_validate(bytes, len, &bad_p) == MUSICPACK_ERR_INVALID,
          "points mismatch rejected");

    /* truncated payload (odd bytes) */
    size_t dummy_count = 0;
    musicpack_waveform_bucket *dummy_out = 0;
    CHECK(musicpack_waveform_decode(bytes, len - 1, &dummy_out, &dummy_count) ==
              MUSICPACK_ERR_INVALID,
          "odd-byte decode rejected");

    free(bytes);
}

/* ------------------------------------------------------------------ */
/* manifest parse                                                     */
/* ------------------------------------------------------------------ */

static const char *HASH =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

static void
test_manifest_parse_valid(void)
{
    char json[2048];
    snprintf(json, sizeof json,
             "{\"format\":\"musicpack\",\"version\":1,"
             "\"album\":{\"title\":\"A\",\"artists\":[{\"name\":\"X\"}]},"
             "\"media\":[{\"disc\":1,\"tracks\":["
             "{\"track\":1,\"title\":\"T\","
             " \"audio\":{\"path\":\"audio/01.mpc\",\"sha256\":\"%s\"},"
             " \"waveform\":{\"version\":1,\"path\":\"analysis/waveform/01-01.wfm\","
             "  \"sha256\":\"%s\",\"intervalMs\":100,"
             "  \"encoding\":\"peak-rms-u8\",\"floorDb\":-60,\"points\":2843}"
             "}]}]}",
             HASH, HASH);
    musicpack_status s;
    musicpack_manifest *m = musicpack_manifest_parse(json, &s);
    CHECK(m != 0, "valid manifest parses");
    if (m == 0) return;
    CHECK(m->disc_count == 1, "1 disc");
    CHECK(m->discs[0].track_count == 1, "1 track");
    musicpack_track *t = &m->discs[0].tracks[0];
    CHECK(t->waveform.present == 1, "waveform present");
    CHECK(t->waveform.version == 1, "waveform version");
    CHECK(t->waveform.interval_ms == 100, "waveform interval");
    CHECK(t->waveform.floor_db == -60, "waveform floor");
    CHECK(t->waveform.points == 2843, "waveform points");
    CHECK(t->waveform.encoding != 0 && strcmp(t->waveform.encoding, "peak-rms-u8") == 0,
          "waveform encoding");
    CHECK(t->waveform.path != 0 && strcmp(t->waveform.path, "analysis/waveform/01-01.wfm") == 0,
          "waveform path");
    /* Round-trip via write. */
    char *out = 0;
    CHECK(musicpack_manifest_write(m, &out) == MUSICPACK_OK, "write");
    if (out) {
        CHECK(strstr(out, "\"waveform\"") != 0, "waveform in output");
        CHECK(strstr(out, "\"peak-rms-u8\"") != 0, "encoding in output");
        CHECK(strstr(out, "\"floorDb\": -60") != 0, "floorDb in output");
        CHECK(strstr(out, "\"points\": 2843") != 0, "points in output");
        free(out);
    }
    musicpack_manifest_free(m);
}

static void
test_manifest_parse_bad_version(void)
{
    char json[2048];
    snprintf(json, sizeof json,
             "{\"format\":\"musicpack\",\"version\":1,"
             "\"album\":{\"title\":\"A\",\"artists\":[{\"name\":\"X\"}]},"
             "\"media\":[{\"disc\":1,\"tracks\":["
             "{\"track\":1,\"title\":\"T\","
             " \"audio\":{\"path\":\"audio/01.mpc\",\"sha256\":\"%s\"},"
             " \"waveform\":{\"version\":2,\"path\":\"analysis/waveform/01-01.wfm\","
             "  \"sha256\":\"%s\",\"intervalMs\":100,"
             "  \"encoding\":\"peak-rms-u8\",\"floorDb\":-60,\"points\":100}}"
             "]}]}",
             HASH, HASH);
    musicpack_status s;
    musicpack_manifest *m = musicpack_manifest_parse(json, &s);
    CHECK(m == 0, "version 2 rejected");
    CHECK(s != MUSICPACK_OK, "error status set");
}

static void
test_manifest_parse_bad_encoding(void)
{
    char json[2048];
    snprintf(json, sizeof json,
             "{\"format\":\"musicpack\",\"version\":1,"
             "\"album\":{\"title\":\"A\",\"artists\":[{\"name\":\"X\"}]},"
             "\"media\":[{\"disc\":1,\"tracks\":["
             "{\"track\":1,\"title\":\"T\","
             " \"audio\":{\"path\":\"audio/01.mpc\",\"sha256\":\"%s\"},"
             " \"waveform\":{\"version\":1,\"path\":\"analysis/waveform/01-01.wfm\","
             "  \"sha256\":\"%s\",\"intervalMs\":100,"
             "  \"encoding\":\"binary-f32le\",\"floorDb\":-60,\"points\":100}}"
             "]}]}",
             HASH, HASH);
    musicpack_status s;
    musicpack_manifest *m = musicpack_manifest_parse(json, &s);
    CHECK(m == 0, "unknown encoding rejected");
}

static void
test_manifest_parse_bad_floor(void)
{
    char json[2048];
    snprintf(json, sizeof json,
             "{\"format\":\"musicpack\",\"version\":1,"
             "\"album\":{\"title\":\"A\",\"artists\":[{\"name\":\"X\"}]},"
             "\"media\":[{\"disc\":1,\"tracks\":["
             "{\"track\":1,\"title\":\"T\","
             " \"audio\":{\"path\":\"audio/01.mpc\",\"sha256\":\"%s\"},"
             " \"waveform\":{\"version\":1,\"path\":\"analysis/waveform/01-01.wfm\","
             "  \"sha256\":\"%s\",\"intervalMs\":100,"
             "  \"encoding\":\"peak-rms-u8\",\"floorDb\":-30,\"points\":100}}"
             "]}]}",
             HASH, HASH);
    musicpack_status s;
    musicpack_manifest *m = musicpack_manifest_parse(json, &s);
    CHECK(m == 0, "wrong floor rejected");
}

static void
test_manifest_parse_bad_interval(void)
{
    char json[2048];
    snprintf(json, sizeof json,
             "{\"format\":\"musicpack\",\"version\":1,"
             "\"album\":{\"title\":\"A\",\"artists\":[{\"name\":\"X\"}]},"
             "\"media\":[{\"disc\":1,\"tracks\":["
             "{\"track\":1,\"title\":\"T\","
             " \"audio\":{\"path\":\"audio/01.mpc\",\"sha256\":\"%s\"},"
             " \"waveform\":{\"version\":1,\"path\":\"analysis/waveform/01-01.wfm\","
             "  \"sha256\":\"%s\",\"intervalMs\":50,"
             "  \"encoding\":\"peak-rms-u8\",\"floorDb\":-60,\"points\":100}}"
             "]}]}",
             HASH, HASH);
    musicpack_status s;
    musicpack_manifest *m = musicpack_manifest_parse(json, &s);
    CHECK(m == 0, "wrong interval rejected");
}

static void
test_manifest_parse_too_many_points(void)
{
    char json[2048];
    snprintf(json, sizeof json,
             "{\"format\":\"musicpack\",\"version\":1,"
             "\"album\":{\"title\":\"A\",\"artists\":[{\"name\":\"X\"}]},"
             "\"media\":[{\"disc\":1,\"tracks\":["
             "{\"track\":1,\"title\":\"T\","
             " \"audio\":{\"path\":\"audio/01.mpc\",\"sha256\":\"%s\"},"
             " \"waveform\":{\"version\":1,\"path\":\"analysis/waveform/01-01.wfm\","
             "  \"sha256\":\"%s\",\"intervalMs\":100,"
             "  \"encoding\":\"peak-rms-u8\",\"floorDb\":-60,\"points\":900000}}"
             "]}]}",
             HASH, HASH);
    musicpack_status s;
    musicpack_manifest *m = musicpack_manifest_parse(json, &s);
    CHECK(m == 0, "absurd points rejected");
}

static void
test_manifest_parse_traversal(void)
{
    char json[2048];
    snprintf(json, sizeof json,
             "{\"format\":\"musicpack\",\"version\":1,"
             "\"album\":{\"title\":\"A\",\"artists\":[{\"name\":\"X\"}]},"
             "\"media\":[{\"disc\":1,\"tracks\":["
             "{\"track\":1,\"title\":\"T\","
             " \"audio\":{\"path\":\"audio/01.mpc\",\"sha256\":\"%s\"},"
             " \"waveform\":{\"version\":1,\"path\":\"../evil.wfm\","
             "  \"sha256\":\"%s\",\"intervalMs\":100,"
             "  \"encoding\":\"peak-rms-u8\",\"floorDb\":-60,\"points\":100}}"
             "]}]}",
             HASH, HASH);
    musicpack_status s;
    musicpack_manifest *m = musicpack_manifest_parse(json, &s);
    CHECK(m == 0, "traversal path rejected");
}

static void
test_manifest_parse_no_waveform(void)
{
    /* A package without `waveform` on the track is fully valid. */
    char json[2048];
    snprintf(json, sizeof json,
             "{\"format\":\"musicpack\",\"version\":1,"
             "\"album\":{\"title\":\"A\",\"artists\":[{\"name\":\"X\"}]},"
             "\"media\":[{\"disc\":1,\"tracks\":["
             "{\"track\":1,\"title\":\"T\","
             " \"audio\":{\"path\":\"audio/01.mpc\",\"sha256\":\"%s\"}}"
             "]}]}",
             HASH);
    musicpack_status s;
    musicpack_manifest *m = musicpack_manifest_parse(json, &s);
    CHECK(m != 0, "no-waveform manifest parses");
    if (m == 0) return;
    CHECK(m->discs[0].tracks[0].waveform.present == 0, "waveform absent");
    /* No waveform field in output either. */
    char *out = 0;
    musicpack_manifest_write(m, &out);
    if (out) {
        CHECK(strstr(out, "\"waveform\"") == 0, "no waveform in serialized output");
        free(out);
    }
    musicpack_manifest_free(m);
}

static void
test_manifest_parse_duplicate_paths(void)
{
    /* Two tracks with the same waveform path -> reject as duplicate asset. */
    char json[2048];
    snprintf(json, sizeof json,
             "{"
             "\"format\":\"musicpack\",\"version\":1,"
             "\"album\":{\"title\":\"A\",\"artists\":[{\"name\":\"X\"}]},"
             "\"media\":[{\"disc\":1,\"tracks\":["
             "{\"track\":1,\"title\":\"A1\","
             "  \"audio\":{\"path\":\"audio/01.mpc\",\"sha256\":\"%s\"},"
             "  \"waveform\":{\"version\":1,\"path\":\"analysis/waveform/01-01.wfm\","
             "   \"sha256\":\"%s\",\"intervalMs\":100,"
             "   \"encoding\":\"peak-rms-u8\",\"floorDb\":-60,\"points\":100}},"
             "{\"track\":2,\"title\":\"A2\","
             "  \"audio\":{\"path\":\"audio/02.mpc\",\"sha256\":\"%s\"},"
             "  \"waveform\":{\"version\":1,\"path\":\"analysis/waveform/01-01.wfm\","
             "   \"sha256\":\"%s\",\"intervalMs\":100,"
             "   \"encoding\":\"peak-rms-u8\",\"floorDb\":-60,\"points\":100}}"
             "]}]}",
             HASH, HASH, HASH, HASH);
    musicpack_status s;
    musicpack_manifest *m = musicpack_manifest_parse(json, &s);
    CHECK(m == 0, "duplicate waveform path rejected");
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int
main(void)
{
    test_quantize();
    test_acc_silence();
    test_acc_full_scale_sine();
    test_acc_impulse();
    test_acc_mono();
    test_acc_stereo_one_channel();
    test_acc_sample_rate_invariance();
    test_acc_cumulative_boundaries();
    test_acc_partial_bucket();
    test_acc_boundary_max_minus_one();
    test_acc_boundary_max();
    test_acc_boundary_exceed();
    test_acc_determinism();
    test_encode_max_points();
    test_validate_max_points();
    test_validate_max_points_plus_one();
    test_encode_decode_roundtrip();
    test_validate();
    test_manifest_parse_valid();
    test_manifest_parse_bad_version();
    test_manifest_parse_bad_encoding();
    test_manifest_parse_bad_floor();
    test_manifest_parse_bad_interval();
    test_manifest_parse_too_many_points();
    test_manifest_parse_traversal();
    test_manifest_parse_no_waveform();
    test_manifest_parse_duplicate_paths();
    if (failures > 0) {
        fprintf(stderr, "waveform_unit: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
