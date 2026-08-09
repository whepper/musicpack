"""Album aggregation: equal vs duration weighting."""

import numpy as np

from album import album_embedding
from pooling import l2_normalize


def _v(seed: int, dim: int = 8) -> np.ndarray:
    rng = np.random.RandomState(seed)
    return l2_normalize(rng.randn(dim).astype(np.float32))


def test_equal_weighting_is_mean():
    tracks = [
        ("a", _v(1), 60.0),
        ("b", _v(2), 60.0),
        ("c", _v(3), 60.0),
    ]
    emb = album_embedding(tracks, "equal")
    assert emb is not None
    assert abs(np.linalg.norm(emb) - 1.0) < 1e-4
    mean = l2_normalize(np.mean([t[1] for t in tracks], axis=0))
    np.testing.assert_allclose(emb, mean, atol=1e-6)


def test_duration_weighting_biases_to_long_tracks():
    long = _v(11)
    short = _v(12)
    tracks = [
        ("long", long, 1000.0),
        ("short1", short, 1.0),
        ("short2", short, 1.0),
    ]
    emb = album_embedding(tracks, "duration")
    assert emb is not None
    # duration-weighted album should be much closer to the long track than
    # the equal-weighted one
    eq = album_embedding(tracks, "equal")
    from pooling import cosine
    assert cosine(emb, long) > cosine(eq, long)


def test_duration_weights_are_proportional():
    a = _v(21)
    b = _v(22)
    # 3:1 duration ratio should make the album closer to a than 1:1 would
    t1 = [("a", a, 90.0), ("b", b, 30.0)]
    t2 = [("a", a, 60.0), ("b", b, 60.0)]
    e1 = album_embedding(t1, "duration")
    e2 = album_embedding(t2, "duration")
    from pooling import cosine
    assert cosine(e1, a) > cosine(e2, a)


def test_skips_tracks_without_embedding():
    tracks = [("a", None, 60.0), ("b", _v(31), 60.0)]
    emb = album_embedding(tracks, "equal")
    assert emb is not None
    np.testing.assert_allclose(emb, tracks[1][1], atol=1e-6)


def test_empty_album_is_none():
    assert album_embedding([("a", None, 60.0)], "equal") is None
    assert album_embedding([], "equal") is None


def test_unknown_strategy_raises():
    try:
        album_embedding([("a", _v(1), 60.0)], "bogus")
    except ValueError:
        return
    raise AssertionError("expected ValueError")


def test_albums_strategies_deterministic():
    tracks = [("a", _v(41), 100.0), ("b", _v(42), 20.0), ("c", _v(43), 55.0)]
    np.testing.assert_array_equal(
        album_embedding(tracks, "equal"), album_embedding(tracks, "equal")
    )
    np.testing.assert_array_equal(
        album_embedding(tracks, "duration"), album_embedding(tracks, "duration")
    )
