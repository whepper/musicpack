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
    if (info.channels < 1 || info.channels > 2) {
        musepack_decoder_close(dec);
        mpc_reader_exit_stdio(&reader);
        return 0;
    }
    ch = info.channels;
    if (info.sample_rate == 0) {
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
    unsigned char hdr[12];
    unsigned char chunkhdr[8];
    int have_fmt = 0, have_data = 0;
    uint16_t fmt_tag = 0, ch = 0, bits = 0, block_align = 0;
    uint32_t sample_rate = 0, byte_rate = 0, data_len = 0;
    long data_offset = -1;
    float *buf;
    size_t len = 0, frame_size, max_frames;

    if (f == 0)
        return 0;
    if (fread(hdr, 1, 12, f) != 12 ||
        memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fclose(f);
        return 0;
    }
    while (!have_fmt || !have_data) {
        char id[4];
        uint32_t csz;
        if (fread(chunkhdr, 1, 8, f) != 8) {
            fclose(f);
            return 0;
        }
        memcpy(id, chunkhdr, 4);
        csz = (uint32_t) chunkhdr[4] | ((uint32_t) chunkhdr[5] << 8) |
              ((uint32_t) chunkhdr[6] << 16) | ((uint32_t) chunkhdr[7] << 24);
        if (csz > 0xFFFFFFFEu) {
            fclose(f);
            return 0;
        }
        if (memcmp(id, "fmt ", 4) == 0) {
            unsigned char fmt[16];
            size_t rd = csz < 16 ? csz : 16;
            if (rd < 16 || fread(fmt, 1, rd, f) != rd) {
                fclose(f);
                return 0;
            }
            if (csz > rd)
                fseek(f, (long) (csz - rd), SEEK_CUR);
            if (csz & 1)
                fseek(f, 1, SEEK_CUR);
            fmt_tag = (uint16_t) fmt[0] | ((uint16_t) fmt[1] << 8);
            ch = (uint16_t) fmt[2] | ((uint16_t) fmt[3] << 8);
            sample_rate = (uint32_t) fmt[4] | ((uint32_t) fmt[5] << 8) |
                          ((uint32_t) fmt[6] << 16) | ((uint32_t) fmt[7] << 24);
            byte_rate = (uint32_t) fmt[8] | ((uint32_t) fmt[9] << 8) |
                        ((uint32_t) fmt[10] << 16) | ((uint32_t) fmt[11] << 24);
            block_align = (uint16_t) fmt[12] | ((uint16_t) fmt[13] << 8);
            bits = (uint16_t) fmt[14] | ((uint16_t) fmt[15] << 8);
            have_fmt = 1;
        } else if (memcmp(id, "data", 4) == 0) {
            data_len = csz;
            data_offset = ftell(f);
            if (data_offset < 0) {
                fclose(f);
                return 0;
            }
            have_data = 1;
            if (!have_fmt)
                fseek(f, (long) (csz + (csz & 1)), SEEK_CUR);
        } else {
            long skip = (long) csz + (long) (csz & 1);
            if (fseek(f, skip, SEEK_CUR) != 0) {
                fclose(f);
                return 0;
            }
        }
    }
    if (!have_fmt || !have_data) {
        fclose(f);
        return 0;
    }
    if (ch < 1 || ch > 2 || sample_rate == 0 || bits == 0 || block_align == 0) {
        fclose(f);
        return 0;
    }
    if (fmt_tag == 1) {
        if (bits != 8 && bits != 16 && bits != 24) {
            fclose(f);
            return 0;
        }
    } else if (fmt_tag == 3) {
        if (bits != 32) {
            fclose(f);
            return 0;
        }
    } else {
        fclose(f);
        return 0;
    }
    {
        size_t bytes_per_sample = bits / 8;
        if (bytes_per_sample == 0) {
            fclose(f);
            return 0;
        }
        frame_size = ch * bytes_per_sample;
    }
    if (frame_size == 0 || block_align != (uint16_t) frame_size) {
        fclose(f);
        return 0;
    }
    if (byte_rate != sample_rate * frame_size) {
        fclose(f);
        return 0;
    }
    if (data_len == 0 || data_len > 4000000000u) {
        fclose(f);
        return 0;
    }
    max_frames = data_len / frame_size;
    if (max_frames == 0 || max_frames > 100000000) {
        fclose(f);
        return 0;
    }
    if (fseek(f, data_offset, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    buf = (float *) malloc(max_frames * sizeof(float));
    if (buf == 0) {
        fclose(f);
        return 0;
    }

    if (fmt_tag == 3) {
        unsigned char *fbuf = (unsigned char *) malloc(frame_size);
        size_t i;
        if (fbuf == 0) {
            free(buf);
            fclose(f);
            return 0;
        }
        for (i = 0; i < max_frames && fread(fbuf, 1, frame_size, f) == frame_size; i++) {
            double acc = 0.0;
            uint16_t c;
            for (c = 0; c < ch; c++) {
                float v;
                memcpy(&v, fbuf + c * 4, sizeof(float));
                acc += (double) v;
            }
            buf[len++] = (float) (acc / (double) ch);
        }
        free(fbuf);
    } else {
        unsigned char *fbuf = (unsigned char *) malloc(frame_size);
        size_t i;
        if (fbuf == 0) {
            free(buf);
            fclose(f);
            return 0;
        }
        for (i = 0; i < max_frames && fread(fbuf, 1, frame_size, f) == frame_size; i++) {
            double acc = 0.0;
            uint16_t c;
            for (c = 0; c < ch; c++) {
                double v;
                if (bits == 16) {
                    int16_t s;
                    memcpy(&s, fbuf + c * 2, 2);
                    v = (double) s / 32768.0;
                } else if (bits == 24) {
                    uint32_t u = (uint32_t) fbuf[c * 3] |
                                 ((uint32_t) fbuf[c * 3 + 1] << 8) |
                                 ((uint32_t) fbuf[c * 3 + 2] << 16);
                    int32_t s;
                    if (u & 0x800000u)
                        s = (int32_t) (u | 0xFF000000u);
                    else
                        s = (int32_t) u;
                    v = (double) s / 8388608.0;
                } else {
                    v = ((double) fbuf[c] - 128.0) / 128.0;
                }
                acc += v;
            }
            buf[len++] = (float) (acc / (double) ch);
        }
        free(fbuf);
    }
    fclose(f);
    if (len == 0) {
        free(buf);
        return 0;
    }
    out->samples = buf;
    out->count = len;
    out->sample_rate = (int) sample_rate;
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
