"""Deterministic album-level aggregation from per-track embeddings.

Two strategies are benchmarked:
  * equal weighting     — album = normalized mean of track embeddings
  * duration weighting  — album = normalized duration-weighted mean

Equal weighting may represent an album as an authored collection better
than duration weighting (which over-emphasizes very long tracks); this
module implements both, and the benchmark decides.
"""

from __future__ import annotations

from typing import Dict, List, Optional, Tuple

import numpy as np

from pooling import l2_normalize

ALBUM_EQUAL = "equal"
ALBUM_DURATION = "duration"
ALBUM_STRATEGIES = (ALBUM_EQUAL, ALBUM_DURATION)

TrackEmbedding = Tuple[str, np.ndarray, float]  # (track_id, vector, duration_s)


def album_embedding(
    tracks: List[TrackEmbedding], strategy: str
) -> Optional[np.ndarray]:
    """Aggregate available track embeddings into one unit-normalized album
    embedding. Tracks without an embedding are skipped. Returns None when no
    track contributes (explicit no-embedding)."""
    available = [t for t in tracks if t[1] is not None]
    if not available:
        return None

    if strategy == ALBUM_EQUAL:
        mean = np.mean([t[1] for t in available], axis=0)
    elif strategy == ALBUM_DURATION:
        weights = np.array([max(float(t[2]), 0.0) for t in available], dtype=np.float32)
        total = float(weights.sum())
        if total <= 0.0:
            return None
        mean = np.average(
            np.stack([t[1] for t in available]), axis=0, weights=weights
        )
    else:
        raise ValueError("unknown album strategy: %r" % strategy)

    return l2_normalize(mean)
