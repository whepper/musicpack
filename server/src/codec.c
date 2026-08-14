/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
  (BSD 3-clause, see codec.h)
*/
#include "codec.h"
#include "mime.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
# include <sys/stat.h>
# include <io.h>
# include <fcntl.h>
#else
# include <sys/stat.h>
# include <fcntl.h>
# include <unistd.h>
#endif

#include <musepack/musepack.h>

static int
is_regular_path(const char *path)
{
#ifdef _WIN32
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    return (st.st_mode & _S_IFREG) != 0;
#else
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
    struct stat st;
    if (fd < 0)
        return 0;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink > 1) {
        close(fd);
        return 0;
    }
    close(fd);
    return 1;
#endif
}

static int
flac_probe(const char *path, mp_codec_info *out)
{
    unsigned char h[42];
    FILE *f;
    int rate = 0, ch = 0;

    if (!is_regular_path(path))
        return MUSICPACK_ERR_IO;
#ifdef _WIN32
    f = fopen(path, "rb");
    if (f == 0)
        return MUSICPACK_ERR_IO;
#else
    {
        int fd = open(path, O_RDONLY | O_NOFOLLOW);
        if (fd < 0)
            return MUSICPACK_ERR_IO;
        f = fdopen(fd, "rb");
        if (f == 0) { close(fd); return MUSICPACK_ERR_IO; }
    }
#endif
    if (fread(h, 1, sizeof h, f) != sizeof h ||
        memcmp(h, "fLaC", 4) != 0) {
        fclose(f);
        return MUSICPACK_ERR_INVALID;
    }
    if ((h[4] & 0x7f) != 0) {
        fclose(f);
        return MUSICPACK_ERR_INVALID;
    }
    {
        const unsigned char *si = h + 8;
        rate = ((unsigned int) si[10] << 12) | ((unsigned int) si[11] << 4) |
               ((unsigned int) si[12] >> 4);
        ch = (int) (((si[12] & 0x0e) >> 1) + 1);
    }
    fclose(f);
    snprintf(out->codec, sizeof out->codec, "flac");
    out->stream_version = 0;
    out->sample_rate = rate;
    out->channels = ch;
    return MUSICPACK_OK;
}

musicpack_status
mp_codec_probe(const char *abs_path, const char *rel_path, mp_codec_info *out)
{
    memset(out, 0, sizeof *out);
    if (!is_regular_path(abs_path))
        return MUSICPACK_ERR_IO;
    if (strcmp(mp_codec_for_path(rel_path), "musepack") == 0) {
        mpc_reader reader;
        musepack_decoder *dec;
        musepack_stream_info si;

        if (mpc_reader_init_stdio(&reader, abs_path) != MPC_STATUS_OK)
            return MUSICPACK_ERR_IO;
        dec = musepack_decoder_open(&reader, 0);
        if (dec == 0) {
            mpc_reader_exit_stdio(&reader);
            snprintf(out->codec, sizeof out->codec, "musepack");
            return MUSICPACK_OK;
        }
        memset(&si, 0, sizeof si);
        si.size = sizeof si;
        if (musepack_decoder_get_stream_info(dec, &si) == 0) {
            snprintf(out->codec, sizeof out->codec, "musepack-sv%d",
                     (int) si.stream_version);
            out->stream_version = (int) si.stream_version;
            out->sample_rate = (long long) si.sample_rate;
            out->channels = (long long) si.channels;
        } else {
            snprintf(out->codec, sizeof out->codec, "musepack");
        }
        musepack_decoder_close(dec);
        mpc_reader_exit_stdio(&reader);
        return MUSICPACK_OK;
    }
    if (strcmp(mp_codec_for_path(rel_path), "flac") == 0) {
        if (flac_probe(abs_path, out) == MUSICPACK_OK)
            return MUSICPACK_OK;
        snprintf(out->codec, sizeof out->codec, "flac");
        return MUSICPACK_OK;
    }
    snprintf(out->codec, sizeof out->codec, "%s", mp_codec_for_path(rel_path));
    return MUSICPACK_OK;
}
