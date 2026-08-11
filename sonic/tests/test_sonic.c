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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache.h"
#include "decode.h"
#include "frontend.h"
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

static void
test_profile(void)
{
    CHECK(strcmp(SONIC_PROFILE_ID, "musicpack-sonic-openl3-v1") == 0, "profile id");
    CHECK(SONIC_PROFILE_DIMENSIONS == 512, "dimensions");
    CHECK(strlen(SONIC_PROFILE_ONNX_SHA256) == 64, "onnx sha length");
    CHECK(strlen(SONIC_PROFILE_WEIGHTS_SHA256) == 64, "weights sha length");
}

int
main(void)
{
    test_profile();
    test_resample_filter();
    test_resample();
    test_mel_filterbank();
    test_mel();
    test_pooling();
    test_album();
    test_cache();
    test_decode_wav();

    if (failures == 0) {
        printf("all sonic analyzer core tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d sonic analyzer test failure(s)\n", failures);
    return 1;
}
