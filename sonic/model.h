/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved. (BSD-3-Clause; see LICENSES/BSD-3-Clause.txt for the full text.)
  SPDX-License-Identifier: BSD-3-Clause

  ONNX Runtime session for the post-frontend OpenL3 network. The graph is
  loaded from a SHA-256-verified file (see acquire.c); feeding it mel
  spectrograms produces window embeddings. Single-threaded inference keeps
  the results deterministic.
*/
#ifndef SONIC_MODEL_H_
#define SONIC_MODEL_H_

#include <stddef.h>

typedef struct sonic_model sonic_model;

/* Opens the ONNX model at `onnx_path`. Returns NULL on failure. */
sonic_model *sonic_model_open(const char *onnx_path);

void sonic_model_close(sonic_model *m);

/* Runs `n_windows` mel spectrograms (each 256 x 199 x 1, contiguous float32,
   window-major) through the network. Fills `out` with n_windows * 512
   float32 embeddings. Returns 1 on success. */
int sonic_model_run(const sonic_model *m, const float *mel, size_t n_windows,
                    float *out, size_t out_cap);

#endif /* SONIC_MODEL_H_ */
