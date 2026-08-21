/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved. (BSD-3-Clause; see LICENSES/BSD-3-Clause.txt for the full text.)
  SPDX-License-Identifier: BSD-3-Clause

  Sonic analysis profiles. Only the pinned openl3-v1 profile is implemented
  in the analyzer; the profile id and every profile-defining parameter are
  frozen constants — never library defaults, never runtime-discovered.
  A profile-defining behavior change requires a new profile id.
*/
#ifndef SONIC_PROFILE_H_
#define SONIC_PROFILE_H_

#define SONIC_PROFILE_ID "musicpack-sonic-openl3-v1"
#define SONIC_PROFILE_DIMENSIONS 512
#define SONIC_PROFILE_ENCODING "base64-f32le"
#define SONIC_PROFILE_DISTANCE "cosine"

/* OpenL3 0.4.0 weights (the H5 the post-frontend ONNX is derived from). */
#define SONIC_PROFILE_WEIGHTS_SHA256 \
    "624ee7b1dd5ff87e18073f66fd8b2052bebb8ac70210e9c0937c0c940c63e9d6"
/* The post-frontend ONNX artifact (derived from those exact weights by
   research/sonic/convert_openl3.py). Pinned and verified before use. */
#define SONIC_PROFILE_ONNX_FILE "openl3_post.onnx"
#define SONIC_PROFILE_ONNX_SHA256 \
    "fc51d01d1c33f9d1d783ceda7727f5f495c6c5639f1340b224396f2396750331"
/* Expected artifact size in bytes (the immutable release asset). */
#define SONIC_PROFILE_ONNX_SIZE 18742941

/* Frontend (openl3 0.4.0 + kapre 0.3.6). */
#define SONIC_SAMPLE_RATE 48000
#define SONIC_FRAME (SONIC_SAMPLE_RATE) /* 1.0 s window */
#define SONIC_CENTER_PAD (SONIC_FRAME / 2) /* 24000 zero samples */
#define SONIC_HOP 242
#define SONIC_N_FFT 2048
#define SONIC_N_MELS 256
#define SONIC_AMIN 1e-10
#define SONIC_DYNAMIC_RANGE 80.0

/* Resampler (resampy 0.4.3 kaiser_best, polyphase with linear
   interpolation between table entries). */
#define SONIC_RESAMPLE_BETA 12.9846
#define SONIC_RESAMPLE_ROLLOFF 0.9173473712608761
#define SONIC_RESAMPLE_ZEROS 50
#define SONIC_RESAMPLE_BITS 8192 /* samples per zero-crossing */
#define SONIC_RESAMPLE_N (SONIC_RESAMPLE_ZEROS * SONIC_RESAMPLE_BITS)

/* Vector normalization tolerance (spec §7). */
#define SONIC_NORM_TOLERANCE 1e-3

#endif /* SONIC_PROFILE_H_ */
