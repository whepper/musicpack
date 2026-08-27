/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved. (BSD-3-Clause; see LICENSES/BSD-3-Clause.txt for the full text.)
  SPDX-License-Identifier: BSD-3-Clause

  Audio decode to mono float32 PCM at native sample rate. MPC via
  libmusepack (the same reader handoff libmusicpack provides), FLAC via the
  vendored dr_flac single header, WAV via a minimal RIFF parser. The
  analyzer always analyzes decoded PCM, never compressed bytes.
*/
#ifndef SONIC_DECODE_H_
#define SONIC_DECODE_H_

#include <stddef.h>

typedef struct sonic_pcm {
    float *samples;   /* mono float32, owned */
    size_t count;     /* number of samples */
    int sample_rate;  /* native rate */
} sonic_pcm;

/* Decodes an audio file (.mpc / .flac / .wav) to mono float32 at its native
   sample rate. Returns 1 on success (0 on failure). */
int sonic_decode(const char *path, sonic_pcm *out);

/* Releases a decoded buffer. */
void sonic_pcm_free(sonic_pcm *pcm);

#endif /* SONIC_DECODE_H_ */
