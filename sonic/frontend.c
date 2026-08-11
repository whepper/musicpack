/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved. (BSD-2-Clause; see the top-level headers.)

  DSP core (see frontend.h). Numerical details are pinned by the profile and
  match the numpy reference exactly (research/sonic/frontend.py).
*/

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "frontend.h"
#include "sonic_profile.h"

/* ------------------------------------------------------------------ */
/* resampler (resampy 0.4.3 kaiser_best)                               */
/* ------------------------------------------------------------------ */

double
sonic_bessel_i0(double x)
{
    /* Abramowitz & Stegun 9.8.1 / 9.8.2 (~1e-8 relative). */
    double ax = fabs(x), y, p2;
    static const double p[] = {
        1.0, 3.5156229, 3.0899424, 1.2067492, 0.2659732, 0.0360768, 0.0045813
    };
    static const double q[] = {
        0.39894228, 0.01328592, 0.00225319, -0.00157565, 0.00916281,
        -0.02057706, 0.02635537, -0.01647633, 0.00392377
    };
    if (ax < 3.75) {
        y = ax / 3.75;
        y = y * y;
        return p[0] + y * (p[1] + y * (p[2] + y * (p[3] + y * (p[4] + y * (p[5] + y * p[6])))));
    }
    y = 3.75 / ax;
    p2 = q[0] + y * (q[1] + y * (q[2] + y * (q[3] + y * (q[4] + y * (q[5] + y * (q[6] + y * (q[7] + y * q[8])))))));
    return exp(ax) / sqrt(ax) * p2;
}

static double *g_interp_win;
static double *g_interp_delta;

static void
build_resample_filter(void)
{
    int i;
    const double n = SONIC_RESAMPLE_N;
    const double i0b = sonic_bessel_i0(SONIC_RESAMPLE_BETA);
    double *w = (double *) malloc(((size_t) SONIC_RESAMPLE_N + 1) * sizeof *w);
    double *d = (double *) malloc(((size_t) SONIC_RESAMPLE_N + 1) * sizeof *d);

    if (w == 0 || d == 0) {
        free(w);
        free(d);
        return;
    }
    for (i = 0; i <= SONIC_RESAMPLE_N; i++) {
        double x = (double) i / SONIC_RESAMPLE_BITS; /* zero-crossing offset */
        double sinc = x == 0.0
                          ? 1.0
                          : sin(M_PI * SONIC_RESAMPLE_ROLLOFF * x) /
                                (M_PI * SONIC_RESAMPLE_ROLLOFF * x);
        double arg = (double) i / n;
        double kaiser = sonic_bessel_i0(SONIC_RESAMPLE_BETA * sqrt(1.0 - arg * arg)) / i0b;
        w[i] = SONIC_RESAMPLE_ROLLOFF * sinc * kaiser;
    }
    for (i = 0; i < SONIC_RESAMPLE_N; i++)
        d[i] = w[i + 1] - w[i];
    d[SONIC_RESAMPLE_N] = 0.0;
    g_interp_win = w;
    g_interp_delta = d;
}

void
sonic_resample_filter(double *win, double *delta, size_t cap)
{
    if (g_interp_win == 0)
        build_resample_filter();
    if (g_interp_win == 0)
        return;
    if (cap < (size_t) SONIC_RESAMPLE_N + 1)
        return;
    memcpy(win, g_interp_win, ((size_t) SONIC_RESAMPLE_N + 1) * sizeof *win);
    memcpy(delta, g_interp_delta, ((size_t) SONIC_RESAMPLE_N + 1) * sizeof *delta);
}

size_t
sonic_resample(const float *in, size_t n_in, int sr_in, float *out, size_t out_cap)
{
    size_t t, n_out;
    double sample_ratio;
    double win_scale, scale, time_increment;
    long index_step;
    const size_t nwin = (size_t) SONIC_RESAMPLE_N + 1;

    if (sr_in == SONIC_SAMPLE_RATE) {
        if (n_in > out_cap)
            n_in = out_cap;
        memcpy(out, in, n_in * sizeof(float));
        return n_in;
    }
    if (g_interp_win == 0)
        build_resample_filter();
    if (g_interp_win == 0)
        return 0;

    sample_ratio = (double) SONIC_SAMPLE_RATE / (double) sr_in;
    /* resampy scales the interpolation window when downsampling */
    win_scale = sample_ratio < 1.0 ? sample_ratio : 1.0;
    scale = win_scale;
    index_step = (long) (scale * SONIC_RESAMPLE_BITS);
    time_increment = 1.0 / sample_ratio;
    n_out = (size_t) ((double) n_in * sample_ratio);
    if (n_out > out_cap)
        n_out = out_cap;

    for (t = 0; t < n_out; t++) {
        double time_register = (double) t * time_increment;
        long nn = (long) time_register;
        double frac = scale * (time_register - (double) nn);
        double index_frac = frac * SONIC_RESAMPLE_BITS;
        long offset = (long) index_frac;
        double eta = index_frac - (double) offset;
        float acc = 0.0f;
        long i, i_max, k_max;

        /* resampy's numba loop accumulates into a float32 accumulator
           (y[t] += weight * x[...] rounds per tap); match that exactly. */
        i_max = (nwin - (size_t) offset) / (size_t) index_step;
        if (nn + 1 < i_max)
            i_max = nn + 1;
        for (i = 0; i < i_max; i++) {
            double wgt = win_scale * (g_interp_win[offset + (size_t) i * index_step] +
                                      eta * g_interp_delta[offset + (size_t) i * index_step]);
            acc = (float) ((double) acc + wgt * (double) in[nn - i]);
        }

        frac = scale - frac;
        index_frac = frac * SONIC_RESAMPLE_BITS;
        offset = (long) index_frac;
        eta = index_frac - (double) offset;
        k_max = (nwin - (size_t) offset) / (size_t) index_step;
        if ((long) n_in - nn - 1 < k_max)
            k_max = (long) n_in - nn - 1;
        for (i = 0; i < k_max; i++) {
            double wgt = win_scale * (g_interp_win[offset + (size_t) i * index_step] +
                                      eta * g_interp_delta[offset + (size_t) i * index_step]);
            acc = (float) ((double) acc + wgt * (double) in[nn + i + 1]);
        }
        out[t] = acc;
    }
    return n_out;
}

/* ------------------------------------------------------------------ */
/* window framing                                                      */
/* ------------------------------------------------------------------ */

int
sonic_frame_count(size_t len)
{
    size_t pad;
    if (len < SONIC_FRAME)
        return 1;
    pad = (len - SONIC_FRAME) % SONIC_FRAME;
    if (pad != 0)
        len += SONIC_FRAME - pad;
    return 1 + (int) ((len - SONIC_FRAME) / SONIC_FRAME);
}

int
sonic_center_window(const float *pcm, size_t n, float *out, size_t cap,
                    size_t *out_len)
{
    int nw = sonic_frame_count(n + SONIC_CENTER_PAD);
    size_t total = (size_t) nw * SONIC_FRAME;
    size_t i;

    if (total > cap)
        return 0;
    memset(out, 0, total * sizeof(float));
    /* centered: samples [CENTER_PAD, CENTER_PAD + n); tail pad is zero */
    for (i = 0; i < n && SONIC_CENTER_PAD + i < total; i++)
        out[SONIC_CENTER_PAD + i] = pcm[i];
    if (out_len != 0)
        *out_len = total;
    return nw;
}

/* ------------------------------------------------------------------ */
/* mel frontend                                                        */
/* ------------------------------------------------------------------ */

static void
hann_periodic(float *w, int n)
{
    int i;
    for (i = 0; i < n; i++)
        w[i] = (float) (0.5 - 0.5 * cos(2.0 * M_PI * (double) i / (double) n));
}

/* librosa.filters.mel(...).T, (1025, 256) float32, matching kapre 0.3.6
   (librosa 0.10 slaney / Auditory Toolbox scale). */
void
sonic_mel_filterbank(float *fb, size_t cap)
{
    int f, m;
    const int n_freqs = SONIC_N_FFT / 2 + 1; /* 1025 */
    const int n_mels = SONIC_N_MELS + 2;     /* 258 */
    const double fmin = 0.0, fmax = SONIC_SAMPLE_RATE / 2.0;
    const double f_sp = 200.0 / 3.0;         /* 66.6667 */
    const double min_log_hz = 1000.0;
    const double min_log_mel = min_log_hz / f_sp; /* 15.0 */
    const double log_step = log(6.4) / 27.0;
    double *fftfreqs = (double *) malloc((size_t) n_freqs * sizeof *fftfreqs);
    double *mel_f = (double *) malloc((size_t) n_mels * sizeof *mel_f);
    double *fdiff;
    double min_mel, max_mel;
    size_t needed = (size_t) n_freqs * SONIC_N_MELS;

    if (fftfreqs == 0 || mel_f == 0 || cap < needed) {
        free(fftfreqs);
        free(mel_f);
        return;
    }
    fdiff = (double *) malloc(((size_t) n_mels - 1) * sizeof *fdiff);
    if (fdiff == 0) {
        free(fftfreqs);
        free(mel_f);
        return;
    }
    for (f = 0; f < n_freqs; f++)
        fftfreqs[f] = (double) f * ((double) SONIC_SAMPLE_RATE / 2.0) / (n_freqs - 1);

    /* librosa hz_to_mel / mel_to_hz (slaney) */
    min_mel = fmin / f_sp; /* hz_to_mel(0) */
    max_mel = min_log_mel + log(fmax / min_log_hz) / log_step; /* hz_to_mel(24000) */
    for (m = 0; m < n_mels; m++) {
        double mel = min_mel + (max_mel - min_mel) * (double) m / (n_mels - 1);
        mel_f[m] = mel < min_log_mel ? f_sp * mel
                                     : min_log_hz * exp(log_step * (mel - min_log_mel));
    }
    for (m = 0; m < n_mels - 1; m++)
        fdiff[m] = mel_f[m + 1] - mel_f[m];

    for (m = 0; m < SONIC_N_MELS; m++) {
        double enorm = 2.0 / (mel_f[m + 2] - mel_f[m]);
        for (f = 0; f < n_freqs; f++) {
            double lower = -(mel_f[m] - fftfreqs[f]) / fdiff[m];
            double upper = (mel_f[m + 2] - fftfreqs[f]) / fdiff[m + 1];
            double wgt = lower < upper ? lower : upper;
            if (wgt < 0.0)
                wgt = 0.0;
            fb[(size_t) f * SONIC_N_MELS + (size_t) m] = (float) (wgt * enorm);
        }
    }
    free(fftfreqs);
    free(mel_f);
    free(fdiff);
}

/* radix-2 iterative FFT (n = 2048 is a power of two), float64. */
static void
fft_radix2(const double *re_in, const double *im_in, int n, double *re_out,
           double *im_out)
{
    int i, m, step;
    int rev = 0;

    for (i = 0; i < n; i++) {
        int bit = n >> 1;
        if (i > 0) {
            for (; rev & bit; bit >>= 1)
                rev ^= bit;
            rev ^= bit;
        }
        re_out[rev] = re_in[i];
        im_out[rev] = im_in[i];
    }
    for (step = 1; step < n; step <<= 1) {
        double w_angle = -2.0 * M_PI / (double) (step << 1);
        double w_re = cos(w_angle), w_im = sin(w_angle);
        for (m = 0; m < n; m += step << 1) {
            double cur_re = 1.0, cur_im = 0.0;
            for (i = 0; i < step; i++) {
                int a = m + i, b = m + i + step;
                double x = cur_re * re_out[b] - cur_im * im_out[b];
                double y = cur_re * im_out[b] + cur_im * re_out[b];
                re_out[b] = re_out[a] - x;
                im_out[b] = im_out[a] - y;
                re_out[a] = re_out[a] + x;
                im_out[a] = im_out[a] + y;
                {
                    double nre = cur_re * w_re - cur_im * w_im;
                    cur_im = cur_re * w_im + cur_im * w_re;
                    cur_re = nre;
                }
            }
        }
    }
}

int
sonic_mel(const float *window, float *mel, size_t mel_cap)
{
    const int n_frames = SONIC_MEL_FRAMES;
    float *win = (float *) malloc(SONIC_N_FFT * sizeof *win);
    double *re = (double *) malloc(SONIC_N_FFT * sizeof *re);
    double *im = (double *) malloc(SONIC_N_FFT * sizeof *im);
    double *reo = (double *) malloc(SONIC_N_FFT * sizeof *reo);
    double *imo = (double *) malloc(SONIC_N_FFT * sizeof *imo);
    float *mag = (float *) malloc((size_t) n_frames * (SONIC_N_FFT / 2 + 1) * sizeof *mag);
    float *fb = (float *) malloc((size_t) (SONIC_N_FFT / 2 + 1) * SONIC_N_MELS * sizeof *fb);
    float *tmp = (float *) malloc((size_t) n_frames * SONIC_N_MELS * sizeof *tmp);
    double max_log;
    int f, i;

    if (mel_cap < (size_t) SONIC_N_MELS * n_frames || win == 0 || re == 0 ||
        im == 0 || reo == 0 || imo == 0 || mag == 0 || fb == 0 || tmp == 0) {
        free(win);
        free(re);
        free(im);
        free(reo);
        free(imo);
        free(mag);
        free(fb);
        free(tmp);
        return 0;
    }

    hann_periodic(win, SONIC_N_FFT);
    sonic_mel_filterbank(fb, (size_t) (SONIC_N_FFT / 2 + 1) * SONIC_N_MELS);

    /* STFT + magnitude: ceil(N/242) frames, hann-windowed, |rfft|. */
    for (f = 0; f < n_frames; f++) {
        size_t start = (size_t) f * SONIC_HOP;
        int take = SONIC_N_FFT;
        if (start + (size_t) SONIC_N_FFT > SONIC_FRAME)
            take = (int) (SONIC_FRAME - start);
        if (take < 0)
            take = 0;
        for (i = 0; i < SONIC_N_FFT; i++) {
            double s = i < take ? (double) window[start + (size_t) i] : 0.0;
            re[i] = s * (double) win[i];
        }
        memset(im, 0, SONIC_N_FFT * sizeof *im);
        fft_radix2(re, im, SONIC_N_FFT, reo, imo);
        for (i = 0; i <= SONIC_N_FFT / 2; i++) {
            double m = sqrt(reo[i] * reo[i] + imo[i] * imo[i]);
            mag[(size_t) f * (SONIC_N_FFT / 2 + 1) + (size_t) i] = (float) m;
        }
    }

    /* mel = mag @ filterbank, float32 accumulation (matches numpy/kapre and
       the research TF path, whose near-floor noise character we must share) */
    for (f = 0; f < n_frames; f++) {
        for (i = 0; i < SONIC_N_MELS; i++) {
            float acc = 0.0f;
            int k;
            for (k = 0; k <= SONIC_N_FFT / 2; k++)
                acc = (float) ((double) acc +
                               (double) mag[(size_t) f * (SONIC_N_FFT / 2 + 1) + (size_t) k] *
                                   (double) fb[(size_t) k * SONIC_N_MELS + (size_t) i]);
            mel[(size_t) f * SONIC_N_MELS + (size_t) i] = acc;
        }
    }

    /* decibel (openl3 kapre_v0_1_4_magnitude_to_decibel) */
    max_log = -1e300;
    for (f = 0; f < n_frames * SONIC_N_MELS; f++) {
        double v = (double) mel[f];
        if (v < SONIC_AMIN)
            v = SONIC_AMIN;
        mel[f] = (float) (10.0 * log10(v));
        if ((double) mel[f] > max_log)
            max_log = (double) mel[f];
    }
    for (f = 0; f < n_frames * SONIC_N_MELS; f++) {
        double v = (double) mel[f] - max_log;
        if (v < -SONIC_DYNAMIC_RANGE)
            v = -SONIC_DYNAMIC_RANGE;
        mel[f] = (float) v;
    }

    /* permute (n_frames, n_mels) -> (n_mels, n_frames) via a temp buffer */
    memcpy(tmp, mel, (size_t) n_frames * SONIC_N_MELS * sizeof *mel);
    for (i = 0; i < SONIC_N_MELS; i++)
        for (f = 0; f < n_frames; f++)
            mel[(size_t) i * n_frames + (size_t) f] =
                tmp[(size_t) f * SONIC_N_MELS + (size_t) i];

    free(win);
    free(re);
    free(im);
    free(reo);
    free(imo);
    free(mag);
    free(fb);
    free(tmp);
    return n_frames;
}

/* ------------------------------------------------------------------ */
/* pooling / album aggregation                                         */
/* ------------------------------------------------------------------ */

int
sonic_pool_mean_norm(const float *emb, size_t n, size_t dims, float *out)
{
    double norm;
    size_t i, j;
    if (n == 0 || emb == 0 || out == 0)
        return 0;
    /* per-window row normalize then mean */
    memset(out, 0, dims * sizeof(float));
    for (i = 0; i < n; i++) {
        double row_norm = 0.0;
        for (j = 0; j < dims; j++)
            row_norm += (double) emb[i * dims + j] * (double) emb[i * dims + j];
        row_norm = sqrt(row_norm);
        if (row_norm == 0.0)
            row_norm = 1.0;
        for (j = 0; j < dims; j++)
            out[j] += (float) ((double) emb[i * dims + j] / row_norm);
    }
    for (j = 0; j < dims; j++)
        out[j] /= (float) n;
    /* L2 normalize the mean */
    norm = 0.0;
    for (j = 0; j < dims; j++)
        norm += (double) out[j] * (double) out[j];
    norm = sqrt(norm);
    if (norm == 0.0 || !isfinite(norm))
        return 0;
    for (j = 0; j < dims; j++)
        out[j] = (float) ((double) out[j] / norm);
    return 1;
}

size_t
sonic_album_equal(const float *vecs, const int *present, size_t n, size_t dims,
                  float *out)
{
    double norm;
    size_t i, j, contributing = 0;

    if (vecs == 0 || out == 0)
        return 0;
    memset(out, 0, dims * sizeof(float));
    for (i = 0; i < n; i++) {
        if (present != 0 && !present[i])
            continue;
        for (j = 0; j < dims; j++)
            out[j] += vecs[i * dims + j];
        contributing++;
    }
    if (contributing == 0)
        return 0;
    for (j = 0; j < dims; j++)
        out[j] /= (float) contributing;
    norm = 0.0;
    for (j = 0; j < dims; j++)
        norm += (double) out[j] * (double) out[j];
    norm = sqrt(norm);
    if (norm == 0.0 || !isfinite(norm))
        return 0;
    for (j = 0; j < dims; j++)
        out[j] = (float) ((double) out[j] / norm);
    return contributing;
}
