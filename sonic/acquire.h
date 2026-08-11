/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved. (BSD-2-Clause; see the top-level headers.)

  Controlled model acquisition. The post-frontend ONNX is an immutable,
  SHA-256-pinned artifact (derived from the profile's pinned OpenL3 weights
  by research/sonic/convert_openl3.py). A package-provided profile can never
  trigger a download: acquisition only ever happens for the analyzer's own
  profile in trusted MusicPack Author configuration.
*/
#ifndef SONIC_ACQUIRE_H_
#define SONIC_ACQUIRE_H_

/* Resolves a SHA-256-verified model path for the analyzer's profile.
   Search order: <model_dir>/openl3_post.onnx, then MUSICPACK_SONIC_MODEL_PATH
   (a development override). Returns a malloc'd path on success, NULL when the
   model is unavailable (clear error emitted to stderr). */
char *sonic_acquire_model(const char *model_dir);

#endif /* SONIC_ACQUIRE_H_ */
