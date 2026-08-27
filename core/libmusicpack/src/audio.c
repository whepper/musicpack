/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved. (BSD-3-Clause; see include/musicpack/audio.h for the
  full text.)
  SPDX-License-Identifier: BSD-3-Clause
*/
/// \file audio.c
/// Lossless-source PCM decoding for the authoring pipeline (see
/// <musicpack/audio.h>). FLAC decodes through vendored dr_flac, WAV through a
/// small native RIFF reader, and Musepack through libmusepack. All three
/// expose a single interleaved float or left-aligned 32-bit PCM stream.

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mpc/reader.h>
#include <musepack/musepack.h>

#include "fileio.h"

#include <musicpack/audio.h>

/* dr_flac's implementation is compiled TU-locally here (DRFLAC_API static)
   so it never collides with the analyzer's own copy in sonic/decode.c. */
#if defined(_MSC_VER)
# pragma warning(push)
# pragma warning(disable: 4505) /* unreferenced local function */
#elif defined(__GNUC__) || defined(__clang__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define DRFLAC_API static
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#if defined(_MSC_VER)
# pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
# pragma GCC diagnostic pop
#endif

#define MUSICPACK_AUDIO_MAX_CHANNELS 8

enum musicpack_audio_kind {
    AUDIO_FLAC,
    AUDIO_WAV,
    AUDIO_MPC
};

struct musicpack_audio {
    enum musicpack_audio_kind kind;
    unsigned rate;          /* sample rate in Hz */
    unsigned channels;      /* 1..MUSICPACK_AUDIO_MAX_CHANNELS */
    unsigned bits;          /* bits per sample (0 for Musepack) */
    uint64_t total;         /* total sample-frames; 0 when unknown */

    union {
        drflac *flac;
        struct {
            FILE *fp;
            unsigned bytes;     /* bytes per sample */
            int is_float;       /* WAVE_FORMAT_IEEE_FLOAT (32-bit) */
            uint64_t data_left; /* frames left per the data chunk length */
        } wav;
        struct {
            mpc_reader reader;
            musepack_decoder *dec;
        } mpc;
    } u;
};

/* ASCII case-insensitive extension match. */
static int
ext_is(const char *path, const char *ext)
{
    const char *dot = strrchr(path, '.');
    size_t i, n = strlen(ext);
    if (dot == 0)
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char) dot[i + 1];
        unsigned char e = (unsigned char) ext[i];
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char) (c - 'A' + 'a');
        if (c != e)
            return 0;
    }
    return dot[n + 1] == '\0';
}

/* ------------------------------------------------------------------ */
/* FLAC (dr_flac)                                                      */
/* ------------------------------------------------------------------ */

static musicpack_audio *
audio_open_flac(const char *path, musicpack_status *status)
{
    musicpack_audio *a;
    drflac *d;

    errno = 0;
    d = drflac_open_file(path, 0);
    if (d == 0) {
        if (status != 0)
            *status = errno == ENOENT ? MUSICPACK_ERR_IO : MUSICPACK_ERR_INVALID;
        return 0;
    }
    if (d->channels < 1 || d->channels > MUSICPACK_AUDIO_MAX_CHANNELS ||
        d->sampleRate == 0 || d->bitsPerSample == 0) {
        drflac_close(d);
        if (status != 0)
            *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    a = (musicpack_audio *) calloc(1, sizeof *a);
    if (a == 0) {
        drflac_close(d);
        if (status != 0)
            *status = MUSICPACK_ERR_NOMEM;
        return 0;
    }
    a->kind = AUDIO_FLAC;
    a->rate = d->sampleRate;
    a->channels = d->channels;
    a->bits = d->bitsPerSample;
    a->total = d->totalPCMFrameCount;
    a->u.flac = d;
    if (status != 0)
        *status = MUSICPACK_OK;
    return a;
}

/* ------------------------------------------------------------------ */
/* WAV (native RIFF reader)                                            */
/* ------------------------------------------------------------------ */

static uint32_t
rd32le(const unsigned char *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
           ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint16_t
rd16le(const unsigned char *p)
{
    return (uint16_t) ((uint16_t) p[0] | ((uint16_t) p[1] << 8));
}

/* Returns 1 when the extensible sub-format GUID (16 bytes at p) is the
   standard PCM or IEEE-float GUID; *fmt_tag receives 1 or 3. */
static int
extensible_subformat(const unsigned char *p, unsigned *fmt_tag)
{
    static const unsigned char tail[12] =
        { 0x00, 0x00, 0x00, 0x00, 0x10, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b };
    unsigned tag = rd16le(p);
    if (tag != 1 && tag != 3)
        return 0;
    if (memcmp(p + 4, tail, sizeof tail) != 0)
        return 0;
    *fmt_tag = tag;
    return 1;
}

static musicpack_audio *
audio_open_wav(const char *path, musicpack_status *status)
{
    FILE *f;
    unsigned char hdr[12];
    unsigned fmt_tag = 0, bits = 0, ch = 0, bytes = 0;
    uint32_t sample_rate = 0, block_align = 0;
    uint64_t data_len = 0;
    int have_fmt = 0, have_data = 0, is_float = 0;
    musicpack_audio *a = 0;

    f = fopen(path, "rb");
    if (f == 0) {
        if (status != 0)
            *status = MUSICPACK_ERR_IO;
        return 0;
    }
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr ||
        memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fclose(f);
        if (status != 0)
            *status = MUSICPACK_ERR_INVALID;
        return 0;
    }

    while (!have_fmt || !have_data) {
        unsigned char chdr[8];
        char id[4];
        uint64_t csz;
        if (fread(chdr, 1, sizeof chdr, f) != sizeof chdr) {
            fclose(f);
            if (status != 0)
                *status = MUSICPACK_ERR_INVALID;
            return 0;
        }
        memcpy(id, chdr, 4);
        csz = rd32le(chdr + 4);
        if (memcmp(id, "fmt ", 4) == 0) {
            unsigned char fmt[40];
            size_t rd = csz < sizeof fmt ? (size_t) csz : sizeof fmt;
            if (rd < 16 || fread(fmt, 1, rd, f) != rd) {
                fclose(f);
                if (status != 0)
                    *status = MUSICPACK_ERR_INVALID;
                return 0;
            }
            if (csz > rd) {
                char junk[256];
                uint64_t skip = csz - rd;
                while (skip > 0) {
                    size_t n = skip > sizeof junk ? sizeof junk : (size_t) skip;
                    if (fread(junk, 1, n, f) != n) {
                        fclose(f);
                        if (status != 0)
                            *status = MUSICPACK_ERR_INVALID;
                        return 0;
                    }
                    skip -= n;
                }
            }
            fmt_tag = rd16le(fmt);
            ch = rd16le(fmt + 2);
            sample_rate = rd32le(fmt + 4);
            block_align = rd16le(fmt + 12);
            bits = rd16le(fmt + 14);
            if (fmt_tag == 0xFFFE) {
                /* WAVE_FORMAT_EXTENSIBLE: the real format is the sub-format
                   GUID at offset 24. Only standard PCM/float GUIDs pass. */
                if (rd < 40 || !extensible_subformat(fmt + 24, &fmt_tag)) {
                    fclose(f);
                    if (status != 0)
                        *status = MUSICPACK_ERR_INVALID;
                    return 0;
                }
            }
            have_fmt = 1;
        } else if (memcmp(id, "data", 4) == 0) {
            data_len = csz;
            have_data = 1;
            if (!have_fmt) {
                /* data before fmt : unsupported by the reader */
                fclose(f);
                if (status != 0)
                    *status = MUSICPACK_ERR_INVALID;
                return 0;
            }
        } else {
            char junk[256];
            uint64_t skip = csz + (csz & 1);
            while (skip > 0) {
                size_t n = skip > sizeof junk ? sizeof junk : (size_t) skip;
                if (fread(junk, 1, n, f) != n) {
                    fclose(f);
                    if (status != 0)
                        *status = MUSICPACK_ERR_INVALID;
                    return 0;
                }
                skip -= n;
            }
        }
    }

    if (!have_fmt || !have_data || ch == 0 || ch > MUSICPACK_AUDIO_MAX_CHANNELS ||
        sample_rate == 0) {
        fclose(f);
        if (status != 0)
            *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    if (fmt_tag == 1) {
        if (bits != 8 && bits != 16 && bits != 24 && bits != 32) {
            fclose(f);
            if (status != 0)
                *status = MUSICPACK_ERR_INVALID;
            return 0;
        }
        is_float = 0;
    } else if (fmt_tag == 3) {
        if (bits != 32) {
            fclose(f);
            if (status != 0)
                *status = MUSICPACK_ERR_INVALID;
            return 0;
        }
        is_float = 1;
    } else {
        /* ADPCM, A-law, compressed containers, ... are rejected explicitly. */
        fclose(f);
        if (status != 0)
            *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    bytes = bits / 8;
    if (bytes == 0 || block_align != (uint16_t) (ch * bytes)) {
        fclose(f);
        if (status != 0)
            *status = MUSICPACK_ERR_INVALID;
        return 0;
    }

    a = (musicpack_audio *) calloc(1, sizeof *a);
    if (a == 0) {
        fclose(f);
        if (status != 0)
            *status = MUSICPACK_ERR_NOMEM;
        return 0;
    }
    a->kind = AUDIO_WAV;
    a->rate = sample_rate;
    a->channels = ch;
    a->bits = bits;
    a->total = data_len / (uint64_t) (ch * bytes);
    a->u.wav.fp = f;
    a->u.wav.bytes = bytes;
    a->u.wav.is_float = is_float;
    a->u.wav.data_left = a->total;
    if (status != 0)
        *status = MUSICPACK_OK;
    return a;
}

/* ------------------------------------------------------------------ */
/* Musepack (libmusepack)                                              */
/* ------------------------------------------------------------------ */

static musicpack_audio *
audio_open_mpc(const char *path, musicpack_status *status)
{
    musicpack_audio *a;
    musepack_stream_info info;

    a = (musicpack_audio *) calloc(1, sizeof *a);
    if (a == 0) {
        if (status != 0)
            *status = MUSICPACK_ERR_NOMEM;
        return 0;
    }
    if (mpc_reader_init_stdio(&a->u.mpc.reader, path) != MPC_STATUS_OK) {
        free(a);
        if (status != 0)
            *status = MUSICPACK_ERR_IO;
        return 0;
    }
    a->u.mpc.dec = musepack_decoder_open(&a->u.mpc.reader, 0);
    if (a->u.mpc.dec == 0) {
        mpc_reader_exit_stdio(&a->u.mpc.reader);
        free(a);
        if (status != 0)
            *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    musepack_decoder_get_stream_info(a->u.mpc.dec, &info);
    if (info.channels < 1 || info.channels > 2 || info.sample_rate == 0) {
        musepack_decoder_close(a->u.mpc.dec);
        mpc_reader_exit_stdio(&a->u.mpc.reader);
        free(a);
        if (status != 0)
            *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    a->kind = AUDIO_MPC;
    a->rate = info.sample_rate;
    a->channels = info.channels;
    a->bits = 0;
    a->total = info.length_samples;
    if (status != 0)
        *status = MUSICPACK_OK;
    return a;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

musicpack_audio *
musicpack_audio_open(const char *path, musicpack_status *status)
{
    if (path == 0 || *path == '\0') {
        if (status != 0)
            *status = MUSICPACK_ERR_INVALID;
        return 0;
    }
    if (ext_is(path, "flac"))
        return audio_open_flac(path, status);
    if (ext_is(path, "wav"))
        return audio_open_wav(path, status);
    if (ext_is(path, "mpc"))
        return audio_open_mpc(path, status);
    if (status != 0)
        *status = MUSICPACK_ERR_INVALID;
    return 0;
}

musicpack_status
musicpack_audio_get_format(const musicpack_audio *a, musicpack_audio_format *fmt)
{
    if (a == 0 || fmt == 0)
        return MUSICPACK_ERR_INVALID;
    memset(fmt, 0, sizeof *fmt);
    fmt->sample_rate = a->rate;
    fmt->channels = a->channels;
    fmt->bits_per_sample = a->bits;
    fmt->total_samples = a->total;
    fmt->is_float = a->kind == AUDIO_WAV && a->u.wav.is_float;
    snprintf(fmt->codec, sizeof fmt->codec, "%s",
             a->kind == AUDIO_FLAC ? "flac"
             : a->kind == AUDIO_WAV ? "wav" : "musepack");
    return MUSICPACK_OK;
}

static musicpack_status
wav_read_frames(musicpack_audio *a, void *out, size_t frames, size_t *read,
                int as_f32)
{
    unsigned ch = a->channels;
    unsigned bytes = a->u.wav.bytes;
    uint64_t want = frames;
    unsigned char *raw;
    size_t got_bytes, got_frames, i;

    if (want > a->u.wav.data_left)
        want = a->u.wav.data_left;
    if (want == 0) {
        *read = 0;
        return MUSICPACK_OK;
    }
    raw = (unsigned char *) malloc((size_t) want * ch * bytes);
    if (raw == 0)
        return MUSICPACK_ERR_NOMEM;
    got_bytes = fread(raw, 1, (size_t) want * ch * bytes, a->u.wav.fp);
    got_frames = got_bytes / (ch * bytes);
    a->u.wav.data_left -= got_frames;

    if (as_f32) {
        float *outf = (float *) out;
        for (i = 0; i < got_frames * ch; i++) {
            const unsigned char *p = raw + i * bytes;
            if (a->u.wav.is_float) {
                memcpy(&outf[i], p, 4);
            } else if (bytes == 1) {
                outf[i] = ((float) (p[0] - 128)) * (1.0f / 128.0f);
            } else if (bytes == 2) {
                outf[i] = (float) (int16_t) rd16le(p) * (1.0f / 32768.0f);
            } else if (bytes == 3) {
                uint32_t u = (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
                             ((uint32_t) p[2] << 16);
                int32_t v = (u & 0x800000u) ? (int32_t) (u | 0xFF000000u)
                                            : (int32_t) u;
                outf[i] = (float) v * (1.0f / 8388608.0f);
            } else {
                outf[i] = (float) (int32_t) rd32le(p) * (1.0f / 2147483648.0f);
            }
        }
    } else {
        int32_t *outi = (int32_t *) out;
        for (i = 0; i < got_frames * ch; i++) {
            const unsigned char *p = raw + i * bytes;
            if (bytes == 1) {
                outi[i] = ((int32_t) (p[0] - 128)) << 24;
            } else if (bytes == 2) {
                outi[i] = (int32_t) (int16_t) rd16le(p) << 16;
            } else if (bytes == 3) {
                uint32_t u = (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
                             ((uint32_t) p[2] << 16);
                int32_t v = (u & 0x800000u) ? (int32_t) (u | 0xFF000000u)
                                            : (int32_t) u;
                outi[i] = v << 8;
            } else {
                outi[i] = (int32_t) rd32le(p);
            }
        }
    }
    free(raw);
    *read = got_frames;
    return MUSICPACK_OK;
}

musicpack_status
musicpack_audio_read_frames_f32(musicpack_audio *a, float *interleaved,
                                size_t frames, size_t *read)
{
    if (a == 0 || interleaved == 0 || frames == 0 || read == 0)
        return MUSICPACK_ERR_INVALID;
    switch (a->kind) {
    case AUDIO_FLAC: {
        drflac_uint64 got = drflac_read_pcm_frames_f32(a->u.flac, frames,
                                                       interleaved);
        *read = (size_t) got;
        return MUSICPACK_OK;
    }
    case AUDIO_WAV:
        return wav_read_frames(a, interleaved, frames, read, 1);
    case AUDIO_MPC: {
        uint64_t got;
        musepack_error err = musepack_decoder_read(a->u.mpc.dec, interleaved,
                                                   frames, &got);
        *read = (size_t) got;
        if (err == MUSEPACK_OK || err == MUSEPACK_ERR_EOF)
            return MUSICPACK_OK;
        return err == MUSEPACK_ERR_INVALID ? MUSICPACK_ERR_INVALID
                                           : MUSICPACK_ERR_IO;
    }
    }
    return MUSICPACK_ERR_INVALID;
}

musicpack_status
musicpack_audio_read_frames_s32(musicpack_audio *a, int32_t *interleaved,
                                size_t frames, size_t *read)
{
    if (a == 0 || interleaved == 0 || frames == 0 || read == 0)
        return MUSICPACK_ERR_INVALID;
    switch (a->kind) {
    case AUDIO_FLAC: {
        drflac_uint64 got = drflac_read_pcm_frames_s32(a->u.flac, frames,
                                                       interleaved);
        *read = (size_t) got;
        return MUSICPACK_OK;
    }
    case AUDIO_WAV:
        /* IEEE-float WAV has no integer representation; s32 reads are never
           valid for it, regardless of stream position. */
        if (a->u.wav.is_float) {
            *read = 0;
            return MUSICPACK_ERR_INVALID;
        }
        return wav_read_frames(a, interleaved, frames, read, 0);
    case AUDIO_MPC:
        /* Musepack decodes to float only; it is never re-encoded. */
        *read = 0;
        return MUSICPACK_ERR_INVALID;
    }
    return MUSICPACK_ERR_INVALID;
}

void
musicpack_audio_close(musicpack_audio *a)
{
    if (a == 0)
        return;
    switch (a->kind) {
    case AUDIO_FLAC:
        drflac_close(a->u.flac);
        break;
    case AUDIO_WAV:
        fclose(a->u.wav.fp);
        break;
    case AUDIO_MPC:
        musepack_decoder_close(a->u.mpc.dec);
        mpc_reader_exit_stdio(&a->u.mpc.reader);
        break;
    }
    free(a);
}
