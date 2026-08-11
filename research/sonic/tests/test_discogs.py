"""Discogs-EffNet comparator tests.

The patch-count formula test is model-free. Real-model tests are marked
``discogs`` and skipped unless essentia-tensorflow is installed (optional,
evaluation-only; never a MusicPack dependency).
"""

import math

import numpy as np
import pytest

import conftest
from analyzers.essentia_discogs import (
    EMBEDDING_DIM,
    MODELS,
    PATCH_HOP_SECONDS,
    PATCH_WINDOW_SECONDS,
    DiscogsAnalyzer,
)
from sonic_profile import Profile, PoolingParams

MODEL_DIR = conftest.REPO_ROOT / "research/sonic/models"


def _analyzer(variant="multi"):
    return DiscogsAnalyzer(
        Profile(model="discogs-effnet", model_variant=variant,
                model_content="discogs", model_input_repr="mel96",
                model_embedding_size=EMBEDDING_DIM, model_sample_rate=16000,
                frontend="essentia", center=False,
                pooling=PoolingParams(hop_seconds=1.0, window_seconds=2.096)),
        MODEL_DIR,
    )


def n_patches(n16):
    n_frames = 1 + math.ceil((n16 - 256) / 256)
    return max(0, 1 + math.floor((n_frames - 131) / 61))


@pytest.mark.parametrize("n16,want", [
    (16000, 0), (32000, 0), (48000, 1), (80000, 3),
    (128000, 7), (160000, 9), (480000, 29),
])
def test_patch_formula(n16, want):
    assert n_patches(n16) == want


def test_model_constants():
    assert EMBEDDING_DIM == 1280
    assert set(MODELS) == {"multi", "release"}
    for fname, sha in MODELS.values():
        assert fname.endswith(".pb")
        assert len(sha) == 64
    assert PATCH_WINDOW_SECONDS > PATCH_HOP_SECONDS


def test_profile_validation():
    with pytest.raises(ValueError):
        _analyzer("bogus")
    bad = Profile(model="discogs-effnet", model_variant="multi",
                  model_embedding_size=2048)
    with pytest.raises(ValueError):
        DiscogsAnalyzer(bad, MODEL_DIR)


def test_window_constants():
    # 131 mel frames * 256 samples @ 16k
    assert abs(PATCH_WINDOW_SECONDS - 2.096) < 1e-3
    # 61 mel frames * 256 samples @ 16k
    assert abs(PATCH_HOP_SECONDS - 0.976) < 1e-3


pytestmark_model = pytest.mark.usefixtures


@conftest.discogs
def test_real_model_shape_and_determinism():
    sr = 16000
    rng = np.random.RandomState(0)
    t = np.arange(int(10 * sr)) / sr
    pcm = (0.5 * np.sin(2 * np.pi * 220 * t)
           + 0.2 * np.sin(2 * np.pi * 440 * t)
           + 0.05 * rng.randn(len(t))).astype(np.float32)
    a = _analyzer("multi")
    assert a.available()
    w = a.window_embeddings(pcm, sr)
    assert w.n == 9
    assert w.embeddings.shape == (9, 1280)
    assert np.all(np.isfinite(w.embeddings))
    w2 = a.window_embeddings(pcm, sr)
    np.testing.assert_array_equal(w.embeddings, w2.embeddings)
    # release variant same frame count
    assert _analyzer("release").window_embeddings(pcm, sr).n == 9


@conftest.discogs
def test_real_model_short_audio():
    sr = 16000
    pcm = np.zeros(int(1.0 * sr), dtype=np.float32)
    w = _analyzer().window_embeddings(pcm, sr)
    assert w.n == 0
    assert not w.available


@conftest.discogs
def test_model_identity_license():
    ident = _analyzer().model_identity()
    assert ident["weights_cc_license"] == "CC BY-NC-SA 4.0"
    assert ident["name"] == "discogs-multi"
