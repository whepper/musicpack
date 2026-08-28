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

/* Hardened regular-file open (O_NOFOLLOW / nlink==1 on POSIX) shared by
   the directory and MPAK backends. */
FILE *musicpack_open_regular_read(const char *path);
/* Size of a hardened regular file, or -1. */
long long musicpack_checked_file_size(const char *path);

/* ---- storage backend member I/O -------------------------------------
   A package handle with `io != NULL` routes member access through the
   backend vtable instead of the filesystem under `root`. The MPAK
   backend implements this; the directory backend uses the filesystem
   directly (io == NULL). All paths are canonical package-relative paths. */
typedef struct musicpack_member_io {
    void *ctx;
    /* Size of the member in bytes. ERR_PATH: unsafe path; ERR_MISSING:
       absent member. */
    musicpack_status (*size)(void *ctx, const char *rel, long long *out);
    /* Lowercase-hex SHA-256 of the member bytes. */
    musicpack_status (*sha256)(void *ctx, const char *rel, char *hex, size_t cap);
    /* Bounded full read; malloc'd buffer in *out. */
    musicpack_status (*read)(void *ctx, const char *rel, size_t max,
                             unsigned char **out, size_t *len);
    /* All member paths (malloc'd array of malloc'd strings). */
    musicpack_status (*list)(void *ctx, char ***paths, size_t *count);
} musicpack_member_io;

struct musicpack_package {
    char *root;             /* absolute package root (directory backend) or
                               container file path (MPAK backend) */
    musicpack_manifest *manifest;
    cJSON *original;        /* original manifest tree (unknown-field save) */
    const musicpack_member_io *io; /* NULL = directory backend */
    void *io_ctx;           /* owned by the backend */
};

/* mpak.c */
musicpack_package *musicpack_mpak_open_package(const char *file, musicpack_status *status);
void musicpack_mpak_io_free(void *io_ctx);
musicpack_status musicpack_mpak_verify_extra(const musicpack_package *pkg,
                                             musicpack_report *rep,
                                             musicpack_report_fn fn, void *ctx,
                                             int *failed);
musicpack_status musicpack_mpak_member_reader(const musicpack_package *pkg,
                                              const char *rel, mpc_reader *reader);
int musicpack_mpak_reader_is_container(const mpc_reader *reader);

/* base64.c — strict standard-alphabet base64 (Sonic vector encoding).
   Both return 1 on success; the output is malloc'd. */
int musicpack_base64_decode(const char *s, size_t n, unsigned char **out, size_t *out_len);
int musicpack_base64_encode(const unsigned char *data, size_t len, char **out);

#endif /* MUSICPACK_INTERNAL_H_ */
