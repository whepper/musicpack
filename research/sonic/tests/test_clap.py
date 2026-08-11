"""LAION-CLAP music checkpoint tests (decision-gate candidate).

The segment-count formula tests are model-free. Real-model tests are marked
``clap`` and skipped unless torch + transformers are installed (separate
venv; Apache-2.0 checkpoint, never committed).
"""

import numpy as np
import pytest

import conftest
from analyzers.clap import (
    EMBEDDING_DIM,
    MIN_FINAL_SECONDS,
    MODEL_ID,
    SAMPLE_RATE,
    SEGMENT_SECONDS,
    WEIGHTS_SHA256,
    ClapAnalyzer,
    segment_count,
)
from sonic_profile import Profile, PoolingParams

MODEL_DIR = conftest.REPO_ROOT / "research/sonic/models"


def _analyzer():
    return ClapAnalyzer(
        Profile(model="clap", model_variant="music", model_content="music",
                model_input_repr="mel64", model_embedding_size=512,
                model_sample_rate=48000, frontend="swin", center=False,
                pooling=PoolingParams(hop_seconds=10.0, window_seconds=10.0)),
        MODEL_DIR,
    )


def test_constants():
    assert MODEL_ID == "laion/larger_clap_music"
    assert EMBEDDING_DIM == 512
    assert SEGMENT_SECONDS == 10.0
    assert MIN_FINAL_SECONDS == 5.0
    assert SAMPLE_RATE == 48000
    assert len(WEIGHTS_SHA256) == 64


@pytest.mark.parametrize("samples,want", [
    (48000 * 10, 1),    # exactly 10 s
    (48000 * 9, 1),     # 9 s trailing >= 5 s -> padded segment
    (48000 * 4, 0),     # < 5 s -> no embedding
    (48000 * 25, 3),    # 10+10 + 5 s trailing
    (48000 * 30, 3),    # exactly 30 s
    (48000 * 44, 4),    # 40 + 4 s (dropped)
    (48000 * 45, 5),    # 40 + 5 s (kept)
])
def test_segment_count(samples, want):
    assert segment_count(samples) == want


def test_profile_validation():
    with pytest.raises(ValueError):
        ClapAnalyzer(Profile(model="openl3"), MODEL_DIR)
    with pytest.raises(ValueError):
        ClapAnalyzer(Profile(model="clap", model_variant="music",
                             model_embedding_size=1024), MODEL_DIR)


@conftest.clap
def test_real_model_shape_determinism():
    sr = 44100
    rng = np.random.RandomState(0)
    t = np.arange(int(25 * sr)) / sr
    pcm = (0.5 * np.sin(2 * np.pi * 220 * t)
           + 0.2 * np.sin(2 * np.pi * 440 * t)
           + 0.05 * rng.randn(len(t))).astype(np.float32)
    a = _analyzer()
    assert a.available()
    w = a.window_embeddings(pcm, sr)
    assert w.n == 3  # 10+10 + 5 s
    assert w.embeddings.shape == (3, 512)
    assert np.all(np.isfinite(w.embeddings))
    norms = np.linalg.norm(w.embeddings, axis=1)
    assert np.allclose(norms, 1.0, atol=1e-3)  # CLAP L2-normalizes
    w2 = a.window_embeddings(pcm, sr)
    np.testing.assert_array_equal(w.embeddings, w2.embeddings)


@conftest.clap
def test_real_model_short_audio_no_windows():
    sr = 44100
    pcm = np.zeros(int(4 * sr), dtype=np.float32)  # < 5 s
    w = _analyzer().window_embeddings(pcm, sr)
    assert w.n == 0
    assert not w.available


@conftest.clap
def test_model_identity():
    ident = _analyzer().model_identity()
    assert ident["model"] == MODEL_ID
    assert ident["weights_license"] == "apache-2.0"
    assert ident["name"] == "clap-music"
