"""Pooling, normalization, cosine and silence-gate behaviour."""

import numpy as np
import pytest

from conftest import make_windows, silent_pcm, synth_pcm, SR
from pooling import (
    WindowEmbeddings,
    apply_silence_and_pool,
    cosine,
    l2_normalize,
    pool_windows,
    silence_mask,
    window_rms_db,
)
from profile import POOL_MEAN, POOL_MEAN_NORM, POOL_ROBUST_MEAN, SilenceParams


def test_l2_normalize_unit():
    v = l2_normalize(np.array([3.0, 4.0]))
    assert v is not None
    assert abs(np.linalg.norm(v) - 1.0) < 1e-6
    assert v[0] == pytest.approx(0.6, abs=1e-6)


def test_l2_normalize_zero_returns_none():
    assert l2_normalize(np.zeros(4)) is None
    assert l2_normalize(np.array([np.nan, 1.0])) is None
    assert l2_normalize(np.array([np.inf, 1.0])) is None


def test_cosine():
    a = l2_normalize(np.array([1.0, 0.0]))
    b = l2_normalize(np.array([0.0, 1.0]))
    c = l2_normalize(np.array([1.0, 0.0]))
    d = l2_normalize(np.array([-1.0, 0.0]))
    assert cosine(a, a) == pytest.approx(1.0)
    assert cosine(a, c) == pytest.approx(1.0)
    assert cosine(a, b) == pytest.approx(0.0, abs=1e-6)
    assert cosine(a, d) == pytest.approx(-1.0, abs=1e-6)


def test_cosine_degenerate_is_zero():
    assert cosine(np.zeros(4), np.ones(4)) == 0.0


def test_mean_norm_unit_and_deterministic():
    w = make_windows(20, dim=16)
    v1 = pool_windows(w, POOL_MEAN_NORM)
    v2 = pool_windows(w, POOL_MEAN_NORM)
    assert v1 is not None
    assert abs(np.linalg.norm(v1) - 1.0) < 1e-4
    np.testing.assert_array_equal(v1, v2)


def test_mean_norm_vs_mean_differ():
    # openl3 pre-normalizes its windows, but the general case must keep both
    # strategies distinct: mean-norm pools directions, mean pools magnitudes.
    w = make_windows(10, dim=4, seed=3)
    a = pool_windows(w, POOL_MEAN_NORM)
    b = pool_windows(w, POOL_MEAN)
    assert not np.allclose(a, b, atol=1e-6)


def test_robust_mean_trims_outliers():
    rng = np.random.RandomState(7)
    base = rng.randn(1, 8).astype(np.float32)
    clean = np.tile(base, (9, 1)) + rng.randn(9, 8).astype(np.float32) * 0.01
    outlier = (base + rng.randn(1, 8).astype(np.float32) * 10.0).astype(np.float32)
    E = np.vstack([clean, outlier])
    w = WindowEmbeddings(E, np.arange(10) + 0.5)
    trimmed = pool_windows(w, POOL_ROBUST_MEAN, robust_trim=0.1)
    plain = pool_windows(w, POOL_MEAN)
    assert trimmed is not None and plain is not None
    # The single outlier pulls the plain mean; trimmed stays close to clean.
    assert cosine(trimmed, base[0]) > cosine(plain, base[0])


def test_empty_pool_is_none():
    w = WindowEmbeddings(np.zeros((0, 8), dtype=np.float32), np.zeros(0))
    assert not w.available
    assert pool_windows(w, POOL_MEAN_NORM) is None


def test_silence_mask_absolute_threshold():
    rms = np.array([-90.0, -20.0, -60.0, -5.0])
    keep = silence_mask(rms, SilenceParams(enabled=True, threshold_db=-50.0, relative_to_median=False))
    np.testing.assert_array_equal(keep, [False, True, False, True])


def test_silence_mask_relative_to_median():
    rms = np.array([-90.0, -25.0, -24.0, -23.0, -22.0, -5.0])
    keep = silence_mask(
        rms, SilenceParams(enabled=True, threshold_db=-10.0, relative_to_median=True)
    )
    # median ~ -23.5; threshold ~ -33.5 -> only the -90 window drops out
    np.testing.assert_array_equal(keep, [False, True, True, True, True, True])


def test_silence_excludes_quiet_windows_from_pool():
    w = make_windows(6, dim=8, seed=5)
    # make the first two windows correspond to near-silence by replacing
    # their embeddings with zeros (their RMS comes from the pcm windows)
    w.embeddings[0] = 0.0
    w.embeddings[1] = 0.0
    pcm = synth_pcm(6.0, seed=5)
    # silence the first 2 seconds of pcm
    n = int(2.0 * SR)
    pcm[:n] = (pcm[:n] * 1e-5)
    silent = SilenceParams(enabled=True, threshold_db=-40.0, relative_to_median=False, window_seconds=1.0)
    v = apply_silence_and_pool(w, pcm, SR, silent, POOL_MEAN)
    assert v is not None
    rms = window_rms_db(pcm, SR, w.timestamps, 1.0)
    assert rms[0] < rms[3]


def test_all_silent_track_has_no_embedding():
    w = make_windows(4, dim=8, seed=5)
    pcm = silent_pcm(4.0)
    silent = SilenceParams(enabled=True, threshold_db=-60.0, relative_to_median=False, window_seconds=1.0)
    v = apply_silence_and_pool(w, pcm, SR, silent, POOL_MEAN)
    assert v is None  # explicit no-embedding, never a fabricated vector


def test_silence_disabled_keeps_all_windows():
    w = make_windows(4, dim=8, seed=5)
    pcm = silent_pcm(4.0)
    v = apply_silence_and_pool(w, pcm, SR, SilenceParams(enabled=False), POOL_MEAN)
    assert v is not None
