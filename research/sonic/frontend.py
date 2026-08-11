"""OpenL3 mel256 kapre frontend reimplemented in numpy.

This is the deterministic, model-free reference for the
`musicpack-sonic-openl3-v1` profile frontend, matching openl3 0.4.0 +
kapre 0.3.6 exactly:

- openl3.core._center_audio / _pad_audio (window framing at hop 1.0 s)
- kapre STFT: tf.signal.stft(frame_length 2048, frame_step 242, fft_length
  2048, window hann (periodic), pad_end=True) -> ceil(N/242) frames
- kapre Magnitude: |stft|
- kapre ApplyFilterbank: librosa.filters.mel(sr 48000, n_fft 2048, n_mels
  256, fmin 0, fmax None, htk False, norm 'slaney') transposed
- openl3.models.kapre_v0_1_4_magnitude_to_decibel: 10*log10(max(x, 1e-10))
  - max, floored at -80
- Permute((2,1,3)) -> (n_mels=256, n_frames, 1)

The production C frontend in sonic/ is ported from this module and verified
against it (and against the research harness) by sonic/compat_measure.py.

Model-free: numpy only (librosa is used lazily only for the mel matrix).
"""

import numpy as np

HOP = 242
N_FFT = 2048
N_MELS = 256
SR = 48000
FRAME = SR  # 1.0 s window
CENTER_PAD = FRAME // 2  # 24000 zero samples
AMIN = 1e-10
DYNAMIC_RANGE = 80.0


def hann_periodic(n=N_FFT):
    """tf.signal.hann_window(periodic=True): 0.5 - 0.5*cos(2*pi*k/n)."""
    return (0.5 - 0.5 * np.cos(2.0 * np.pi * np.arange(n) / n)).astype(np.float32)


def center_pad(audio):
    """openl3.core._center_audio: prepend 24000 zeros."""
    return np.pad(audio, (CENTER_PAD, 0), mode="constant", constant_values=0)


def frame_pad(audio):
    """openl3.core._pad_audio at the window level (hop = 48000), then count
    windows. Returns (padded_audio, n_windows)."""
    n = audio.size
    if n < FRAME:
        pad = FRAME - n
    else:
        pad = int(np.ceil((n - FRAME) / float(FRAME))) * FRAME - (n - FRAME)
    if pad > 0:
        audio = np.pad(audio, (0, pad), mode="constant", constant_values=0)
    n_windows = 1 + (audio.size - FRAME) // FRAME
    return audio, n_windows


def windows(audio):
    """(N,) float32 -> ((n_windows, 48000) float32 windows, timestamps)."""
    audio, n_windows = frame_pad(audio)
    w = np.stack([audio[i * FRAME:(i + 1) * FRAME] for i in range(n_windows)])
    return w, np.arange(n_windows) * 1.0  # hop_seconds = 1.0


def stft_magnitude(window):
    """kapre STFT (hann periodic, pad_end -> ceil(N/242) frames) + Magnitude
    (|stft|). (N,) float32 -> (ceil(N/242), 1025) float32."""
    n_frames = int(np.ceil(window.size / float(HOP)))
    w = hann_periodic()
    spec = np.zeros((n_frames, N_FFT // 2 + 1), np.float32)
    for f in range(n_frames):
        start = f * HOP
        seg = np.zeros(N_FFT, np.float32)
        take = min(N_FFT, window.size - start)
        if take > 0:
            seg[:take] = window[start:start + take]
        seg = seg * w
        spec[f] = np.abs(np.fft.rfft(seg, N_FFT)).astype(np.float32)
    return spec


def mel_filterbank():
    """librosa.filters.mel(...).T, (1025, 256) float32, matching kapre."""
    import librosa
    return librosa.filters.mel(
        sr=SR, n_fft=N_FFT, n_mels=N_MELS, fmin=0.0, fmax=None, htk=False, norm="slaney"
    ).T.astype(np.float32)


def frontend(window):
    """(48000,) float32 -> (256, n_frames, 1) float32 mel for one window."""
    mag = stft_magnitude(window)
    mel = mag @ mel_filterbank()
    log = 10.0 * np.log10(np.maximum(mel, AMIN))
    out = np.maximum(log - log.max(), -DYNAMIC_RANGE)
    return out.T[..., None].astype(np.float32)


def track_embedding(windows_mel):
    """mean-norm pooling: per-window L2 row normalize -> mean -> L2. Input
    (n_windows, 256, n_frames, 1). Returns (512,) float32 unit vector."""
    n = windows_mel.shape[0]
    if n == 0:
        return None
    E = windows_mel.reshape(n, -1)
    norms = np.linalg.norm(E, axis=1, keepdims=True)
    norms[norms == 0.0] = 1.0
    v = (E / norms).mean(axis=0)
    nrm = float(np.linalg.norm(v))
    if nrm == 0.0 or not np.isfinite(nrm):
        return None
    return (v / nrm).astype(np.float32)
