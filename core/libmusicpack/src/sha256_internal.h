/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
*/
/// \file sha256_internal.h
/// Internal incremental SHA-256 access for translation units that must
/// hash byte ranges obtained through a container source (the MPAK
/// range-backed backend). Not installed; the public one-shot helpers in
/// <musicpack/checksum.h> remain the supported interface.
#ifndef MUSICPACK_SHA256_INTERNAL_H_
#define MUSICPACK_SHA256_INTERNAL_H_

#include <stddef.h>
#include <stdint.h>

typedef struct musicpack_sha256_ctx {
    unsigned int h[8];
    unsigned long long len;
    unsigned char block[64];
    unsigned int block_used;
} musicpack_sha256_ctx;

void musicpack_sha256_init(musicpack_sha256_ctx *c);
void musicpack_sha256_update(musicpack_sha256_ctx *c, const void *data,
                             size_t len);
void musicpack_sha256_final(musicpack_sha256_ctx *c, unsigned char out[32]);

#endif /* MUSICPACK_SHA256_INTERNAL_H_ */
