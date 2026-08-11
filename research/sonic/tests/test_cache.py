"""Two-level embedding cache: round-trips and identity invalidation."""

import numpy as np

from cache import MISSING, Cache
from pooling import WindowEmbeddings
from sonic_profile import PoolingParams, Profile, SilenceParams


def _profile(**kw) -> Profile:
    p = Profile()
    for k, v in kw.items():
        object.__setattr__(p, k, v)
    return p


def test_windows_roundtrip(tmp_path):
    cache = Cache(tmp_path)
    w = WindowEmbeddings(np.random.RandomState(0).randn(5, 8).astype(np.float32), np.arange(5) + 0.5)
    cache.store_windows("aaa", "modelkey", w)
    got = cache.load_windows("aaa", "modelkey")
    assert got is not None
    np.testing.assert_array_equal(got.embeddings, w.embeddings)
    np.testing.assert_array_equal(got.timestamps, w.timestamps)


def test_windows_miss(tmp_path):
    cache = Cache(tmp_path)
    assert cache.load_windows("nope", "key") is None


def test_track_roundtrip_and_none_sentinel(tmp_path):
    cache = Cache(tmp_path)
    p = _profile()
    vec = np.ones(8, dtype=np.float32) / np.sqrt(8)
    assert cache.load_track("sha", p) is MISSING
    cache.store_track("sha", p, vec)
    got = cache.load_track("sha", p)
    assert got is not MISSING
    np.testing.assert_allclose(got, vec, atol=1e-7)
    cache.store_track("sha2", p, None)
    assert cache.load_track("sha2", p) is None  # cached no-embedding


def test_cache_identity_changes_with_audio(tmp_path):
    cache = Cache(tmp_path)
    p = _profile()
    assert cache.track_path("aaa", p) != cache.track_path("bbb", p)
    assert cache.window_path("aaa", "k") != cache.window_path("bbb", "k")


def test_cache_identity_changes_with_profile(tmp_path):
    cache = Cache(tmp_path)
    base = _profile()
    assert cache.track_path("aaa", base) != cache.track_path(
        "aaa", _profile(pooling=PoolingParams(hop_seconds=0.5))
    )
    assert cache.track_path("aaa", base) != cache.track_path(
        "aaa", _profile(pooling=PoolingParams(silence=SilenceParams(enabled=True)))
    )
    assert cache.track_path("aaa", base) != cache.track_path(
        "aaa", _profile(model_input_repr="mel128")
    )
    assert cache.track_path("aaa", base) != cache.track_path(
        "aaa", _profile(model_weights_sha256="deadbeef")
    )
    # window cache follows the model key, not pooling
    assert cache.window_path("aaa", base.model_key()) == cache.window_path(
        "aaa", _profile(pooling=PoolingParams(hop_seconds=0.5)).model_key()
    )
    assert cache.window_path("aaa", base.model_key()) != cache.window_path(
        "aaa", _profile(model_input_repr="mel128").model_key()
    )


def test_corrupt_cache_file_is_miss(tmp_path):
    cache = Cache(tmp_path)
    path = cache.window_path("aaa", "k")
    path.parent.mkdir(parents=True)
    path.write_bytes(b"garbage")
    assert cache.load_windows("aaa", "k") is None
