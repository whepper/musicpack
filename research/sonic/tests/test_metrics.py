"""Quantitative metrics on synthetic clustered embeddings."""

import numpy as np

from metrics import genre_purity_at_k, nearest, same_group_at_k, summary_at_k


def _clustered(n_clusters=3, per=5, dim=16, seed=0):
    """Tight clusters around distinct random centroids."""
    rng = np.random.RandomState(seed)
    centroids = rng.randn(n_clusters, dim).astype(np.float32)
    centroids /= np.linalg.norm(centroids, axis=1, keepdims=True)
    E = []
    for c in centroids:
        for _ in range(per):
            v = (c + rng.randn(dim).astype(np.float32) * 0.01)
            E.append(v / np.linalg.norm(v))
    return np.stack(E)


def test_same_group_at_k_perfect_clusters():
    E = _clustered()
    groups = [f"g{i // 5}" for i in range(len(E))]
    score = same_group_at_k(E, groups, k=4)
    assert score == 1.0


def test_same_group_respects_k():
    E = _clustered()
    groups = [f"g{i // 5}" for i in range(len(E))]
    assert same_group_at_k(E, groups, k=1) == 1.0


def test_same_group_zero_for_unrelated():
    rng = np.random.RandomState(3)
    E = rng.randn(10, 8).astype(np.float32)
    E /= np.linalg.norm(E, axis=1, keepdims=True)
    groups = [str(i) for i in range(10)]  # every group unique
    assert same_group_at_k(E, groups, k=3) == 0.0


def test_genre_purity():
    E = _clustered()
    # first cluster shares genre {A,B}, others empty/unique
    genres = [frozenset({"A", "B"})] * 5 + [frozenset()] * 10
    score = genre_purity_at_k(E, genres, k=4)
    assert score > 0.0  # cluster 0 members find each other


def test_nearest_excludes_self():
    E = _clustered(n_clusters=1, per=3)
    labels = [f"t{i}" for i in range(len(E))]
    res = nearest(E, labels, 0, k=2)
    assert {r["label"] for r in res} == {"t1", "t2"}
    assert all(r["similarity"] > 0.9 for r in res)
    assert res[0]["similarity"] >= res[1]["similarity"]


def test_summary_shapes():
    E = _clustered()
    groups = [f"g{i // 5}" for i in range(len(E))]
    s = summary_at_k(E, groups, groups, [frozenset()] * len(E), k=4)
    assert set(s) == {"same_album@4", "same_artist@4", "genre_purity@4"}
    assert s["same_album@4"] == 1.0
