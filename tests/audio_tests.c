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

/* Native source-decoder tests: FLAC (16/24-bit, 44.1/48/96 kHz, mono),
   WAV (PCM/float/extensible/ADPCM-rejection/truncation) and the Musepack
   handoff, all through the libmusicpack `musicpack_audio_*` abstraction.
   Fixtures live in tests/fixtures/audio/ and are committed.

   Usage: audio_tests <audio-fixtures-dir> <mpc-fixture>
   Wired into CTest as the "audio_decode" suite. */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
# include <direct.h>
# include <windows.h>
# define mkdir_one(p) _mkdir(p)
#else
# include <unistd.h>
# define mkdir_one(p) mkdir(p, 0755)
#endif

#include <musicpack/musicpack.h>

static int failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);    \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/* Reads the whole stream via read_frames_f32, asserting EOF ends cleanly.
   Returns the number of frames decoded, or (size_t)-1 on error. */
static size_t
read_all_f32(const char *path, musicpack_audio_format *fmt_out)
{
    musicpack_audio *a = musicpack_audio_open(path, 0);
    musicpack_audio_format fmt;
    float pcm[1152 * 8];
    size_t n, total = 0;
    if (a == 0)
        return (size_t) -1;
    if (musicpack_audio_get_format(a, &fmt) != MUSICPACK_OK) {
        musicpack_audio_close(a);
        return (size_t) -1;
    }
    while (musicpack_audio_read_frames_f32(a, pcm, 1152, &n) == MUSICPACK_OK
           && n > 0)
        total += n;
    musicpack_audio_close(a);
    if (fmt_out != 0)
        *fmt_out = fmt;
    return total;
}

static void
test_flac_16_44k(const char *dir)
{
    char path[512];
    musicpack_audio *a;
    musicpack_audio_format f;
    musicpack_status st;

    snprintf(path, sizeof path, "%s/flac16-44k.flac", dir);
    a = musicpack_audio_open(path, &st);
    CHECK(a != 0, "16-bit FLAC opens");
    if (a == 0)
        return;
    CHECK(musicpack_audio_get_format(a, &f) == MUSICPACK_OK, "format read");
    CHECK(strcmp(f.codec, "flac") == 0, "codec is flac");
    CHECK(f.sample_rate == 44100, "44.1 kHz");
    CHECK(f.channels == 2, "stereo");
    CHECK(f.bits_per_sample == 16, "16-bit");
    CHECK(f.total_samples == 88200, "2 s at 44.1 kHz");
    CHECK(f.is_float == 0, "integer PCM");
    {
        float pcm[1152 * 2];
        size_t n, got = 0;
        while (musicpack_audio_read_frames_f32(a, pcm, 1152, &n)
                   == MUSICPACK_OK && n > 0)
            got += n;
        CHECK(got == 88200, "full stream decodes to the declared count");
        CHECK(n == 0, "clean EOF reports zero frames");
    }
    musicpack_audio_close(a);
}

static void
test_flac_24bit_rates(const char *dir)
{
    char path[512];
    const char *name;
    unsigned rate;
    uint64_t total;
    int k;

    for (k = 0; k < 2; k++) {
        name = k == 0 ? "flac24-48k.flac" : "flac24-96k.flac";
        rate = k == 0 ? 48000u : 96000u;
        total = k == 0 ? 96000u : 192000u;
        snprintf(path, sizeof path, "%s/%s", dir, name);
        {
            musicpack_audio_format f;
            size_t frames = read_all_f32(path, &f);
            CHECK(frames != (size_t) -1, "24-bit FLAC decodes");
            if (frames != (size_t) -1) {
                CHECK(f.sample_rate == rate, "24-bit sample rate");
                CHECK(f.channels == 2, "24-bit stereo");
                CHECK(f.bits_per_sample == 24, "24-bit depth");
                CHECK(f.total_samples == total, "24-bit total samples");
                CHECK(frames == total, "24-bit stream fully consumed");
            }
        }
        /* 24-bit must be lossless through the left-aligned s32 path. */
        snprintf(path, sizeof path, "%s/%s", dir, name);
        {
            musicpack_audio *a = musicpack_audio_open(path, 0);
            int32_t s32[1152 * 2];
            float f32[1152 * 2];
            size_t n1, n2, i;
            int ok = 1;
            CHECK(a != 0, "24-bit FLAC opens");
            if (a == 0)
                continue;
            CHECK(musicpack_audio_read_frames_s32(a, s32, 1152, &n1)
                      == MUSICPACK_OK && n1 == 1152, "s32 read");
            musicpack_audio_close(a);
            a = musicpack_audio_open(path, 0);
            CHECK(musicpack_audio_read_frames_f32(a, f32, 1152, &n2)
                      == MUSICPACK_OK && n2 == 1152, "f32 read");
            musicpack_audio_close(a);
            for (i = 0; i < n1 * 2; i++) {
                double back = round((double) f32[i] * 8388608.0);
                int32_t want = (int32_t) back << 8;
                if (labs((long) (s32[i] - want)) > 1)
                    ok = 0;
            }
            CHECK(ok, "24-bit s32/f32 round-trip is exact");
        }
    }
}

static void
test_flac_mono(const char *dir)
{
    char path[512];
    musicpack_audio_format f;
    snprintf(path, sizeof path, "%s/flac-mono-44k.flac", dir);
    {
        size_t frames = read_all_f32(path, &f);
        CHECK(frames != (size_t) -1, "mono FLAC decodes");
        if (frames != (size_t) -1) {
            CHECK(f.channels == 1, "mono");
            CHECK(f.sample_rate == 44100, "mono 44.1 kHz");
            CHECK(f.bits_per_sample == 16, "mono 16-bit");
            CHECK(frames == 88200, "mono 2 s");
        }
    }
}

static void
test_wav_16_44k(const char *dir)
{
    char path[512];
    musicpack_audio_format f;
    snprintf(path, sizeof path, "%s/wav16-44k.wav", dir);
    {
        size_t frames = read_all_f32(path, &f);
        CHECK(frames != (size_t) -1, "16-bit PCM WAV decodes");
        if (frames != (size_t) -1) {
            CHECK(strcmp(f.codec, "wav") == 0, "codec is wav");
            CHECK(f.sample_rate == 44100, "WAV 44.1 kHz");
            CHECK(f.channels == 2, "WAV stereo");
            CHECK(f.bits_per_sample == 16, "WAV 16-bit");
            CHECK(f.total_samples == 88200, "WAV total samples");
            CHECK(frames == 88200, "WAV fully consumed");
            CHECK(f.is_float == 0, "WAV integer PCM");
        }
    }
}

static void
test_wav_24bit(const char *dir)
{
    char path[512];
    const char *names[2] = { "wav24-44k.wav", "wav24-ext.wav" };
    int k;
    for (k = 0; k < 2; k++) {
        musicpack_audio_format f;
        snprintf(path, sizeof path, "%s/%s", dir, names[k]);
        {
            size_t frames = read_all_f32(path, &f);
            CHECK(frames != (size_t) -1, "24-bit WAV decodes");
            if (frames != (size_t) -1) {
                CHECK(f.bits_per_sample == 24, "WAV 24-bit");
                CHECK(f.channels == 2 && f.sample_rate == 44100, "WAV 24-bit format");
                CHECK(frames == 88200, "WAV 24-bit fully consumed");
            }
        }
    }
}

static void
test_wav_float(const char *dir)
{
    char path[512];
    musicpack_audio *a;
    musicpack_audio_format f;
    float pcm[1152 * 2];
    size_t n, total = 0;

    snprintf(path, sizeof path, "%s/wav-float.wav", dir);
    a = musicpack_audio_open(path, 0);
    CHECK(a != 0, "float WAV opens");
    if (a == 0)
        return;
    CHECK(musicpack_audio_get_format(a, &f) == MUSICPACK_OK, "float format read");
    CHECK(f.is_float == 1, "float WAV flagged as float");
    CHECK(f.bits_per_sample == 32, "float WAV 32-bit");
    while (musicpack_audio_read_frames_f32(a, pcm, 1152, &n) == MUSICPACK_OK
           && n > 0)
        total += n;
    CHECK(total == 88200, "float WAV decodes fully");
    /* float WAV is never encodable: s32 reads are rejected */
    CHECK(musicpack_audio_read_frames_s32(a, (int32_t *) pcm, 1, &n)
              == MUSICPACK_ERR_INVALID, "s32 rejected for float WAV");
    musicpack_audio_close(a);
}

static void
test_wav_rejection(const char *dir)
{
    char path[512];
    musicpack_status st;

    /* ADPCM (format tag 2) must be rejected explicitly. */
    snprintf(path, sizeof path, "%s/wav-adpcm.wav", dir);
    st = MUSICPACK_OK;
    CHECK(musicpack_audio_open(path, &st) == 0, "ADPCM WAV rejected");
    CHECK(st == MUSICPACK_ERR_INVALID, "ADPCM rejection is INVALID");

    /* A non-WAV payload with the .wav extension must fail cleanly. */
    snprintf(path, sizeof path, "%s/garbage.wav", dir);
    {
        FILE *g = fopen(path, "wb");
        if (g != 0) {
            fputs("this is not a RIFF file at all", g);
            fclose(g);
            st = MUSICPACK_OK;
            CHECK(musicpack_audio_open(path, &st) == 0, "garbage WAV rejected");
            remove(path);
        }
    }

    /* A missing file is an I/O error. */
    snprintf(path, sizeof path, "%s/does-not-exist.wav", dir);
    st = MUSICPACK_OK;
    CHECK(musicpack_audio_open(path, &st) == 0, "missing WAV rejected");
    CHECK(st == MUSICPACK_ERR_IO, "missing file is IO");
}

static void
test_wav_truncated(const char *dir)
{
    char path[512];
    musicpack_audio_format f;
    snprintf(path, sizeof path, "%s/wav-truncated.wav", dir);
    {
        size_t frames = read_all_f32(path, &f);
        CHECK(frames != (size_t) -1, "truncated WAV decodes gracefully");
        if (frames != (size_t) -1) {
            CHECK(f.total_samples == 88200, "declared total is preserved");
            CHECK(frames < 88200 && frames > 0, "only the available frames decode");
        }
    }
}

static void
test_mpc_handoff(const char *mpc_path)
{
    musicpack_audio *a;
    musicpack_audio_format f;
    float pcm[1152 * 2];
    size_t n, total = 0;

    a = musicpack_audio_open(mpc_path, 0);
    CHECK(a != 0, "Musepack opens through the abstraction");
    if (a == 0)
        return;
    CHECK(musicpack_audio_get_format(a, &f) == MUSICPACK_OK, "MPC format read");
    CHECK(strcmp(f.codec, "musepack") == 0, "codec is musepack");
    CHECK(f.channels == 2, "MPC stereo");
    CHECK(f.bits_per_sample == 0, "MPC has no integer depth");
    while (musicpack_audio_read_frames_f32(a, pcm, 1152, &n) == MUSICPACK_OK
           && n > 0)
        total += n;
    CHECK(total > 0, "MPC stream decodes");
    CHECK(musicpack_audio_read_frames_s32(a, (int32_t *) pcm, 1, &n)
              == MUSICPACK_ERR_INVALID, "MPC s32 reads are unsupported");
    musicpack_audio_close(a);
}

/* Filenames with spaces, quotes and metacharacters must round-trip. */
static void
test_unusual_filename(const char *dir)
{
    char src[512], dst[512], tmp[512];
    FILE *in, *out;
    unsigned char buf[8192];
    size_t n;
    musicpack_audio_format f;

#if defined(_WIN32)
    if (snprintf(tmp, sizeof tmp, "%s\\audio_tmp_%lu", getenv("TEMP"),
                 (unsigned long) GetCurrentProcessId()) >= (int) sizeof tmp)
        return;
    mkdir_one(tmp);
#else
    if (snprintf(tmp, sizeof tmp, "/tmp/audio_tmp_XXXXXX") >= (int) sizeof tmp)
        return;
    if (mkdtemp(tmp) == 0)
        return;
#endif
    snprintf(src, sizeof src, "%s/flac16-44k.flac", dir);
    snprintf(dst, sizeof dst, "%s/track's $; [input].flac", tmp);
    in = fopen(src, "rb");
    out = fopen(dst, "wb");
    if (in != 0 && out != 0) {
        while ((n = fread(buf, 1, sizeof buf, in)) > 0)
            fwrite(buf, 1, n, out);
    }
    if (in != 0)
        fclose(in);
    if (out != 0)
        fclose(out);

    {
        size_t frames = read_all_f32(dst, &f);
        CHECK(frames == 88200, "unusual filename decodes");
        if (frames == 88200) {
            CHECK(f.sample_rate == 44100 && f.channels == 2, "unusual filename format");
        }
    }
    remove(dst);
#if defined(_WIN32)
    _rmdir(tmp);
#else
    remove(tmp);
#endif
}

int
main(int argc, char **argv)
{
    const char *dir, *mpc;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <audio-fixtures-dir> <mpc-fixture>\n", argv[0]);
        return 2;
    }
    dir = argv[1];
    mpc = argv[2];

    test_flac_16_44k(dir);
    test_flac_24bit_rates(dir);
    test_flac_mono(dir);
    test_wav_16_44k(dir);
    test_wav_24bit(dir);
    test_wav_float(dir);
    test_wav_rejection(dir);
    test_wav_truncated(dir);
    test_mpc_handoff(mpc);
    test_unusual_filename(dir);

    if (failures != 0)
        fprintf(stderr, "%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
