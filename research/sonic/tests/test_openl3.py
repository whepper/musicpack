"""OpenL3 analyzer tests (real model; skipped when TF/openl3 is absent).

These are NOT in the always-green CI suite. They download the CC BY 4.0
weights into research/sonic/models/ (gitignored) on first run.
"""

import numpy as np
import pytest

import conftest
from analyzers.openl3 import WEIGHTS_SHA256, WEIGHTS_BASENAME, OpenL3Analyzer
from pooling import POOL_MEAN_NORM, cosine, pool_windows
from sonic_profile import Profile, PoolingParams, SilenceParams

pytestmark = [conftest.openl3]

SR = 44100


def _pcm(seconds=10.0, seed=0):
    rng = np.random.RandomState(seed)
    t = np.arange(int(seconds * SR)) / SR
    return (
        0.5 * np.sin(2 * np.pi * 220 * t)
        + 0.2 * np.sin(2 * np.pi * 440 * t)
        + 0.05 * rng.randn(len(t))
    ).astype(np.float32)


def _analyzer(hop=1.0, **kw):
    return OpenL3Analyzer(
        Profile(pooling=PoolingParams(hop_seconds=hop), **kw),
        conftest.REPO_ROOT / "research/sonic/models",
    )


def test_weight_constants_recorded():
    assert WEIGHTS_BASENAME == "openl3_audio_mel256_music.h5"
    assert len(WEIGHTS_SHA256) == 64


def test_profile_must_be_openl3_music_mel256_512_kapre():
    with pytest.raises(ValueError):
        OpenL3Analyzer(Profile(model="discogs-effnet"), conftest.REPO_ROOT / "research/sonic/models")
    with pytest.raises(ValueError):
        OpenL3Analyzer(Profile(model_input_repr="mel128"), conftest.REPO_ROOT / "research/sonic/models")
    with pytest.raises(ValueError):
        OpenL3Analyzer(Profile(frontend="librosa"), conftest.REPO_ROOT / "research/sonic/models")


def test_model_identity_records_versions():
    ident = _analyzer().model_identity()
    assert ident["name"] == "openl3"
    assert ident["openl3"] == "0.4.0"
    assert ident["weight_sha256"] == WEIGHTS_SHA256


def test_short_audio_has_no_windows():
    a = _analyzer()
    w = a.window_embeddings(_pcm(0.5), SR)
    assert w.n == 0
    assert not w.available
    assert pool_windows(w, POOL_MEAN_NORM) is None


def test_window_embeddings_shape_and_determinism():
    a = _analyzer(hop=1.0)
    w = a.window_embeddings(_pcm(10.0), SR)
    assert w.n == 11  # center=True => 1 + ceil((10s - 1s)/1s)
    assert w.embeddings.shape == (11, 512)
    assert w.timestamps[0] == 0.0
    assert np.all(np.isfinite(w.embeddings))
    w2 = a.window_embeddings(_pcm(10.0), SR)
    np.testing.assert_array_equal(w.embeddings, w2.embeddings)


def test_hop_counts():
    w1 = _analyzer(hop=1.0).window_embeddings(_pcm(10.0), SR)
    w05 = _analyzer(hop=0.5).window_embeddings(_pcm(10.0), SR)
    assert w1.n == 11
    assert w05.n == 20


def test_windows_not_unit_norm():
    # openl3 does not L2-normalize its output; this is what makes pooling
    # strategy A (per-window norm) differ from B (plain mean)
    w = _analyzer().window_embeddings(_pcm(10.0), SR)
    norms = np.linalg.norm(w.embeddings, axis=1)
    assert np.all(norms > 5.0)


def test_silence_gate_on_constant_synth_keeps_all():
    pcm = _pcm(10.0)
    a = _analyzer(hop=1.0)
    w = a.window_embeddings(pcm, SR)
    p = Profile(
        pooling=PoolingParams(
            hop_seconds=1.0,
            silence=SilenceParams(enabled=True, threshold_db=-20.0, relative_to_median=True),
        )
    )
    from pooling import apply_silence_and_pool
    v = apply_silence_and_pool(w, pcm, SR, p.pooling.silence, p.pooling.strategy, p.pooling.robust_trim)
    assert v is not None
    assert abs(np.linalg.norm(v) - 1.0) < 1e-3
