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
/// \file base64.c
/// Strict base64 (standard alphabet, mandatory padding) used by the Sonic
/// document's `base64-f32le` vector encoding.

#include <stdlib.h>
#include <string.h>

#include "internal.h"

static int
b64_val(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return (int) (c - 'A');
    if (c >= 'a' && c <= 'z') return (int) (c - 'a' + 26);
    if (c >= '0' && c <= '9') return (int) (c - '0' + 52);
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Strict decode: standard alphabet, `=` padding only at the end, no
   whitespace, length a multiple of 4. Returns 1 on success. */
int
musicpack_base64_decode(const char *s, size_t n, unsigned char **out, size_t *out_len)
{
    unsigned char *buf;
    size_t i, o = 0;

    if (s == 0 || n == 0 || n % 4 != 0)
        return 0;

    /* validate the whole string first (padding only at the very end) */
    {
        size_t pad = 0;
        for (i = 0; i < n; i++) {
            unsigned char c = (unsigned char) s[i];
            if (c == '=') {
                pad++;
            } else {
                if (pad > 0 || b64_val(c) < 0)
                    return 0;
            }
        }
        if (pad > 2)
            return 0;
    }

    buf = (unsigned char *) malloc((n / 4) * 3);
    if (buf == 0)
        return 0;

    for (i = 0; i < n; i += 4) {
        int v0 = b64_val((unsigned char) s[i]);
        int v1 = b64_val((unsigned char) s[i + 1]);
        int v2 = s[i + 2] == '=' ? 0 : b64_val((unsigned char) s[i + 2]);
        int v3 = s[i + 3] == '=' ? 0 : b64_val((unsigned char) s[i + 3]);
        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) {
            free(buf);
            return 0;
        }
        buf[o++] = (unsigned char) ((v0 << 2) | (v1 >> 4));
        if (s[i + 2] != '=')
            buf[o++] = (unsigned char) (((v1 & 0x0f) << 4) | (v2 >> 2));
        if (s[i + 3] != '=')
            buf[o++] = (unsigned char) (((v2 & 0x03) << 6) | v3);
    }
    *out = buf;
    *out_len = o;
    return 1;
}

/* Standard encode with padding. Returns 1 on success. */
int
musicpack_base64_encode(const unsigned char *data, size_t len, char **out)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_len = ((len + 2) / 3) * 4;
    char *buf;
    size_t i, o = 0;

    if (data == 0 && len > 0)
        return 0;
    buf = (char *) malloc(out_len + 1);
    if (buf == 0)
        return 0;

    for (i = 0; i < len; i += 3) {
        unsigned int a = data[i];
        unsigned int b = i + 1 < len ? data[i + 1] : 0;
        unsigned int c = i + 2 < len ? data[i + 2] : 0;
        unsigned int n = (a << 16) | (b << 8) | c;
        buf[o++] = tbl[(n >> 18) & 0x3f];
        buf[o++] = tbl[(n >> 12) & 0x3f];
        buf[o++] = i + 1 < len ? tbl[(n >> 6) & 0x3f] : '=';
        buf[o++] = i + 2 < len ? tbl[n & 0x3f] : '=';
    }
    buf[o] = '\0';
    *out = buf;
    return 1;
}
