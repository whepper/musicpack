/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved. (BSD-2-Clause; see the top-level headers.)
*/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
# include <direct.h>
# define mkdir_one(p) _mkdir(p)
#else
# include <sys/stat.h>
# define mkdir_one(p) mkdir(p, 0755)
#endif

#include <musicpack/checksum.h>
#include <musicpack/musicpack.h>

#include "cache.h"
#include "sonic_profile.h"

static int
mkdir_p(const char *path)
{
    char tmp[4096];
    size_t len = strlen(path), i;
    if (len == 0 || len >= sizeof tmp)
        return -1;
    memcpy(tmp, path, len + 1);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';
    for (i = 1; tmp[i] != '\0'; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir_one(tmp) != 0 && errno != EEXIST)
                return -1;
            tmp[i] = '/';
        }
    }
    if (mkdir_one(tmp) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int
entry_path(char *out, size_t cap, const char *cache_dir, const char *profile_id,
           const char *audio_sha)
{
    if (snprintf(out, cap, "%s/%s/%s.vec", cache_dir, profile_id, audio_sha) >=
        (int) cap)
        return -1;
    return 0;
}

static int
read_b64(const char *path, char **data)
{
    FILE *f = fopen(path, "rb");
    long len;
    char *buf;
    if (f == 0)
        return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    buf = (char *) malloc((size_t) len + 1);
    if (buf == 0) {
        fclose(f);
        return 0;
    }
    if (len > 0 && fread(buf, 1, (size_t) len, f) != (size_t) len) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    buf[len] = '\0';
    *data = buf;
    return 1;
}

int
sonic_cache_load(const char *cache_dir, const char *profile_id,
                 const char *weights_sha, const char *audio_sha, float *out,
                 size_t dims, int *present)
{
    char path[4096];
    char *content = 0;
    char *nl1, *nl2, *nl3, *b64;
    float *vec = 0;
    size_t n;
    int rc = 0;

    if (cache_dir == 0 || profile_id == 0 || weights_sha == 0 || audio_sha == 0)
        return 0;
    if (entry_path(path, sizeof path, cache_dir, profile_id, audio_sha) != 0)
        return 0;
    if (!read_b64(path, &content))
        return 0;

    /* format: profile_id\nweights_sha\n<dims>\n<base64|"null"> */
    nl1 = strchr(content, '\n');
    if (nl1 == 0)
        goto done;
    *nl1 = '\0';
    nl2 = strchr(nl1 + 1, '\n');
    if (nl2 == 0)
        goto done;
    *nl2 = '\0';
    nl3 = strchr(nl2 + 1, '\n');
    if (nl3 == 0)
        goto done;
    *nl3 = '\0';
    if (strcmp(content, profile_id) != 0 || strcmp(nl1 + 1, weights_sha) != 0)
        goto done; /* stale: different profile / weights */
    if ((size_t) atoi(nl2 + 1) != dims)
        goto done;
    b64 = nl3 + 1;
    /* strip the trailing newline */
    {
        size_t bl = strlen(b64);
        while (bl > 0 && (b64[bl - 1] == '\n' || b64[bl - 1] == '\r'))
            b64[--bl] = '\0';
    }

    if (strcmp(b64, "null") == 0) {
        *present = 0;
        rc = 1;
        goto done;
    }
    if (musicpack_sonic_vector_decode(b64, strlen(b64), dims, &vec, &n) !=
        MUSICPACK_OK)
        goto done;
    if (n != dims)
        goto done;
    memcpy(out, vec, dims * sizeof(float));
    *present = 1;
    rc = 1;
done:
    free(vec);
    free(content);
    return rc;
}

int
sonic_cache_store(const char *cache_dir, const char *profile_id,
                  const char *weights_sha, const char *audio_sha,
                  const float *vec, size_t dims, int present)
{
    char path[4096];
    char dir[4096];
    FILE *f;
    char *b64 = 0;
    int rc = 0;

    if (cache_dir == 0 || profile_id == 0 || weights_sha == 0 || audio_sha == 0)
        return 0;
    if (entry_path(path, sizeof path, cache_dir, profile_id, audio_sha) != 0)
        return 0;
    if (snprintf(dir, sizeof dir, "%s/%s", cache_dir, profile_id) >= (int) sizeof dir)
        return 0;
    if (mkdir_p(dir) != 0)
        return 0;
    if (present) {
        if (musicpack_sonic_vector_encode(vec, dims, &b64) != MUSICPACK_OK)
            return 0;
    }
    f = fopen(path, "wb");
    if (f == 0) {
        free(b64);
        return 0;
    }
    if (present)
        fprintf(f, "%s\n%s\n%zu\n%s\n", profile_id, weights_sha, dims, b64);
    else
        fprintf(f, "%s\n%s\n%zu\nnull\n", profile_id, weights_sha, dims);
    rc = fclose(f) == 0 ? 1 : 0;
    free(b64);
    return rc;
}
