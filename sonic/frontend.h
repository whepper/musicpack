/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved. (BSD-3-Clause; see LICENSES/BSD-3-Clause.txt for the full text.)
  SPDX-License-Identifier: BSD-3-Clause

  The musicpack-sonic-openl3-v1 DSP core: resample (resampy kaiser_best),
  mel frontend (kapre STFT/mel/decibel), window framing, mean-norm pooling
  and equal-track album aggregation. This is the C port of the numpy
  reference research/sonic/frontend.py; embedding compatibility against the
  research harness is verified by research/sonic/compat_measure.py.
*/
#ifndef SONIC_FRONTEND_H_
#define SONIC_FRONTEND_H_

#include <stddef.h>

/* Model-window count for a signal of `len` samples AFTER center padding
   (openl3.core._pad_audio + window framing at hop 1.0 s). Never 0. */
int sonic_frame_count(size_t len);

/* Centers + window-frames a mono signal: prepends SONIC_CENTER_PAD zeros and
   pads the tail so `*out_len` is an exact multiple of SONIC_FRAME. `out` must
   hold at least sonic_frame_count(len) * SONIC_FRAME floats. Returns the
   window count. */
int sonic_center_window(const float *pcm, size_t n, float *out, size_t cap,
                        size_t *out_len);

/* Resamples mono float32 from `sr` to SONIC_SAMPLE_RATE with the resampy
   kaiser_best polyphase filter. `out` holds at least (n * SR / sr) floats;
   returns the output length. Identical rate returns a copy. */
size_t sonic_resample(const float *in, size_t n, int sr, float *out, size_t out_cap);

/* librosa.filters.mel(...).T (1025 x SONIC_N_MELS) float32, matching
   kapre 0.3.6. `fb` holds (SONIC_N_FFT/2+1) * SONIC_N_MELS floats. */
void sonic_mel_filterbank(float *fb, size_t cap);

/* Per-window mel frontend: one SONIC_FRAME window -> (SONIC_N_MELS,
   n_frames) float32 log-mel (openl3's kapre_v0_1_4_magnitude_to_decibel,
   permuted to (n_mels, n_frames)). `mel` holds at least SONIC_N_MELS *
   SONIC_MEL_FRAMES floats. Returns the number of mel frames. */
#define SONIC_MEL_FRAMES 199 /* ceil(48000 / 242) */
int sonic_mel(const float *window, float *mel, size_t mel_cap);

/* mean-norm pooling over (n, dims) window embeddings -> unit vector in
   `out`. Returns 1 on success; 0 when no windows survive (no embedding). */
int sonic_pool_mean_norm(const float *emb, size_t n, size_t dims, float *out);

/* equal-track album aggregation over (n, dims) track vectors; tracks with
   present[i] == 0 are skipped. Returns the number of contributing tracks
   (0 => no album embedding); `out` is the L2-normalized mean when > 0. */
size_t sonic_album_equal(const float *vecs, const int *present, size_t n,
                         size_t dims, float *out);

/* bessel I0 (needed to build the resample filter) */
double sonic_bessel_i0(double x);

/* Exposes the resampy kaiser_best polyphase table (win and its derivative),
   each SONIC_RESAMPLE_N + 1 doubles. */
void sonic_resample_filter(double *win, double *delta, size_t cap);

#endif /* SONIC_FRONTEND_H_ */
