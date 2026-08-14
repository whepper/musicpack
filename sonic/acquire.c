/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved. (BSD-3-Clause; see LICENSES/BSD-3-Clause.txt for the full text.)
  SPDX-License-Identifier: BSD-3-Clause
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <musicpack/checksum.h>

#include "acquire.h"
#include "sonic_profile.h"

static int
dev_hints_enabled(void)
{
    const char *e = getenv("MUSICPACK_SONIC_DEV");
    return e != 0 && *e != '\0' && strcmp(e, "0") != 0;
}

static int
model_sha_ok(const char *path)
{
    char hex[MUSICPACK_SHA256_HEX_SIZE];
    if (musicpack_sha256_file(path, hex, sizeof hex) != MUSICPACK_OK)
        return -1; /* unreadable */
    return musicpack_sha256_eq(hex, SONIC_PROFILE_ONNX_SHA256) ? 1 : 0;
}

sonic_model_status
sonic_acquire_model(const char *model_dir, char **path_out)
{
    char *candidate = 0;

    if (path_out == 0)
        return SONIC_MODEL_UNREADABLE;
    *path_out = 0;

    if (model_dir != 0 && *model_dir != '\0') {
        size_t n = strlen(model_dir) + strlen(SONIC_PROFILE_ONNX_FILE) + 2;
        candidate = (char *) malloc(n);
        if (candidate == 0)
            return SONIC_MODEL_UNREADABLE;
        snprintf(candidate, n, "%s/%s", model_dir, SONIC_PROFILE_ONNX_FILE);
        {
            int ok = model_sha_ok(candidate);
            if (ok == 1) {
                *path_out = candidate;
                return SONIC_MODEL_OK;
            }
            fprintf(stderr,
                    "sonic: model '%s' %s\n", candidate,
                    ok == 0
                        ? "fails the pinned SHA-256 check"
                        : "cannot be read");
            free(candidate);
        }
    }

    {
        const char *env = getenv("MUSICPACK_SONIC_MODEL_PATH");
        if (env != 0 && *env != '\0') {
            int ok = model_sha_ok(env);
            if (ok == 1) {
                *path_out = strdup(env);
                if (*path_out == 0)
                    return SONIC_MODEL_UNREADABLE;
                return SONIC_MODEL_OK;
            }
            if (ok == 0) {
                fprintf(stderr,
                        "sonic: model '%s' fails the pinned SHA-256 check\n", env);
                return SONIC_MODEL_CHECKSUM;
            }
        }
    }

    if (dev_hints_enabled()) {
        fprintf(stderr,
                "sonic: the %s analysis model is unavailable.\n"
                "  Expected '%s' in the model cache directory.\n"
                "  Produce it from the pinned OpenL3 weights with:\n"
                "    research/sonic/.venv/bin/python research/sonic/convert_openl3.py \\\n"
                "        <openl3_audio_mel256_music.h5> <model-cache>/%s\n"
                "  (or point MUSICPACK_SONIC_MODEL_PATH at a SHA-256-verified copy)\n",
                SONIC_PROFILE_ID, SONIC_PROFILE_ONNX_FILE, SONIC_PROFILE_ONNX_FILE);
    } else {
        fprintf(stderr,
                "sonic: the %s analysis model is not installed\n",
                SONIC_PROFILE_ID);
    }
    return SONIC_MODEL_MISSING;
}
