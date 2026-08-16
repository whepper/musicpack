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
 * API tests for the stable libmusepack decoder interface.
 *
 * Exercises the public musepack_* API: lifecycle, invalid input, memory- and
 * file-backed readers, stream info, full decode, seeking (beginning/middle/
 * near end/repeated), end-of-stream and multiple independent instances.
 *
 * Usage: api_tests <fixture-a.mpc> <fixture-b.mpc>
 * Set MPC_TEST_SV7_FIXTURE to additionally decode a valid external SV7 file.
 * Wired into CTest as the "api" suite (see tests/CMakeLists.txt).
 */

#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <musepack/musepack.h>

static int failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);    \
            failures++;                                                      \
        }                                                                    \
    } while (0)

typedef struct test_reader_context {
    const unsigned char *data;
    mpc_seek_t size;
    mpc_seek_t pos;
    mpc_uint32_t max_read;
	unsigned int read_calls;
	unsigned int fail_read_call;
	unsigned int read_failures;
    unsigned int seek_calls;
    unsigned int fail_seek_call;
    unsigned int seek_failures;
    mpc_bool_t seekable;
} test_reader_context;

static mpc_int32_t test_read(mpc_reader *r, void *ptr, mpc_int32_t size)
{
    test_reader_context *c = r->data;
    mpc_seek_t avail, n;
	c->read_calls++;
	if (c->fail_read_call != 0 && c->read_calls == c->fail_read_call) {
		c->read_failures++;
		c->fail_read_call = 0;
		return MPC_STATUS_FAIL;
	}
    if (size < 0 || c->pos >= c->size) return 0;
    avail = c->size - c->pos;
    n = (mpc_seek_t) size < avail ? (mpc_seek_t) size : avail;
	if (c->max_read != 0 && n > c->max_read)
		n = c->max_read;
    memcpy(ptr, c->data + c->pos, (size_t) n);
    c->pos += n;
    return (mpc_int32_t) n;
}

static mpc_bool_t test_seek(mpc_reader *r, mpc_seek_t offset)
{
    test_reader_context *c = r->data;
	c->seek_calls++;
	if (!c->seekable || offset > c->size) return MPC_FALSE;
	if (c->fail_seek_call != 0 && c->seek_calls == c->fail_seek_call) {
		c->seek_failures++;
		c->fail_seek_call = 0;
		return MPC_FALSE;
	}
    c->pos = offset;
    return MPC_TRUE;
}

static mpc_seek_t test_tell(mpc_reader *r)
{
    test_reader_context *c = r->data;
    return c->pos;
}

static mpc_seek_t test_size(mpc_reader *r)
{
    test_reader_context *c = r->data;
    return c->seekable ? c->size : 0;
}

static mpc_bool_t test_canseek(mpc_reader *r)
{
    test_reader_context *c = r->data;
    return c->seekable;
}

static void init_test_reader(mpc_reader *r, test_reader_context *c,
                             const unsigned char *data, size_t size,
                             mpc_bool_t seekable)
{
    c->data = data;
    c->size = size;
    c->pos = 0;
    c->max_read = 0;
	c->read_calls = 0;
	c->fail_read_call = 0;
	c->read_failures = 0;
    c->seek_calls = 0;
    c->fail_seek_call = 0;
    c->seek_failures = 0;
    c->seekable = seekable;
    r->read = test_read;
    r->seek = test_seek;
    r->tell = test_tell;
    r->get_size = test_size;
    r->canseek = test_canseek;
    r->data = c;
}

static unsigned char *load_file(const char *path, size_t *size)
{
    FILE *f;
    long n;
    unsigned char *buf;

    f = fopen(path, "rb");
    if (f == 0) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    buf = malloc((size_t) n);
    if (buf == 0) { fclose(f); return 0; }
    if (fread(buf, 1, (size_t) n, f) != (size_t) n) {
        free(buf); fclose(f); return 0;
    }
    fclose(f);
    *size = (size_t) n;
    return buf;
}

typedef struct decode_result {
    uint64_t frames;
    uint64_t last_frames;
    musepack_error terminal;
} decode_result;

static decode_result decode_all(musepack_decoder *d)
{
    static float pcm[MUSEPACK_FRAME_MAX * 2];
    decode_result result = {0, 0, MUSEPACK_OK};

    for (;;) {
        result.terminal = musepack_decoder_read(d, pcm, MUSEPACK_FRAME_MAX,
                                                &result.last_frames);
        if (result.terminal != MUSEPACK_OK)
            return result;
        result.frames += result.last_frames;
    }
}

/* Deterministic PCM checksum: sum of raw float bits mod 2^32. */
static unsigned long pcm_checksum(musepack_decoder *d)
{
    static float pcm[MUSEPACK_FRAME_MAX * 2];
    unsigned long sum = 0;
    uint64_t frames;

    musepack_decoder_seek_sample(d, 0);
    while (musepack_decoder_read(d, pcm, MUSEPACK_FRAME_MAX, &frames) == MUSEPACK_OK) {
        uint64_t i;
        for (i = 0; i < frames; i++) {
            unsigned int bits;
            memcpy(&bits, &pcm[i], sizeof bits);
            sum += bits;
        }
    }
    return sum;
}

/* ------------------------------------------------------------------ */
/* create / destroy lifecycle                                          */
/* ------------------------------------------------------------------ */
static void test_lifecycle(const char *fixture)
{
    size_t sz;
    unsigned char *data = load_file(fixture, &sz);
    mpc_reader reader;
    musepack_error err = MUSEPACK_OK;
    musepack_decoder *d;
    int i;

    CHECK(data != 0, "load fixture A");
    if (data == 0) return;

    for (i = 0; i < 3; i++) {
        mpc_reader_init_memory(&reader, data, sz);
        d = musepack_decoder_open(&reader, &err);
        CHECK(d != 0, "open succeeds");
        CHECK(err == MUSEPACK_OK, "open error is OK");
        if (d != 0)
            musepack_decoder_close(d);
        mpc_reader_exit_memory(&reader);
    }
    free(data);
}

/* ------------------------------------------------------------------ */
/* invalid input                                                       */
/* ------------------------------------------------------------------ */
static void test_invalid_input(void)
{
    const char garbage[] = "this is definitely not a musepack file at all";
    const char truncated[] = "MPCK";
    mpc_reader reader;
    musepack_error err = MUSEPACK_OK;
    musepack_decoder *d;

    mpc_reader_init_memory(&reader, garbage, sizeof garbage - 1);
    d = musepack_decoder_open(&reader, &err);
    CHECK(d == 0, "garbage rejected");
    CHECK(err != MUSEPACK_OK, "garbage reports an error");
    mpc_reader_exit_memory(&reader);

    mpc_reader_init_memory(&reader, truncated, sizeof truncated - 1);
    d = musepack_decoder_open(&reader, &err);
    CHECK(d == 0, "truncated magic rejected");
    mpc_reader_exit_memory(&reader);

    err = MUSEPACK_OK;
    d = musepack_decoder_open(0, &err);
    CHECK(d == 0, "NULL reader rejected");
    CHECK(err == MUSEPACK_ERR_INVALID, "NULL reader error code");

    memset(&reader, 0, sizeof reader);
    d = musepack_decoder_open(&reader, &err);
    CHECK(d == 0 && err == MUSEPACK_ERR_INVALID, "missing callbacks rejected");
    CHECK(mpc_reader_init_memory(0, garbage, sizeof garbage) == MPC_STATUS_FAIL,
          "memory adapter rejects NULL reader");
    CHECK(mpc_reader_init_memory(&reader, 0, 1) == MPC_STATUS_FAIL,
          "memory adapter rejects NULL nonempty data");
    CHECK(mpc_reader_init_stdio_stream(0, stdin) == MPC_STATUS_FAIL,
          "stdio adapter rejects NULL reader");
}

static unsigned long test_crc32(const unsigned char *data, size_t size)
{
    unsigned long crc = 0xffffffffUL;
    size_t i;
    for (i = 0; i < size; i++) {
        unsigned int bit;
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320UL & (0UL - (crc & 1UL)));
    }
    return crc ^ 0xffffffffUL;
}

static int read_size_code(const unsigned char *data, size_t size, size_t *pos,
                          uint64_t *value, size_t *width)
{
    unsigned char byte;
    *value = 0;
    *width = 0;
    do {
        if (*pos >= size || *width >= 10) return 0;
        byte = data[(*pos)++];
        *value = (*value << 7) | (byte & 0x7f);
        (*width)++;
    } while (byte & 0x80);
    return 1;
}

static int find_sv8_block(const unsigned char *data, size_t size,
                          const char key[2], size_t *header,
                          size_t *payload, uint64_t *block_size)
{
    size_t pos = 4, width;
    if (size < 4 || memcmp(data, "MPCK", 4) != 0)
        return 0;
    while (pos + 3 <= size) {
        size_t start = pos;
        char key0 = (char) data[pos++], key1 = (char) data[pos++];
        if (!read_size_code(data, size, &pos, block_size, &width) ||
            *block_size < pos - start || *block_size > size - start)
            return 0;
        if (key0 == key[0] && key1 == key[1]) {
            *header = start;
            *payload = pos;
            return 1;
        }
        pos = start + (size_t) *block_size;
    }
    return 0;
}

static void write_test_bits(unsigned char *data, size_t *bit_pos,
                            uint32_t value, unsigned int bits)
{
    while (bits != 0) {
        bits--;
        data[*bit_pos >> 3] |= ((value >> bits) & 1u) << (7 - (*bit_pos & 7));
        (*bit_pos)++;
    }
}

static int set_sv8_samples(unsigned char *data, size_t size, uint64_t samples)
{
    size_t header, payload, pos, width, i;
    uint64_t block_size, old_samples;
    unsigned long crc;
    if (!find_sv8_block(data, size, "SH", &header, &payload, &block_size))
        return 0;
    pos = payload + 5;
    if (!read_size_code(data, size, &pos, &old_samples, &width) || width == 0 ||
        (width < 10 && samples >= (1ULL << (7 * width))))
        return 0;
    for (i = width; i != 0; i--) {
        data[payload + 5 + i - 1] = (unsigned char) (samples & 0x7f);
        if (i != width)
            data[payload + 5 + i - 1] |= 0x80;
        samples >>= 7;
    }
    crc = test_crc32(data + payload + 4,
                     (size_t) block_size - (payload - header) - 4);
    data[payload + 0] = (unsigned char) (crc >> 24);
    data[payload + 1] = (unsigned char) (crc >> 16);
    data[payload + 2] = (unsigned char) (crc >> 8);
    data[payload + 3] = (unsigned char) crc;
    return 1;
}

static void test_hostile_st(const char *fixture)
{
    size_t size, header, payload;
    unsigned char *source = load_file(fixture, &size);
    unsigned char *data;
    uint64_t block_size;
    int found;
    mpc_reader reader;
    musepack_decoder *d;
    musepack_error err;

    CHECK(source != 0, "load fixture for hostile ST");
    if (source == 0) return;
    found = find_sv8_block(source, size, "ST", &header, &payload, &block_size);
    CHECK(found, "find ST block");
    if (!found || payload - header != 3 || block_size < 4) {
        free(source);
        return;
    }

    data = malloc(size);
    CHECK(data != 0, "allocate zero-length ST mutation");
    if (data != 0) {
        memcpy(data, source, size);
        data[header + 2] = 3;
        mpc_reader_init_memory(&reader, data, size);
        d = musepack_decoder_open(&reader, &err);
        CHECK(d == 0 && err == MUSEPACK_ERR_INVALID,
              "zero-length ST payload rejected without overread");
        if (d != 0) musepack_decoder_close(d);
        mpc_reader_exit_memory(&reader);
        free(data);
    }

    data = malloc(size);
    CHECK(data != 0, "allocate truncated ST size-code mutation");
    if (data != 0) {
        memcpy(data, source, size);
        data[header + 2] = 4;
        data[payload] = 0x80;
        mpc_reader_init_memory(&reader, data, size);
        d = musepack_decoder_open(&reader, &err);
        CHECK(d == 0 && err == MUSEPACK_ERR_INVALID,
              "truncated ST size code rejected without overread");
        if (d != 0) musepack_decoder_close(d);
        mpc_reader_exit_memory(&reader);
        free(data);
    }

    data = malloc(size);
    CHECK(data != 0, "allocate truncated ST offset mutation");
    if (data != 0) {
        size_t bit_pos = 0;
        memcpy(data, source, size);
        data[header + 2] = 7;
        memset(data + payload, 0, 4);
        write_test_bits(data + payload, &bit_pos, 2, 8);
        write_test_bits(data + payload, &bit_pos, 0, 4);
        write_test_bits(data + payload, &bit_pos, 0, 8);
        write_test_bits(data + payload, &bit_pos, 0x80, 8);
        mpc_reader_init_memory(&reader, data, size);
        d = musepack_decoder_open(&reader, &err);
        CHECK(d == 0 && err == MUSEPACK_ERR_INVALID,
              "truncated ST second size code rejected without overread");
        if (d != 0) musepack_decoder_close(d);
        mpc_reader_exit_memory(&reader);
        free(data);
    }

    data = malloc(size);
    CHECK(data != 0, "allocate truncated ST Golomb mutation");
    if (data != 0) {
        size_t bit_pos = 0;
        memcpy(data, source, size);
        CHECK(set_sv8_samples(data, size, 100000),
              "extend sample accounting to require a Golomb entry");
        data[header + 2] = 7;
        memset(data + payload, 0, 4);
        write_test_bits(data + payload, &bit_pos, 3, 8);
        write_test_bits(data + payload, &bit_pos, 0, 4);
        write_test_bits(data + payload, &bit_pos, 0, 8);
        write_test_bits(data + payload, &bit_pos, 0, 8);
        mpc_reader_init_memory(&reader, data, size);
        d = musepack_decoder_open(&reader, &err);
        CHECK(d == 0 && err == MUSEPACK_ERR_INVALID,
              "unterminated ST Golomb code rejected without overread");
        if (d != 0) musepack_decoder_close(d);
        mpc_reader_exit_memory(&reader);
        free(data);
    }
    free(source);
}

static void test_hostile_accounting(const char *fixture)
{
    size_t size, pos = 4;
    unsigned char *data = load_file(fixture, &size);
    mpc_reader reader;
    musepack_decoder *d;
    musepack_error err;
    uint64_t block_size, samples, beg;
    size_t block_width, sample_width, beg_width, payload, sample_pos, beg_pos, i;

    CHECK(data != 0, "load fixture for hostile accounting");
    if (data == 0) return;
    CHECK(size > 8 && memcmp(data, "MPCK", 4) == 0, "hostile source is SV8");
    if (size <= 8 || memcmp(data, "MPCK", 4) != 0) { free(data); return; }
    while (pos + 3 < size) {
        size_t header = pos;
        char key0 = (char) data[pos++], key1 = (char) data[pos++];
        if (!read_size_code(data, size, &pos, &block_size, &block_width) ||
            block_size < pos - header || block_size > size - header)
            break;
        (void) block_width;
        payload = pos;
        if (key0 == 'S' && key1 == 'H') {
            unsigned long crc;
            pos = payload + 5;
            sample_pos = pos;
            CHECK(read_size_code(data, size, &pos, &samples, &sample_width),
                  "read SH sample count");
            beg_pos = pos;
            CHECK(read_size_code(data, size, &pos, &beg, &beg_width),
                  "read SH leading silence");
            CHECK(samples >= beg && sample_width != 0 && beg_width != 0,
                  "fixture has valid sample accounting");
            for (i = 0; i < sample_width; i++)
                data[sample_pos + i] = i + 1 == sample_width ? 0 : 0x80;
            for (i = 0; i < beg_width; i++)
                data[beg_pos + i] = i + 1 == beg_width ? 1 : 0x80;
            crc = test_crc32(data + payload + 4,
                             (size_t) block_size - (payload - header) - 4);
            data[payload + 0] = (unsigned char) (crc >> 24);
            data[payload + 1] = (unsigned char) (crc >> 16);
            data[payload + 2] = (unsigned char) (crc >> 8);
            data[payload + 3] = (unsigned char) crc;
            break;
        }
        pos = header + (size_t) block_size;
    }
    mpc_reader_init_memory(&reader, data, size);
    d = musepack_decoder_open(&reader, &err);
    CHECK(d == 0 && err == MUSEPACK_ERR_INVALID, "SV8 beg_silence > samples rejected");
    if (d != 0) musepack_decoder_close(d);
    mpc_reader_exit_memory(&reader);
    free(data);

    {
        unsigned char sv7[28] = {'M', 'P', '+', 7};
        unsigned char logical[24] = {0};
        size_t bit = 0;
        unsigned int fields[][2] = {
            {0, 16}, {0, 16}, {0, 1}, {0, 1}, {31, 6}, {10, 4}, {0, 2},
            {0, 2}, {0, 16}, {0, 16}, {0, 16}, {0, 16}, {0, 16}, {0, 1},
            {0, 11}, {0, 1}, {0, 19}, {0, 8}
        };
        size_t f;
        for (f = 0; f < sizeof fields / sizeof *fields; f++) {
            unsigned int n;
            for (n = fields[f][1]; n != 0; n--, bit++)
                logical[bit >> 3] |= ((fields[f][0] >> (n - 1)) & 1u) << (7 - (bit & 7));
        }
        for (i = 0; i < sizeof logical; i += 4) {
            sv7[4 + i] = logical[i + 3];
            sv7[5 + i] = logical[i + 2];
            sv7[6 + i] = logical[i + 1];
            sv7[7 + i] = logical[i];
        }
        mpc_reader_init_memory(&reader, sv7, sizeof sv7);
        d = musepack_decoder_open(&reader, &err);
        CHECK(d == 0 && err == MUSEPACK_ERR_INVALID, "SV7 zero-frame accounting rejected");
        if (d != 0) musepack_decoder_close(d);
        mpc_reader_exit_memory(&reader);
    }
}

/* ------------------------------------------------------------------ */
/* stream info                                                         */
/* ------------------------------------------------------------------ */
static void test_streaminfo(const char *fixture, unsigned int freq,
                            unsigned int channels, unsigned int version,
                            unsigned long samples)
{
    size_t sz;
    unsigned char *data = load_file(fixture, &sz);
    mpc_reader reader;
    mpc_streaminfo si;
    musepack_stream_info msi;
    musepack_decoder *d;
    musepack_error err;

    CHECK(data != 0, "load fixture for info");
    if (data == 0) return;
    mpc_reader_init_memory(&reader, data, sz);
    d = musepack_decoder_open(&reader, &err);
    CHECK(d != 0, "open for info");
    if (d == 0) { free(data); return; }

    CHECK(musepack_decoder_get_info(d, &si) == MUSEPACK_OK, "get_info ok");
    CHECK(si.sample_freq == freq, "sample_freq matches");
    CHECK(si.channels == channels, "channels match");
    CHECK(si.stream_version == version, "stream version matches");
    CHECK(musepack_decoder_length_samples(d) == samples, "length samples matches");

    /* versioned structure */
    memset(&msi, 0, sizeof msi);
    msi.size = sizeof msi;
    CHECK(musepack_decoder_get_stream_info(d, &msi) == MUSEPACK_OK,
          "get_stream_info ok");
    CHECK(msi.sample_rate == freq, "msi sample_rate matches");
    CHECK(msi.channels == channels, "msi channels match");
    CHECK(msi.stream_version == version, "msi stream version matches");
    CHECK(msi.length_samples == samples, "msi length matches");
    CHECK(msi.total_samples >= msi.length_samples + msi.beg_silence,
          "msi sample accounting");
    CHECK(msi.encoder[0] != '\0', "msi encoder populated");
    CHECK(msi.profile_name[0] != '\0', "msi profile_name populated");
    CHECK(MUSEPACK_STREAM_INFO_V1_END == 164u,
          "v1 ABI end offset remains frozen at 164 bytes");
    CHECK(MUSEPACK_STREAM_INFO_V1_END ==
              offsetof(musepack_stream_info, profile_name) + sizeof msi.profile_name,
          "v1 ABI floor is the last v1 field end offset");
    CHECK(sizeof(musepack_stream_info) >= MUSEPACK_STREAM_INFO_V1_END,
          "current stream-info layout contains v1 without requiring tail padding");

    /* the size field must meet the v1 floor; leading-field filling keeps
       older consumers working with a future, larger library */
    memset(&msi, 0, sizeof msi);
    msi.size = MUSEPACK_STREAM_INFO_MIN_SIZE;
    CHECK(musepack_decoder_get_stream_info(d, &msi) == MUSEPACK_OK,
          "get_stream_info at floor size ok");
    CHECK(msi.sample_rate == freq, "floor-size msi sample_rate matches");
    msi.size = MUSEPACK_STREAM_INFO_MIN_SIZE - 1;
    CHECK(musepack_decoder_get_stream_info(d, &msi) == MUSEPACK_ERR_INVALID,
          "get_stream_info rejects below-floor size");

    /* accessors */
    CHECK(musepack_decoder_sample_rate(d) == freq, "sample_rate accessor");
    CHECK(musepack_decoder_channels(d) == channels, "channels accessor");
    CHECK(musepack_decoder_stream_version(d) == version, "stream_version accessor");
    CHECK(strcmp(musepack_version(), MUSEPACK_VERSION) == 0, "version string");

    musepack_decoder_close(d);
    mpc_reader_exit_memory(&reader);
    free(data);
}

/* ------------------------------------------------------------------ */
/* full decode: memory- and file-backed, deterministic                 */
/* ------------------------------------------------------------------ */
static void test_full_decode(const char *fixture, unsigned long samples)
{
    size_t sz;
    unsigned char *data = load_file(fixture, &sz);
    mpc_reader reader;
    musepack_decoder *d;
    musepack_error err;
    decode_result decoded;
    unsigned long c1, c2;

    /* memory-backed */
    mpc_reader_init_memory(&reader, data, sz);
    d = musepack_decoder_open(&reader, &err);
    CHECK(d != 0, "open memory-backed");
    if (d != 0) {
        decoded = decode_all(d);
        CHECK(decoded.terminal == MUSEPACK_ERR_EOF, "memory decode reaches EOF");
        CHECK(decoded.last_frames == 0, "EOF reports zero frames");
        CHECK(decoded.frames == samples, "memory decode sample count");
        CHECK(decoded.frames == musepack_decoder_position(d), "position == decoded count");
        err = musepack_decoder_read(d, 0, 1, 0);
        CHECK(err == MUSEPACK_ERR_INVALID, "read after EOF rejects NULL pcm");
        musepack_decoder_close(d);
    }
    mpc_reader_exit_memory(&reader);

    /* file-backed */
    {
        mpc_reader freader;
        CHECK(mpc_reader_init_stdio(&freader, fixture) == MPC_STATUS_OK,
              "init stdio reader");
        d = musepack_decoder_open(&freader, &err);
        CHECK(d != 0, "open file-backed");
        if (d != 0) {
            decoded = decode_all(d);
            CHECK(decoded.terminal == MUSEPACK_ERR_EOF, "file decode reaches EOF");
            CHECK(decoded.frames == samples, "file decode sample count");
            musepack_decoder_close(d);
        }
        mpc_reader_exit_stdio(&freader);
    }

    /* determinism: two decodes of the same fixture hash identically */
    mpc_reader_init_memory(&reader, data, sz);
    d = musepack_decoder_open(&reader, &err);
    c1 = pcm_checksum(d);
    musepack_decoder_close(d);
    mpc_reader_exit_memory(&reader);
    mpc_reader_init_memory(&reader, data, sz);
    d = musepack_decoder_open(&reader, &err);
    c2 = pcm_checksum(d);
    musepack_decoder_close(d);
    mpc_reader_exit_memory(&reader);
    CHECK(c1 == c2, "decode is deterministic");

    free(data);
}

static void test_valid_sv7(const char *fixture)
{
    size_t size;
    unsigned char *data;
    mpc_reader reader;
    musepack_decoder *d;
    musepack_error err;
    decode_result decoded;
    uint64_t expected;

    if (fixture == 0 || fixture[0] == '\0')
        return;
    data = load_file(fixture, &size);
    CHECK(data != 0, "load optional valid SV7 fixture");
    if (data == 0) return;
    CHECK(mpc_reader_init_memory(&reader, data, size) == MPC_STATUS_OK,
          "initialize valid SV7 reader");
    d = musepack_decoder_open(&reader, &err);
    CHECK(d != 0 && err == MUSEPACK_OK, "open valid SV7 fixture");
    if (d != 0) {
        CHECK(musepack_decoder_stream_version(d) == 7, "fixture is SV7");
        expected = musepack_decoder_length_samples(d);
        decoded = decode_all(d);
        CHECK(decoded.terminal == MUSEPACK_ERR_EOF, "valid SV7 reaches normal EOF");
        CHECK(decoded.frames == expected, "valid SV7 decodes its advertised length");
        musepack_decoder_close(d);
    }
    mpc_reader_exit_memory(&reader);
    free(data);
}

/* ------------------------------------------------------------------ */
/* seeking                                                             */
/* ------------------------------------------------------------------ */
static void test_seeking(const char *fixture, unsigned long samples,
                         unsigned long freq)
{
    static const unsigned long targets[] = { 0, 4410, 22050, 22050, 44000, 44099 };
    size_t sz;
    unsigned char *data = load_file(fixture, &sz);
    mpc_reader reader;
    musepack_decoder *d;
    musepack_error err;
    unsigned int i;
    unsigned long expected_seconds;
	decode_result decoded;

    mpc_reader_init_memory(&reader, data, sz);
    d = musepack_decoder_open(&reader, &err);
    CHECK(d != 0, "open for seek");
    if (d == 0) { free(data); return; }

    /* seek to beginning */
    CHECK(musepack_decoder_seek_sample(d, 0) == MUSEPACK_OK, "seek 0 ok");
    CHECK(musepack_decoder_position(d) == 0, "position after seek 0");
    decoded = decode_all(d);
    CHECK(decoded.terminal == MUSEPACK_ERR_EOF && decoded.frames == samples,
          "decode from 0 reaches EOF at full length");

    /* seek to middle, near end, repeated */
    for (i = 0; i < sizeof targets / sizeof *targets; i++) {
        unsigned long t = targets[i];
        if (t >= samples) t = samples;
        err = musepack_decoder_seek_sample(d, t);
        CHECK(err == MUSEPACK_OK, "seek ok");
        CHECK(musepack_decoder_position(d) == t, "position after seek");
        decoded = decode_all(d);
        CHECK(decoded.terminal == MUSEPACK_ERR_EOF,
              "decode after seek reaches EOF");
        CHECK(decoded.frames == samples - t, "decode after seek reaches the end");
        CHECK(musepack_decoder_position(d) == samples, "position at end after seek");
    }

    /* out-of-range seek clamps to the length */
    CHECK(musepack_decoder_seek_sample(d, samples + 10000) == MUSEPACK_OK,
          "oversized seek ok");
    CHECK(musepack_decoder_position(d) == samples, "oversized seek clamps");
    decoded = decode_all(d);
    CHECK(decoded.terminal == MUSEPACK_ERR_EOF && decoded.frames == 0,
          "decode at clamped end is empty EOF");

    /* seek by seconds */
    expected_seconds = (unsigned long) (0.1 * (double) freq + 0.5);
    CHECK(musepack_decoder_seek_seconds(d, 0.1) == MUSEPACK_OK, "seek seconds ok");
    CHECK(musepack_decoder_position(d) == expected_seconds, "seek seconds position");
    CHECK(musepack_decoder_seek_seconds(d, NAN) == MUSEPACK_ERR_INVALID,
          "NaN seek rejected");
    CHECK(musepack_decoder_seek_seconds(d, INFINITY) == MUSEPACK_ERR_INVALID,
          "infinite seek rejected");

    musepack_decoder_close(d);
    mpc_reader_exit_memory(&reader);
    free(data);
}

static void test_nonseekable(const char *fixture, unsigned long samples)
{
    size_t size;
    unsigned char *data = load_file(fixture, &size);
    unsigned char *tagged;
    test_reader_context context;
    mpc_reader reader;
    musepack_decoder *d;
    musepack_error err;
    float pcm[200 * 2];
    uint64_t frames = 0, next_frames = 0;
	decode_result decoded;

    CHECK(data != 0, "load fixture for nonseekable reader");
    if (data == 0) return;
    init_test_reader(&reader, &context, data, size, MPC_FALSE);
	context.max_read = 1;
    d = musepack_decoder_open(&reader, &err);
    CHECK(d != 0 && err == MUSEPACK_OK,
          "open one-byte fragmented unknown-size nonseekable reader");
    if (d != 0) {
        CHECK(musepack_decoder_read(d, pcm, 200, &frames) == MUSEPACK_OK && frames == 200,
              "read nonseekable reader");
        CHECK(musepack_decoder_seek_sample(d, 0) == MUSEPACK_ERR_SEEK,
              "nonseekable sample seek fails cleanly");
        CHECK(musepack_decoder_position(d) == frames,
              "nonseekable failed seek preserves position");
        decoded = decode_all(d);
        CHECK(decoded.terminal == MUSEPACK_ERR_EOF,
              "nonseekable sequential decode reaches EOF");
        CHECK(frames + decoded.frames == samples,
              "nonseekable sequential decode completes");
        musepack_decoder_close(d);
    }

    tagged = malloc(size + 10);
    CHECK(tagged != 0, "allocate ID3-prefixed fixture");
    if (tagged != 0) {
        static const unsigned char id3[] = {'I','D','3',4,0,0,0,0,0,0};
        memcpy(tagged, id3, sizeof id3);
        memcpy(tagged + sizeof id3, data, size);
        init_test_reader(&reader, &context, tagged, size + sizeof id3, MPC_FALSE);
        d = musepack_decoder_open(&reader, &err);
        CHECK(d != 0, "forward ID3 skip on nonseekable reader");
        if (d != 0) {
            decoded = decode_all(d);
            CHECK(decoded.terminal == MUSEPACK_ERR_EOF && decoded.frames == samples,
                  "ID3-prefixed nonseekable decode completes at EOF");
            musepack_decoder_close(d);
        }
        free(tagged);
    }

    init_test_reader(&reader, &context, data, size, MPC_TRUE);
	context.max_read = 4096;
    d = musepack_decoder_open(&reader, &err);
    CHECK(d != 0, "open reader for failed seek");
    if (d != 0) {
        CHECK(musepack_decoder_read(d, pcm, 200, &frames) == MUSEPACK_OK,
              "read before failed seek");
        context.fail_seek_call = context.seek_calls + 1;
        CHECK(musepack_decoder_seek_sample(d, samples / 2) == MUSEPACK_ERR_SEEK,
		      "reader seek failure after decoding is propagated");
		CHECK(context.seek_failures == 1, "reader seek callback failed once");
        CHECK(musepack_decoder_position(d) == frames,
              "internal failed seek preserves facade position");
		CHECK(musepack_decoder_read(d, pcm, 200, &next_frames) == MUSEPACK_OK &&
		      next_frames == 200, "read recovers after seek callback failure");
		context.fail_read_call = context.read_calls + 1;
		CHECK(musepack_decoder_seek_sample(d, samples * 3 / 4) == MUSEPACK_ERR_SEEK,
		      "post-seek refill failure is propagated");
		CHECK(context.read_failures == 1, "reader refill failed after physical seek");
		CHECK(musepack_decoder_position(d) == frames + next_frames,
		      "post-seek refill failure preserves facade position");
		decoded = decode_all(d);
		CHECK(decoded.terminal == MUSEPACK_ERR_EOF,
		      "decode recovers after transactional seek failure");
		CHECK(frames + next_frames + decoded.frames == samples,
		      "failed seek preserves the original decode position");
        musepack_decoder_close(d);
    }
    free(data);
}

/* ------------------------------------------------------------------ */
/* multiple independent instances                                      */
/* ------------------------------------------------------------------ */
static void test_multiple_instances(const char *fa, unsigned long sa,
                                    const char *fb, unsigned long sb)
{
    size_t za, zb;
    unsigned char *da = load_file(fa, &za);
    unsigned char *db = load_file(fb, &zb);
    mpc_reader ra1, ra2, rb1;
    musepack_decoder *da1, *da2, *db1;
    musepack_error err;
	decode_result decoded;

    /* Each decoder gets its own reader so instances are fully independent. */
    mpc_reader_init_memory(&ra1, da, za);
    mpc_reader_init_memory(&ra2, da, za);
    mpc_reader_init_memory(&rb1, db, zb);
    da1 = musepack_decoder_open(&ra1, &err);
    da2 = musepack_decoder_open(&ra2, &err);   /* second, independent instance */
    db1 = musepack_decoder_open(&rb1, &err);
    CHECK(da1 != 0 && da2 != 0 && db1 != 0, "three instances open");

    /* interleave: decode a bit of each, alternate */
    {
        static float pcm[MUSEPACK_FRAME_MAX * 2];
        uint64_t f1 = 0, f2 = 0, f3 = 0, n;
        while (musepack_decoder_read(da1, pcm, MUSEPACK_FRAME_MAX, &n) == MUSEPACK_OK)
            f1 += n;
        CHECK(f1 == sa, "instance 1 full decode");
        while (musepack_decoder_read(db1, pcm, MUSEPACK_FRAME_MAX, &n) == MUSEPACK_OK)
            f2 += n;
        CHECK(f2 == sb, "instance 2 (other file) full decode");
        musepack_decoder_seek_sample(da2, 0);
        while (musepack_decoder_read(da2, pcm, MUSEPACK_FRAME_MAX, &n) == MUSEPACK_OK)
            f3 += n;
        CHECK(f3 == sa, "instance 3 (repeat of file A) full decode");
    }

	decoded = decode_all(da1);
	CHECK(decoded.terminal == MUSEPACK_ERR_EOF && decoded.frames == 0,
	      "instance 1 already exhausted at EOF");
    musepack_decoder_close(da1);
    musepack_decoder_close(da2);
    musepack_decoder_close(db1);
    mpc_reader_exit_memory(&ra1);
    mpc_reader_exit_memory(&ra2);
    mpc_reader_exit_memory(&rb1);
    free(da);
    free(db);
}

static unsigned long get_length(const char *fixture)
{
    size_t sz;
    unsigned char *data = load_file(fixture, &sz);
    mpc_reader reader;
    musepack_decoder *d;
    musepack_error err;
    unsigned long length;

    if (data == 0) return 0;
    mpc_reader_init_memory(&reader, data, sz);
    d = musepack_decoder_open(&reader, &err);
    if (d == 0) { free(data); return 0; }
    length = (unsigned long) musepack_decoder_length_samples(d);
    musepack_decoder_close(d);
    mpc_reader_exit_memory(&reader);
    free(data);
    return length;
}

int main(int argc, char **argv)
{
    const char *fa, *fb;
    unsigned long sa, sb;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <fixture-a.mpc> <fixture-b.mpc>\n", argv[0]);
        return 2;
    }
    fa = argv[1];
    fb = argv[2];
    sa = get_length(fa);
    sb = get_length(fb);
    if (sa == 0 || sb == 0) {
        fprintf(stderr, "cannot read fixture lengths\n");
        return 1;
    }

    test_lifecycle(fa);
    test_invalid_input();
	test_hostile_accounting(fa);
	test_hostile_st(fa);
    test_streaminfo(fa, 44100, 2, 8, 44100);
    test_full_decode(fa, sa);
	test_valid_sv7(getenv("MPC_TEST_SV7_FIXTURE"));
    test_seeking(fa, sa, 44100);
    test_nonseekable(fa, sa);
    test_multiple_instances(fa, sa, fb, sb);

    if (failures) {
        fprintf(stderr, "%d api test(s) failed\n", failures);
        return 1;
    }
    printf("all api tests passed\n");
    return 0;
}
