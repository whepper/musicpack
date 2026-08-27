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
 * Model-free tests for the musicpack-sonic analyzer core (sonic_core):
 * resampler (resampy kaiser_best), mel filterbank (librosa Slaney), pooling,
 * album aggregation, the per-track embedding cache, and WAV decode.
 *
 * No ONNX Runtime and no model files are required; reference constants are
 * pinned from resampy 0.4.3 / librosa 0.10.2. Wired into CTest as
 * "sonic_analyzer".
 */

#if defined(_WIN32)
# define _USE_MATH_DEFINES /* must precede <math.h> for M_PI on MSVC */
# include <windows.h>
# include <direct.h>
# define mkdir_one(p) _mkdir(p)
#else
# include <unistd.h>
# include <sys/stat.h>
# define mkdir_one(p) mkdir(p, 0755)
#endif
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <musepack/musepack.h>

#include "cache.h"
#include "decode.h"
#include "frontend.h"
#include "model_output.h"
#include "sonic_profile.h"

static int failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);    \
            failures++;                                                      \
        }                                                                    \
    } while (0)

#define CLOSE(a, b, tol, msg)                                                \
    do {                                                                     \
        double _a = (a), _b = (b);                                           \
        if (fabs(_a - _b) > (tol)) {                                         \
            fprintf(stderr, "FAIL %s:%d: %s (%.9g vs %.9g)\n", __FILE__,     \
                    __LINE__, msg, _a, _b);                                  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------ */

static void
test_resample_filter(void)
{
    double win[SONIC_RESAMPLE_N + 1];
    double delta[SONIC_RESAMPLE_N + 1];
    sonic_resample_filter(win, delta, SONIC_RESAMPLE_N + 1);

    /* Pinned taps from resampy 0.4.3 kaiser_best.npz. */
    CLOSE(win[0], 0.91734737126087607, 1e-8, "filter peak");
    CLOSE(win[1], 0.91734735230464037, 1e-8, "filter tap 1");
    CLOSE(win[2], 0.91734729543593729, 1e-8, "filter tap 2");
    CLOSE(win[8192], 0.081523303179451459, 1e-8, "filter tap 8192");
    CLOSE(win[409600], -5.2887095330227954e-08, 1e-8, "filter last tap");

    /* delta is the forward difference, last entry zero (np.diff append). */
    CLOSE(delta[0], win[1] - win[0], 1e-15, "delta 0");
    CLOSE(delta[SONIC_RESAMPLE_N], 0.0, 1e-15, "delta last");
}

static void
test_resample(void)
{
    size_t n_in = 44100, n_out;
    float *in = (float *) malloc(n_in * sizeof(float));
    float *out = (float *) malloc((size_t) 48000 * sizeof(float));
    float max_abs = 0.0f;
    size_t i;

    /* DC pass-through: gain must be 1.0 in the passband (skip the filter
       transient at both edges, which resampy shares). */
    for (i = 0; i < n_in; i++)
        in[i] = 0.5f;
    n_out = sonic_resample(in, n_in, 44100, out, 48000);
    CHECK(n_out == 48000, "resample length");
    for (i = 100; i + 100 < n_out; i++)
        CLOSE(out[i], 0.5, 1e-4, "DC preserved");

    /* A 220 Hz sine inside the passband keeps its amplitude. */
    for (i = 0; i < n_in; i++)
        in[i] = sinf((float) (2.0 * M_PI * 220.0 * (double) i / 44100.0));
    n_out = sonic_resample(in, n_in, 44100, out, 48000);
    for (i = 0; i < n_out; i++)
        if (fabsf(out[i]) > max_abs)
            max_abs = fabsf(out[i]);
    CLOSE(max_abs, 1.0, 1e-3, "sine amplitude preserved");

    /* same-rate passthrough */
    n_out = sonic_resample(in, n_in, 48000, out, 48000);
    CHECK(n_out == n_in && out[0] == in[0] && out[100] == in[100],
          "same-rate passthrough");

    free(in);
    free(out);
}

static void
test_mel_filterbank(void)
{
    float *fb = (float *) malloc((size_t) (SONIC_N_FFT / 2 + 1) * SONIC_N_MELS *
                                 sizeof(float));
    sonic_mel_filterbank(fb, (size_t) (SONIC_N_FFT / 2 + 1) * SONIC_N_MELS);
    /* Pinned values from librosa 0.10.2 filters.mel (slaney). */
    CLOSE(fb[1 * SONIC_N_MELS + 0], 0.0330104344f, 1e-7, "mel band 0 peak");
    CLOSE(fb[1 * SONIC_N_MELS + 1], 0.0299539976f, 1e-7, "mel band 1 peak");
    CLOSE(fb[2 * SONIC_N_MELS + 2], 0.0599079952f, 1e-7, "mel band 2 peak");
    CLOSE(fb[80 * SONIC_N_MELS + 100], 0.0215401892f, 1e-7, "mel band 100 peak");
    free(fb);
}

static void
test_mel(void)
{
    float *w = (float *) malloc(SONIC_FRAME * sizeof(float));
    float *mel = (float *) malloc((size_t) SONIC_N_MELS * SONIC_MEL_FRAMES *
                                  sizeof(float));
    float mx = -1e30f, mn = 1e30f;
    int i, nf;

    for (i = 0; i < SONIC_FRAME; i++)
        w[i] = sinf((float) (2.0 * M_PI * 220.0 * (double) i / SONIC_SAMPLE_RATE));
    nf = sonic_mel(w, mel, (size_t) SONIC_N_MELS * SONIC_MEL_FRAMES);
    CHECK(nf == SONIC_MEL_FRAMES, "mel frame count");
    for (i = 0; i < SONIC_N_MELS * SONIC_MEL_FRAMES; i++) {
        CHECK(isfinite(mel[i]), "mel values finite");
        if (mel[i] > mx)
            mx = mel[i];
        if (mel[i] < mn)
            mn = mel[i];
    }
    CLOSE(mx, 0.0, 1e-6, "decibel max is 0");
    CHECK(mn >= -SONIC_DYNAMIC_RANGE, "decibel floor respected");
    free(w);
    free(mel);
}

static void
test_pooling(void)
{
    float emb[4 * 8];
    float out[8];
    double norm;
    int i, j;

    for (i = 0; i < 4; i++)
        for (j = 0; j < 8; j++)
            emb[i * 8 + j] = (float) ((i + 1) * (j + 1));
    CHECK(sonic_pool_mean_norm(emb, 4, 8, out) == 1, "pool succeeds");
    norm = 0.0;
    for (j = 0; j < 8; j++)
        norm += (double) out[j] * (double) out[j];
    CLOSE(sqrt(norm), 1.0, 1e-6, "pooled vector unit norm");

    /* no windows -> no embedding */
    CHECK(sonic_pool_mean_norm(0, 0, 8, out) == 0, "empty pool fails");
}

static void
test_album(void)
{
    float vecs[3 * 4];
    float out[4];
    int present[3] = { 1, 0, 1 };
    size_t c, j;
    double norm;

    vecs[0] = 1; vecs[1] = 0; vecs[2] = 0; vecs[3] = 0;
    vecs[4] = 0; vecs[5] = 1; vecs[6] = 0; vecs[7] = 0; /* skipped */
    vecs[8] = 0; vecs[9] = 0; vecs[10] = 1; vecs[11] = 0;

    c = sonic_album_equal(vecs, present, 3, 4, out);
    CHECK(c == 2, "two contributing tracks");
    norm = 0.0;
    for (j = 0; j < 4; j++)
        norm += (double) out[j] * (double) out[j];
    CLOSE(sqrt(norm), 1.0, 1e-6, "album vector unit norm");
    /* mean of (1,0,0,0) and (0,0,1,0) = (0.5,0,0.5,0), L2 normalized */
    CLOSE(out[0], 1.0 / sqrt(2.0), 1e-6, "album x");
    CLOSE(out[2], 1.0 / sqrt(2.0), 1e-6, "album z");

    present[0] = present[2] = 0;
    CHECK(sonic_album_equal(vecs, present, 3, 4, out) == 0, "no contributors");

    {
        float cancellation[3 * 4] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            -0.4999999701976776f, 0.866025447845459f, 0.0f, 0.0f,
            -0.5f, -0.8660253882408142f, 0.0f, 0.0f
        };
        int all_present[3] = { 1, 1, 1 };
        CHECK(sonic_album_equal(cancellation, all_present, 3, 4, out) == 3,
              "cancellation-sensitive album aggregates");
        CLOSE(out[0], 0.0, 0.0, "cancellation-sensitive album x");
        CLOSE(out[1], 1.0, 0.0, "cancellation-sensitive album y");
    }
}

static int
make_temp_dir(char *buf, size_t cap)
{
#if defined(_WIN32)
    const char *base = getenv("TEMP");
    if (base == 0) base = ".";
    if (snprintf(buf, cap, "%s\\sonic_core_test_%lu", base,
                 (unsigned long) GetCurrentProcessId()) >= (int) cap)
        return -1;
    if (_mkdir(buf) != 0 && errno != EEXIST)
        return -1;
    return 0;
#else
    if (snprintf(buf, cap, "/tmp/sonic_core_test_XXXXXX") >= (int) cap)
        return -1;
    return mkdtemp(buf) != 0 ? 0 : -1;
#endif
}

static void
remove_dir_tree(const char *dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/mel", dir); remove(path);
    snprintf(path, sizeof path, "%s/sha-a.vec", dir); remove(path);
    snprintf(path, sizeof path, "%s/sha-null.vec", dir); remove(path);
    snprintf(path, sizeof path, "%s/test.wav", dir); remove(path);
    snprintf(path, sizeof path, "%s/mono.wav", dir); remove(path);
    snprintf(path, sizeof path, "%s/mono.mpc", dir); remove(path);
    snprintf(path, sizeof path, "%s/musicpack-sonic-openl3-v1/sha-a.vec", dir); remove(path);
    snprintf(path, sizeof path, "%s/musicpack-sonic-openl3-v1/sha-null.vec", dir); remove(path);
    snprintf(path, sizeof path, "%s/musicpack-sonic-openl3-v1", dir);
#if defined(_WIN32)
    _rmdir(path);
#else
    remove(path);
#endif
    snprintf(path, sizeof path, "%s", dir);
#if defined(_WIN32)
    _rmdir(path);
#else
    remove(path);
#endif
}

typedef struct {
    void *status;
    float *data;
    int released;
} fake_model_output;

static void *
fake_get_output(void *context, float **data)
{
    fake_model_output *fake = (fake_model_output *) context;
    *data = fake->data;
    return fake->status;
}

static void
fake_release_status(void *context, void *status)
{
    fake_model_output *fake = (fake_model_output *) context;
    if (status == fake->status)
        fake->released++;
}

static void
test_model_output_access(void)
{
    float data[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
    float out[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
    float unchanged[4];
    fake_model_output fake = { (void *) 1, data, 0 };

    memcpy(unchanged, out, sizeof out);
    CHECK(!sonic_model_read_output(fake_get_output, fake_release_status,
                                   &fake, out, 4),
          "model output access error fails");
    CHECK(fake.released == 1, "model output access status released");
    CHECK(memcmp(out, unchanged, sizeof out) == 0,
          "model output access error does not copy");
    fake.status = 0;
    fake.data = 0;
    CHECK(!sonic_model_read_output(fake_get_output, fake_release_status,
                                   &fake, out, 4),
          "null model output data fails");
    CHECK(memcmp(out, unchanged, sizeof out) == 0,
          "null model output data does not copy");
    fake.data = data;
    CHECK(sonic_model_read_output(fake_get_output, fake_release_status,
                                  &fake, out, 4),
          "valid model output succeeds");
    CHECK(memcmp(out, data, sizeof out) == 0,
          "valid model output is copied");
}

static void
test_decode_mono_mpc(const char *mpcenc)
{
    char dir[512], wav_path[512], mpc_path[512], command[1600];
    unsigned char hdr[44] = {
        'R', 'I', 'F', 'F', 0x24, 0x12, 0, 0, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 16, 0, 0, 0,
        1, 0, 1, 0,
        0x44, 0xac, 0, 0,
        0x88, 0x58, 0x01, 0,
        2, 0, 16, 0,
        'd', 'a', 't', 'a', 0x00, 0x12, 0, 0
    };
    int16_t samples[2304];
    sonic_pcm pcm;
    mpc_reader reader;
    musepack_decoder *dec = 0;
    musepack_stream_info info;
    uint64_t offset = 0, frames;
    float direct[1152];
    size_t i;
    FILE *f;

    if (make_temp_dir(dir, sizeof dir) != 0) {
        CHECK(0, "mono MPC temp directory created");
        return;
    }
    snprintf(wav_path, sizeof wav_path, "%s/mono.wav", dir);
    snprintf(mpc_path, sizeof mpc_path, "%s/mono.mpc", dir);
    for (i = 0; i < 2304; i++)
        samples[i] = (int16_t) (sinf((float) (2.0 * M_PI * 440.0 *
                                             (double) i / 44100.0)) * 20000.0f);
    f = fopen(wav_path, "wb");
    if (f == 0 || fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr ||
        fwrite(samples, sizeof samples[0], 2304, f) != 2304) {
        CHECK(0, "mono WAV fixture written");
        if (f != 0)
            fclose(f);
        remove_dir_tree(dir);
        return;
    }
    fclose(f);
#if defined(_WIN32)
    snprintf(command, sizeof command,
             "\"\"%s\" --silent --overwrite \"%s\" \"%s\"\"",
             mpcenc, wav_path, mpc_path);
#else
    snprintf(command, sizeof command,
             "\"%s\" --silent --overwrite \"%s\" \"%s\"",
             mpcenc, wav_path, mpc_path);
#endif
    if (system(command) != 0) {
        CHECK(0, "mono Musepack fixture encoded");
        remove_dir_tree(dir);
        return;
    }
    if (!sonic_decode(mpc_path, &pcm)) {
        CHECK(0, "mono Musepack decodes through Sonic");
        remove_dir_tree(dir);
        return;
    }
    if (mpc_reader_init_stdio(&reader, mpc_path) != MPC_STATUS_OK) {
        CHECK(0, "mono Musepack reference reader opens");
        sonic_pcm_free(&pcm);
        remove_dir_tree(dir);
        return;
    }
    dec = musepack_decoder_open(&reader, 0);
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    if (dec == 0 || musepack_decoder_get_stream_info(dec, &info) != MUSEPACK_OK ||
        info.channels != 1) {
        CHECK(0, "encoded Musepack fixture is mono");
    } else {
        while (musepack_decoder_read(dec, direct, 1152, &frames) == MUSEPACK_OK &&
               frames > 0) {
            CHECK(offset + frames <= pcm.count, "Sonic mono decode length bounded");
            if (offset + frames > pcm.count)
                break;
            for (i = 0; i < frames; i++)
                CLOSE(pcm.samples[offset + i], direct[i], 0.0,
                      "Sonic mono decode preserves channel stride");
            offset += frames;
        }
        CHECK(offset == pcm.count, "Sonic mono decode length matches decoder");
        CHECK(pcm.sample_rate == (int) info.sample_rate,
              "Sonic mono decode sample rate matches decoder");
    }
    if (dec != 0)
        musepack_decoder_close(dec);
    mpc_reader_exit_stdio(&reader);
    sonic_pcm_free(&pcm);
    remove_dir_tree(dir);
}

static void
test_cache(void)
{
    char dir[512];
    float vec[8], out[8];
    int present = 0;
    const char *profile = "musicpack-sonic-openl3-v1";
    const char *weights = "624ee7b1dd5ff87e18073f66fd8b2052bebb8ac70210e9c0937c0c940c63e9d6";

    if (make_temp_dir(dir, sizeof dir) != 0)
        return;
    memset(vec, 0, sizeof vec);
    vec[0] = 1.0f; /* unit vector (the cache decodes+validates) */
    CHECK(sonic_cache_store(dir, profile, weights, "sha-a", vec, 8, 1),
          "store vector");
    CHECK(sonic_cache_load(dir, profile, weights, "sha-a", out, 8, &present),
          "load vector");
    CHECK(present == 1 && memcmp(vec, out, sizeof vec) == 0, "round-trip");

    /* null marker */
    CHECK(sonic_cache_store(dir, profile, weights, "sha-null", 0, 8, 0),
          "store null");
    CHECK(sonic_cache_load(dir, profile, weights, "sha-null", out, 8, &present),
          "load null");
    CHECK(present == 0, "null present flag");

    /* stale on profile / weights / audio change */
    CHECK(!sonic_cache_load(dir, "musicpack-sonic-other-v1", weights, "sha-a",
                            out, 8, &present), "profile change invalidates");
    CHECK(!sonic_cache_load(dir, profile, "deadbeef", "sha-a", out, 8, &present),
          "weights change invalidates");
    CHECK(!sonic_cache_load(dir, profile, weights, "sha-b", out, 8, &present),
          "audio change misses");
    remove_dir_tree(dir);
}

static void
test_decode_wav(void)
{
    char dir[512];
    char path[512];
    /* 16-bit PCM stereo, 4 frames: L=R = -1, 0, 1, 32767 */
    unsigned char hdr[44] = {
        'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 16, 0, 0, 0,
        1, 0,           /* PCM */
        2, 0,           /* channels */
        0x44, 0xac, 0, 0, /* 44100 */
        0x10, 0xb1, 0x02, 0, /* byte rate 44100*4 */
        4, 0,           /* block align */
        16, 0,          /* bits */
        'd', 'a', 't', 'a', 16, 0, 0, 0
    };
    int16_t frames[8] = { -32768, -32768, 0, 0, 32767, 32767, 16384, 16384 };
    sonic_pcm pcm;

    if (make_temp_dir(dir, sizeof dir) != 0)
        return;
    snprintf(path, sizeof path, "%s/test.wav", dir);
    {
        FILE *f = fopen(path, "wb");
        if (f != 0) {
            fwrite(hdr, 1, sizeof hdr, f);
            fwrite(frames, 2, 8, f);
            fclose(f);
        }
    }
    if (!sonic_decode(path, &pcm)) {
        CHECK(0, "wav decodes");
        remove_dir_tree(dir);
        return;
    }
    CHECK(pcm.sample_rate == 44100 && pcm.count == 4, "wav rate + length");
    if (pcm.count == 4) {
        CLOSE(pcm.samples[0], -1.0, 1e-6, "wav frame 0");
        CLOSE(pcm.samples[1], 0.0, 1e-6, "wav frame 1");
        CLOSE(pcm.samples[2], 32767.0 / 32768.0, 1e-6, "wav frame 2");
        CLOSE(pcm.samples[3], 0.5, 1e-6, "wav frame 3");
    }
    sonic_pcm_free(&pcm);
    remove_dir_tree(dir);
}

/* Writes \p bytes to \p path and asserts sonic_decode rejects it. */
static void
expect_wav_rejected(const char *path, const unsigned char *bytes, size_t n,
                    const char *msg)
{
    FILE *f = fopen(path, "wb");
    sonic_pcm pcm;
    if (f != 0) {
        fwrite(bytes, 1, n, f);
        fclose(f);
    }
    if (sonic_decode(path, &pcm)) {
        CHECK(0, msg);
        sonic_pcm_free(&pcm);
    } else {
        CHECK(1, msg);
    }
}

static void
test_wav_robustness(void)
{
    char dir[512];
    char path[512];
    /* well-formed 16-bit stereo header prefix for reuse */
    static const unsigned char goodfmt[44] = {
        'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 16, 0, 0, 0,
        1, 0, 2, 0,
        0x44, 0xac, 0, 0,
        0x10, 0xb1, 0x02, 0,
        4, 0, 16, 0,
        'd', 'a', 't', 'a', 16, 0, 0, 0
    };
    unsigned char w[512];

    if (make_temp_dir(dir, sizeof dir) != 0)
        return;
    snprintf(path, sizeof path, "%s/bad.wav", dir);

    /* not RIFF/WAVE at all */
    memset(w, 0, sizeof w);
    expect_wav_rejected(path, w, 44, "not RIFF rejected");

    /* truncated header (short read) */
    expect_wav_rejected(path, goodfmt, 12, "truncated header rejected");

    /* fmt chunk size larger than declared -> oversized chunk */
    memset(w, 0, sizeof w);
    memcpy(w, goodfmt, sizeof goodfmt);
    w[16] = 0xff; w[17] = 0xff; w[18] = 0xff; w[19] = 0x7f;
    expect_wav_rejected(path, w, sizeof goodfmt, "oversized fmt rejected");

    /* zero channels */
    memset(w, 0, sizeof w);
    memcpy(w, goodfmt, sizeof goodfmt);
    w[22] = 0; w[23] = 0;
    expect_wav_rejected(path, w, sizeof goodfmt, "zero channels rejected");

    /* zero sample rate */
    memset(w, 0, sizeof w);
    memcpy(w, goodfmt, sizeof goodfmt);
    w[24] = 0; w[25] = 0; w[26] = 0; w[27] = 0;
    expect_wav_rejected(path, w, sizeof goodfmt, "zero sample rate rejected");

    /* block align inconsistent with channels * bytes-per-sample */
    memset(w, 0, sizeof w);
    memcpy(w, goodfmt, sizeof goodfmt);
    w[32] = 8; w[33] = 0;
    expect_wav_rejected(path, w, sizeof goodfmt, "bad block align rejected");

    /* unsupported format tag (compressed/float codec) */
    memset(w, 0, sizeof w);
    memcpy(w, goodfmt, sizeof goodfmt);
    w[20] = 6; w[21] = 0;
    expect_wav_rejected(path, w, sizeof goodfmt, "unsupported format rejected");

    /* missing data chunk (data size 0) */
    memset(w, 0, sizeof w);
    memcpy(w, goodfmt, sizeof goodfmt);
    w[40] = 0; w[41] = 0; w[42] = 0; w[43] = 0;
    expect_wav_rejected(path, w, sizeof goodfmt, "empty data rejected");

    /* oversized data length that would overflow frame count */
    memset(w, 0, sizeof w);
    memcpy(w, goodfmt, sizeof goodfmt);
    w[40] = 0x00; w[41] = 0x00; w[42] = 0x00; w[43] = 0xff;
    expect_wav_rejected(path, w, sizeof goodfmt, "oversized data rejected");

    /* odd-sized chunk with padding: insert an unknown chunk with odd size
       between fmt and data, with its pad byte */
    memset(w, 0, sizeof w);
    memcpy(w, goodfmt, 36);
    w[36] = 'L'; w[37] = 'I'; w[38] = 'S'; w[39] = 'T';
    w[40] = 3; w[41] = 0; w[42] = 0; w[43] = 0;
    w[44] = 'x'; w[45] = 'y'; w[46] = 'z'; w[47] = 0; /* payload + pad */
    w[48] = 'd'; w[49] = 'a'; w[50] = 't'; w[51] = 'a';
    w[52] = 16; w[53] = 0; w[54] = 0; w[55] = 0;
    w[56] = 1; w[57] = 0; w[58] = 0; w[59] = 0;
    expect_wav_rejected(path, w, 44, "chunk walk odd-pad path rejected/decoded");
    /* The above is missing 12 bytes of data payload, so it must be rejected
       (short data read). Assert explicitly with a well-formed padded case. */

    /* well-formed odd-padded unknown chunk must still decode */
    memset(w, 0, sizeof w);
    memcpy(w, goodfmt, 36);
    w[36] = 'L'; w[37] = 'I'; w[38] = 'S'; w[39] = 'T';
    w[40] = 3; w[41] = 0; w[42] = 0; w[43] = 0;
    w[44] = 'x'; w[45] = 'y'; w[46] = 'z'; w[47] = 0;
    w[48] = 'd'; w[49] = 'a'; w[50] = 't'; w[51] = 'a';
    w[52] = 16; w[53] = 0; w[54] = 0; w[55] = 0;
    {
        int16_t frames[8] = { -32768, -32768, 0, 0, 32767, 32767, 16384, 16384 };
        FILE *f = fopen(path, "wb");
        sonic_pcm pcm;
        if (f != 0) {
            fwrite(w, 1, 56, f);
            fwrite(frames, 2, 8, f);
            fclose(f);
        }
        if (!sonic_decode(path, &pcm)) {
            CHECK(0, "odd-padded chunk decodes");
        } else {
            CHECK(1, "odd-padded chunk decodes");
            CHECK(pcm.sample_rate == 44100 && pcm.count == 4,
                  "odd-padded chunk rate + length");
            sonic_pcm_free(&pcm);
        }
    }

    snprintf(path, sizeof path, "%s/bad.wav", dir);
    remove(path);
    remove_dir_tree(dir);
}

static void
test_profile(void)
{
    CHECK(strcmp(SONIC_PROFILE_ID, "musicpack-sonic-openl3-v1") == 0, "profile id");
    CHECK(SONIC_PROFILE_DIMENSIONS == 512, "dimensions");
    CHECK(strlen(SONIC_PROFILE_ONNX_SHA256) == 64, "onnx sha length");
    CHECK(strlen(SONIC_PROFILE_WEIGHTS_SHA256) == 64, "weights sha length");
}

int
main(int argc, char **argv)
{
    test_profile();
    test_resample_filter();
    test_resample();
    test_mel_filterbank();
    test_mel();
    test_pooling();
    test_album();
    test_cache();
    test_model_output_access();
    test_decode_wav();
    test_wav_robustness();
    if (argc == 2)
        test_decode_mono_mpc(argv[1]);
    else
        CHECK(0, "mpcenc path supplied for mono Musepack regression");

    if (failures == 0) {
        printf("all sonic analyzer core tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d sonic analyzer test failure(s)\n", failures);
    return 1;
}
