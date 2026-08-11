/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved. (BSD-2-Clause; see the top-level headers.)
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <musicpack/checksum.h>

#include "acquire.h"
#include "sonic_profile.h"

static int
model_sha_ok(const char *path)
{
    char hex[MUSICPACK_SHA256_HEX_SIZE];
    if (musicpack_sha256_file(path, hex, sizeof hex) != MUSICPACK_OK)
        return 0;
    return musicpack_sha256_eq(hex, SONIC_PROFILE_ONNX_SHA256);
}

static char *
check_candidate(const char *path)
{
    if (path == 0 || *path == '\0')
        return 0;
    if (!model_sha_ok(path)) {
        fprintf(stderr, "sonic: model '%s' fails the pinned SHA-256 check\n", path);
        return 0;
    }
    return strdup(path);
}

char *
sonic_acquire_model(const char *model_dir)
{
    char *candidate;

    if (model_dir != 0 && *model_dir != '\0') {
        size_t n = strlen(model_dir) + strlen(SONIC_PROFILE_ONNX_FILE) + 2;
        candidate = (char *) malloc(n);
        if (candidate != 0) {
            snprintf(candidate, n, "%s/%s", model_dir, SONIC_PROFILE_ONNX_FILE);
            if (model_sha_ok(candidate))
                return candidate;
            fprintf(stderr,
                    "sonic: model '%s' is missing or fails the pinned SHA-256 "
                    "check (%s)\n",
                    candidate, SONIC_PROFILE_ONNX_SHA256);
            free(candidate);
        }
    }
    {
        const char *env = getenv("MUSICPACK_SONIC_MODEL_PATH");
        if (env != 0 && *env != '\0') {
            candidate = check_candidate(env);
            if (candidate != 0)
                return candidate;
        }
    }
    fprintf(stderr,
            "sonic: the %s analysis model is unavailable.\n"
            "  Expected '%s' in the model cache directory.\n"
            "  Produce it from the pinned OpenL3 weights with:\n"
            "    research/sonic/.venv/bin/python research/sonic/convert_openl3.py \\\n"
            "        <openl3_audio_mel256_music.h5> <model-cache>/%s\n"
            "  (or point MUSICPACK_SONIC_MODEL_PATH at a SHA-256-verified copy)\n",
            SONIC_PROFILE_ID, SONIC_PROFILE_ONNX_FILE, SONIC_PROFILE_ONNX_FILE);
    return 0;
}
