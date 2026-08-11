/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved. (BSD-2-Clause; see the top-level headers.)
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <musicpack/musicpack.h>
#include <musepack/musepack.h>

#include "decode.h"

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "vendor/dr_flac.h"

/* ------------------------------------------------------------------ */
/* Musepack                                                            */
/* ------------------------------------------------------------------ */

static int
decode_mpc(const char *path, sonic_pcm *out)
{
    mpc_reader reader;
    musepack_decoder *dec;
    musepack_stream_info info;
    float tmp[1152 * 2];
    float *buf = 0;
    size_t cap = 0, len = 0;
    uint64_t frames;
    unsigned ch;

    if (mpc_reader_init_stdio(&reader, path) != MPC_STATUS_OK)
        return 0;
    dec = musepack_decoder_open(&reader, 0);
    if (dec == 0) {
        mpc_reader_exit_stdio(&reader);
        return 0;
    }
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    musepack_decoder_get_stream_info(dec, &info);
    ch = info.channels > 2 ? 2 : info.channels;
    if (ch == 0 || info.sample_rate == 0) {
        musepack_decoder_close(dec);
        mpc_reader_exit_stdio(&reader);
        return 0;
    }
    while (musepack_decoder_read(dec, tmp, 1152, &frames) == MUSEPACK_OK && frames > 0) {
        size_t i;
        if (len + frames > cap) {
            size_t ncap = cap == 0 ? 65536 : cap * 2;
            while (ncap < len + frames)
                ncap *= 2;
            {
                float *nb = (float *) realloc(buf, ncap * sizeof(float));
                if (nb == 0) {
                    free(buf);
                    musepack_decoder_close(dec);
                    mpc_reader_exit_stdio(&reader);
                    return 0;
                }
                buf = nb;
                cap = ncap;
            }
        }
        for (i = 0; i < frames; i++) {
            double l = (double) tmp[i * 2];
            double r = ch > 1 ? (double) tmp[i * 2 + 1] : l;
            buf[len + i] = (float) ((l + r) * 0.5);
        }
        len += frames;
    }
    musepack_decoder_close(dec);
    mpc_reader_exit_stdio(&reader);
    if (len == 0) {
        free(buf);
        return 0;
    }
    out->samples = buf;
    out->count = len;
    out->sample_rate = (int) info.sample_rate;
    return 1;
}

/* ------------------------------------------------------------------ */
/* FLAC (dr_flac)                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    const drflac_uint8 *data;
    size_t size;
    size_t pos;
} flac_mem;

static size_t
flac_read(void *pUserData, void *pBufferOut, size_t bytesToRead)
{
    flac_mem *m = (flac_mem *) pUserData;
    size_t n = m->size - m->pos;
    if (n > bytesToRead)
        n = bytesToRead;
    memcpy(pBufferOut, m->data + m->pos, n);
    m->pos += n;
    return n;
}

static drflac_bool32
flac_seek(void *pUserData, int offset, drflac_seek_origin origin)
{
    flac_mem *m = (flac_mem *) pUserData;
    size_t base = origin == DRFLAC_SEEK_SET ? 0 : m->pos;
    size_t np = base + (size_t) offset;
    if (np > m->size)
        return DRFLAC_FALSE;
    m->pos = np;
    return DRFLAC_TRUE;
}

static drflac_bool32
flac_tell(void *pUserData, drflac_int64 *pCursor)
{
    flac_mem *m = (flac_mem *) pUserData;
    *pCursor = (drflac_int64) m->pos;
    return DRFLAC_TRUE;
}

static int
decode_flac(const char *path, sonic_pcm *out)
{
    FILE *f = fopen(path, "rb");
    long size;
    drflac_uint32 ch, rate;
    drflac_uint64 total;
    float *pcm;
    float *mono;
    drflac_uint64 i;
    flac_mem mem;

    if (f == 0)
        return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    mem.data = (const drflac_uint8 *) malloc((size_t) size);
    if (mem.data == 0) {
        fclose(f);
        return 0;
    }
    if (fread((void *) mem.data, 1, (size_t) size, f) != (size_t) size) {
        free((void *) mem.data);
        fclose(f);
        return 0;
    }
    fclose(f);
    mem.size = (size_t) size;
    mem.pos = 0;

    pcm = drflac_open_and_read_pcm_frames_f32(flac_read, flac_seek, flac_tell,
                                              &mem, &ch, &rate, &total, 0);
    if (pcm == 0) {
        free((void *) mem.data);
        return 0;
    }
    if (ch == 0 || rate == 0 || total == 0) {
        drflac_free(pcm, 0);
        free((void *) mem.data);
        return 0;
    }
    mono = (float *) malloc((size_t) total * sizeof(float));
    if (mono == 0) {
        drflac_free(pcm, 0);
        free((void *) mem.data);
        return 0;
    }
    for (i = 0; i < total; i++) {
        drflac_uint32 c;
        double acc = 0.0;
        for (c = 0; c < ch; c++)
            acc += (double) pcm[i * ch + c];
        mono[i] = (float) (acc / (double) ch);
    }
    drflac_free(pcm, 0);
    free((void *) mem.data);
    out->samples = mono;
    out->count = (size_t) total;
    out->sample_rate = (int) rate;
    return 1;
}

/* ------------------------------------------------------------------ */
/* WAV (minimal RIFF)                                                  */
/* ------------------------------------------------------------------ */

static int
decode_wav(const char *path, sonic_pcm *out)
{
    FILE *f = fopen(path, "rb");
    unsigned char hdr[44];
    uint32_t data_len = 0, sample_rate = 0, byte_rate = 0, block_align = 0;
    uint16_t fmt_tag = 0, ch = 0, bits = 0;
    float *buf;
    size_t len = 0;
    int rc = 0;

    if (f == 0)
        return 0;
    if (fread(hdr, 1, 44, f) != 44 || memcmp(hdr, "RIFF", 4) != 0 ||
        memcmp(hdr + 8, "WAVE", 4) != 0) {
        fclose(f);
        return 0;
    }
    fmt_tag = (uint16_t) (hdr[20] | (hdr[21] << 8));
    ch = (uint16_t) (hdr[22] | (hdr[23] << 8));
    sample_rate = (uint32_t) (hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24));
    byte_rate = (uint32_t) (hdr[28] | (hdr[29] << 8) | (hdr[30] << 16) | (hdr[31] << 24));
    block_align = (uint16_t) (hdr[32] | (hdr[33] << 8));
    bits = (uint16_t) (hdr[34] | (hdr[35] << 8));
    data_len = (uint32_t) (hdr[40] | (hdr[41] << 8) | (hdr[42] << 16) | (hdr[43] << 24));
    if (ch == 0 || sample_rate == 0 || bits == 0 || block_align == 0) {
        fclose(f);
        return 0;
    }
    buf = (float *) malloc(data_len / (ch * (bits / 8)) * sizeof(float) + 1);
    if (buf == 0) {
        fclose(f);
        return 0;
    }

    if (fmt_tag == 3) { /* IEEE float */
        size_t n = data_len / (ch * (bits / 8));
        size_t i;
        for (i = 0; i < n && fread(hdr, 1, ch * (bits / 8), f) == ch * (bits / 8); i++) {
            double acc = 0.0;
            uint16_t c;
            for (c = 0; c < ch; c++) {
                float v;
                memcpy(&v, hdr + c * (bits / 8), sizeof(float));
                acc += (double) v;
            }
            buf[len++] = (float) (acc / (double) ch);
        }
    } else if (fmt_tag == 1) { /* PCM */
        size_t i;
        size_t n = data_len / (ch * (bits / 8));
        for (i = 0; i < n && fread(hdr, 1, ch * (bits / 8), f) == ch * (bits / 8); i++) {
            double acc = 0.0;
            uint16_t c;
            for (c = 0; c < ch; c++) {
                double v;
                if (bits == 16) {
                    int16_t s;
                    memcpy(&s, hdr + c * 2, 2);
                    v = (double) s / 32768.0;
                } else if (bits == 24) {
                    int32_t s = (int8_t) hdr[c * 3] | (hdr[c * 3 + 1] << 8) |
                                (hdr[c * 3 + 2] << 16);
                    v = (double) s / 8388608.0;
                } else if (bits == 8) {
                    v = ((double) hdr[c] - 128.0) / 128.0;
                } else {
                    v = 0.0;
                }
                acc += v;
            }
            buf[len++] = (float) (acc / (double) ch);
        }
    }
    fclose(f);
    if (len == 0) {
        free(buf);
        return 0;
    }
    (void) byte_rate;
    (void) block_align;
    out->samples = buf;
    out->count = len;
    out->sample_rate = (int) sample_rate;
    (void) rc;
    return 1;
}

/* ------------------------------------------------------------------ */

int
sonic_decode(const char *path, sonic_pcm *out)
{
    const char *dot;
    if (path == 0 || out == 0)
        return 0;
    memset(out, 0, sizeof *out);
    dot = strrchr(path, '.');
    if (dot == 0)
        return 0;
    if (strcmp(dot, ".mpc") == 0)
        return decode_mpc(path, out);
    if (strcmp(dot, ".flac") == 0)
        return decode_flac(path, out);
    if (strcmp(dot, ".wav") == 0)
        return decode_wav(path, out);
    return 0;
}

void
sonic_pcm_free(sonic_pcm *pcm)
{
    if (pcm == 0)
        return;
    free(pcm->samples);
    memset(pcm, 0, sizeof *pcm);
}
