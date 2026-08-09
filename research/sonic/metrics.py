"""Quantitative evaluation metrics (diagnostics, not ground truth).

Perfect 'sounds similar' has no objective definition, so these proxies are
reported per pooling/hop/album strategy to compare candidates — never
treated as the desired end result. Genres are evaluation labels only and are
never used to train anything.
"""

from __future__ import annotations

import math
from typing import Dict, List, Sequence

import numpy as np


def _cosine_matrix(embeddings: np.ndarray) -> np.ndarray:
    E = np.asarray(embeddings, dtype=np.float32)
    norms = np.linalg.norm(E, axis=1, keepdims=True)
    norms[norms == 0.0] = 1.0
    sim = (E / norms) @ (E / norms).T
    np.fill_diagonal(sim, -np.inf)  # exclude self
    return sim


def _neighbors(sim: np.ndarray, k: int) -> np.ndarray:
    k = min(k, sim.shape[1] - 1)
    return np.argsort(-sim, axis=1, kind="stable")[:, :k]


def same_group_at_k(
    embeddings: np.ndarray, groups: Sequence[str], k: int = 10
) -> float:
    """Mean fraction of the k nearest neighbours that share the track's
    group label (album, artist, ...)."""
    sim = _cosine_matrix(embeddings)
    neigh = _neighbors(sim, k)
    total = hits = 0.0
    for i, row in enumerate(neigh):
        for j in row:
            total += 1.0
            if groups[i] == groups[int(j)]:
                hits += 1.0
    return hits / total if total else float("nan")


def genre_purity_at_k(
    embeddings: np.ndarray, genre_sets: Sequence[frozenset], k: int = 10
) -> float:
    """Mean Jaccard overlap between a track's multi-value genre set and each
    of its k nearest neighbours (0 when either set is empty)."""
    sim = _cosine_matrix(embeddings)
    neigh = _neighbors(sim, k)
    total = score = 0.0
    for i, row in enumerate(neigh):
        gi = genre_sets[i]
        for j in row:
            gj = genre_sets[int(j)]
            total += 1.0
            if gi and gj:
                score += len(gi & gj) / len(gi | gj)
    return score / total if total else float("nan")


def album_coherence_at_k(
    album_embeddings: np.ndarray, album_artists: Sequence[str], k: int = 10
) -> float:
    """Do albums by the same artist cluster? Same-artist fraction among the
    k nearest album neighbours."""
    return same_group_at_k(album_embeddings, album_artists, k)


def summary_at_k(
    embeddings: np.ndarray,
    albums: Sequence[str],
    artists: Sequence[str],
    genre_sets: Sequence[frozenset],
    k: int = 10,
) -> Dict[str, float]:
    return {
        "same_album@%d" % k: same_group_at_k(embeddings, albums, k),
        "same_artist@%d" % k: same_group_at_k(embeddings, artists, k),
        "genre_purity@%d" % k: genre_purity_at_k(embeddings, genre_sets, k),
    }


def nearest(
    embeddings: np.ndarray, labels: Sequence[str], query: int, k: int = 10
) -> List[Dict[str, object]]:
    """Ordered nearest neighbours for one track (for human-eval reports)."""
    sim = _cosine_matrix(embeddings)
    idx = _neighbors(sim, k)[query]
    return [
        {"rank": r + 1, "label": labels[int(i)], "similarity": round(float(sim[query, i]), 5)}
        for r, i in enumerate(idx)
    ]
