/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved.
  (BSD-3-Clause; see LICENSES/BSD-3-Clause.txt for the full text.)
  SPDX-License-Identifier: BSD-3-Clause
*/
/// \file internal.h
/// Internal declarations shared between libmusicpack translation units.
#ifndef MUSICPACK_INTERNAL_H_
#define MUSICPACK_INTERNAL_H_

#include <musicpack/musicpack.h>

#include "cJSON.h"

/* manifest.c */
musicpack_status musicpack_manifest_parse_tree(const cJSON *root, musicpack_manifest *m);
musicpack_status musicpack_manifest_write_with_original(const musicpack_manifest *m,
                                                        const cJSON *original,
                                                        char **json_out);

/* package.c */
int musicpack_report_error(musicpack_report *rep);

/* base64.c — strict standard-alphabet base64 (Sonic vector encoding).
   Both return 1 on success; the output is malloc'd. */
int musicpack_base64_decode(const char *s, size_t n, unsigned char **out, size_t *out_len);
int musicpack_base64_encode(const unsigned char *data, size_t len, char **out);

#endif /* MUSICPACK_INTERNAL_H_ */
