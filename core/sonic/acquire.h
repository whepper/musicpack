/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved. (BSD-3-Clause; see LICENSES/BSD-3-Clause.txt for the full text.)
  SPDX-License-Identifier: BSD-3-Clause

  Model resolution for the analyzer. The post-frontend ONNX is an immutable,
  SHA-256-pinned artifact (derived from the profile's pinned OpenL3 weights
  by research/sonic/convert_openl3.py). The analyzer never downloads: it
  only locates and verifies a model that trusted MusicPack Author
  configuration has placed on disk. A package-provided profile can never
  trigger acquisition.
*/
#ifndef SONIC_ACQUIRE_H_
#define SONIC_ACQUIRE_H_

/* Why no verified model could be resolved. */
typedef enum sonic_model_status {
    SONIC_MODEL_OK = 0,          /* path_out holds a SHA-256-verified model */
    SONIC_MODEL_MISSING,         /* no model file present anywhere */
    SONIC_MODEL_CHECKSUM,        /* present but fails the pinned SHA-256 */
    SONIC_MODEL_UNREADABLE       /* present but cannot be read/hashed */
} sonic_model_status;

/* Resolves a SHA-256-verified model path for the analyzer's profile.
   Search order: <model_dir>/openl3_post.onnx, then MUSICPACK_SONIC_MODEL_PATH
   (a development override). On SONIC_MODEL_OK, *path_out is a malloc'd path
   the caller frees. On failure *path_out is NULL and a clean message is
   emitted to stderr (long developer instructions only under
   MUSICPACK_SONIC_DEV=1). */
sonic_model_status sonic_acquire_model(const char *model_dir, char **path_out);

#endif /* SONIC_ACQUIRE_H_ */
