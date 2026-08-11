"""Profile fingerprints: comparability and cache identity."""

from sonic_profile import (
    POOL_MEAN,
    POOL_MEAN_NORM,
    POOL_ROBUST_MEAN,
    PoolingParams,
    Profile,
    SilenceParams,
)


def base_profile(**kw) -> Profile:
    p = Profile()
    for k, v in kw.items():
        object.__setattr__(p, k, v)
    return p


def test_fingerprint_stable_across_instances():
    a = base_profile()
    b = base_profile()
    assert a.fingerprint() == b.fingerprint()
    assert a.id == b.id
    assert a.id.startswith("musicpack-sonic-v1-")


def test_any_model_param_changes_fingerprint():
    p = base_profile()
    for attr, value in [
        ("model", "discogs-effnet"),
        ("model_variant", "multi"),
        ("model_content", "env"),
        ("model_input_repr", "mel128"),
        ("model_embedding_size", 6144),
        ("model_sample_rate", 16000),
        ("frontend", "librosa"),
        ("center", False),
        ("model_weights_sha256", "deadbeef"),
        ("distance", "euclidean"),
    ]:
        other = base_profile(**{attr: value})
        assert other.fingerprint() != p.fingerprint(), attr


def test_any_pooling_param_changes_fingerprint():
    p = base_profile()
    assert base_profile(pooling=PoolingParams(strategy=POOL_MEAN)).fingerprint() != p.fingerprint()
    assert base_profile(pooling=PoolingParams(hop_seconds=0.5)).fingerprint() != p.fingerprint()
    assert base_profile(pooling=PoolingParams(strategy=POOL_ROBUST_MEAN, robust_trim=0.05)).fingerprint() != p.fingerprint()
    assert base_profile(
        pooling=PoolingParams(silence=SilenceParams(enabled=True))
    ).fingerprint() != p.fingerprint()


def test_compatible_only_on_full_match():
    a = base_profile()
    b = base_profile(pooling=PoolingParams(hop_seconds=0.5))
    assert a.compatible_with(base_profile())
    assert not a.compatible_with(b)


def test_model_key_ignores_pooling():
    a = base_profile()
    b = base_profile(pooling=PoolingParams(hop_seconds=0.5, strategy=POOL_MEAN))
    c = base_profile(model_input_repr="mel128")
    # window embeddings are the same across pooling choices, so the model key
    # must match for a/b and differ for the model change
    assert a.model_key() == b.model_key()
    assert a.model_key() != c.model_key()


def test_hop_change_changes_full_fingerprint():
    # hop is part of pooling -> changes the full profile (track cache) even
    # though the model key is unchanged
    a = base_profile()
    b = base_profile(pooling=PoolingParams(hop_seconds=0.5))
    assert a.fingerprint() != b.fingerprint()
    assert a.model_key() == b.model_key()
