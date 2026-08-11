"""Deterministic window-to-track pooling: L2 normalization, cosine
similarity, mean / mean-norm / robust-mean aggregation, and the
energy-based silence gate.

All operations are pure numpy and float32-deterministic given the same
input, which is the property the MusicPack profile relies on.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Optional

import numpy as np

from sonic_profile import (
    POOL_MEAN,
    POOL_MEAN_NORM,
    POOL_ROBUST_MEAN,
    SilenceParams,
)


@dataclass
class WindowEmbeddings:
    """Sequence of per-window embeddings over time, as produced by an
    analyzer. ``timestamps`` are the window centre times in seconds and are
    the basis for the silence gate."""

    embeddings: np.ndarray  # (n, dim) float32
    timestamps: np.ndarray  # (n,) float64 centre times (seconds)

    def __post_init__(self):
        self.embeddings = np.asarray(self.embeddings, dtype=np.float32)
        self.timestamps = np.asarray(self.timestamps, dtype=np.float64)
        if self.embeddings.ndim != 2:
            raise ValueError("embeddings must be 2-D (n_windows, dim)")
        if len(self.timestamps) != len(self.embeddings):
            raise ValueError("timestamps and embeddings must have equal length")

    @property
    def n(self) -> int:
        return len(self.embeddings)

    @property
    def available(self) -> bool:
        """True when there is at least one usable window embedding."""
        return self.n > 0

    @property
    def norm(self) -> np.ndarray:
        return np.linalg.norm(self.embeddings, axis=1)


def l2_normalize(v: np.ndarray) -> Optional[np.ndarray]:
    """Unit-normalize ``v``. Returns None for a zero or non-finite vector."""
    v = np.asarray(v, dtype=np.float32)
    n = float(np.linalg.norm(v))
    if n == 0.0 or not np.isfinite(n):
        return None
    return v / n


def cosine(a: np.ndarray, b: np.ndarray) -> float:
    """Cosine similarity in [0, 1] for unit-normalized inputs; degenerate
    (zero) vectors compare as 0.0."""
    a = np.asarray(a, dtype=np.float32)
    b = np.asarray(b, dtype=np.float32)
    na = float(np.linalg.norm(a))
    nb = float(np.linalg.norm(b))
    if na == 0.0 or nb == 0.0 or not (np.isfinite(na) and np.isfinite(nb)):
        return 0.0
    return float(np.dot(a, b) / (na * nb))


def _normalize_rows(E: np.ndarray) -> np.ndarray:
    norms = np.linalg.norm(E, axis=1, keepdims=True)
    norms[norms == 0.0] = 1.0
    return E / norms


def _robust_mean(E: np.ndarray, trim: float) -> Optional[np.ndarray]:
    """Mean over the windows after discarding the ``trim`` fraction with the
    largest distance from the mean. ``trim <= 0`` disables trimming."""
    if trim > 0.0 and E.shape[0] > 1:
        mean = E.mean(axis=0)
        dist = np.linalg.norm(E - mean, axis=1)
        keep_n = int(E.shape[0] * (1.0 - trim))
        if keep_n < 1:
            return None
        idx = np.argsort(dist, kind="stable")[:keep_n]
        E = E[idx]
    if E.shape[0] == 0:
        return None
    return E.mean(axis=0)


def pool_windows(
    windows: WindowEmbeddings,
    strategy: str,
    robust_trim: float = 0.0,
    keep: Optional[np.ndarray] = None,
) -> Optional[np.ndarray]:
    """Aggregate window embeddings into one unit-normalized track embedding.

    ``keep`` is an optional boolean mask (e.g. from the silence gate); masked
    windows are excluded first. Returns None when no window survives — the
    explicit "no embedding" state, never a fabricated vector.
    """
    E = windows.embeddings
    if keep is not None:
        E = E[keep]
    if E.shape[0] == 0:
        return None

    if strategy == POOL_MEAN_NORM:
        v = _normalize_rows(E).mean(axis=0)
    elif strategy == POOL_MEAN:
        v = E.mean(axis=0)
    elif strategy == POOL_ROBUST_MEAN:
        v = _robust_mean(E, robust_trim)
        if v is None:
            return None
    else:
        raise ValueError("unknown pooling strategy: %r" % strategy)

    return l2_normalize(v)


def window_rms_db(
    pcm: np.ndarray,
    sample_rate: int,
    timestamps: np.ndarray,
    window_seconds: float,
) -> np.ndarray:
    """Per-window RMS in dBFS, aligned with ``timestamps`` (each window spans
    ``window_seconds`` centred on its timestamp)."""
    pcm = np.asarray(pcm, dtype=np.float32)
    half = max(1, int(window_seconds * sample_rate / 2))
    n = len(pcm)
    out = np.empty(len(timestamps), dtype=np.float32)
    for i, t in enumerate(timestamps):
        c = int(round(t * sample_rate))
        lo = max(0, c - half)
        hi = min(n, c + half)
        seg = pcm[lo:hi]
        rms = float(np.sqrt(np.mean(seg * seg))) if seg.size else 0.0
        out[i] = 20.0 * math.log10(rms + 1e-12)
    return out


def silence_mask(rms_db: np.ndarray, params: SilenceParams) -> Optional[np.ndarray]:
    """Boolean keep-mask for the silence gate. Returns None when the gate is
    disabled (all windows kept). Absolute threshold is dBFS; when
    ``relative_to_median`` is set the effective threshold is
    ``median(rms) + threshold_db``."""
    if not params.enabled:
        return None
    rms = np.asarray(rms_db, dtype=np.float64)
    if params.relative_to_median:
        threshold = float(np.median(rms)) + float(params.threshold_db)
    else:
        threshold = float(params.threshold_db)
    return rms > threshold


def apply_silence_and_pool(
    windows: WindowEmbeddings,
    pcm: np.ndarray,
    sample_rate: int,
    silence: SilenceParams,
    strategy: str,
    robust_trim: float = 0.0,
) -> Optional[np.ndarray]:
    """Pool ``windows`` into a track embedding, applying the silence gate
    using ``pcm``. Convenience wrapper used by analyzers/benchmark."""
    if not windows.available:
        return None
    keep = None
    if silence.enabled:
        rms = window_rms_db(pcm, sample_rate, windows.timestamps, silence.window_seconds)
        keep = silence_mask(rms, silence)
        if keep is not None and not bool(np.any(keep)):
            return None
    return pool_windows(windows, strategy, robust_trim=robust_trim, keep=keep)
