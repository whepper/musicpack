"""Embedding storage encodings and strict validation."""

import base64

import numpy as np
import pytest

from storage import decode, encode, size_bytes, validate


def test_roundtrip_all_encodings():
    v = np.array([0.5, -0.25, 0.75, 0.1, 0.0, -0.9, 0.33, 0.2], dtype=np.float32)
    for enc in ("json", "base64-f32le", "binary-f32le"):
        payload = encode(v, enc)
        out = decode(payload, enc, len(v))
        np.testing.assert_allclose(out, v, rtol=0, atol=1e-7)


def test_sizes_ordered_json_gt_base64_gt_binary():
    rng = np.random.RandomState(0)
    v = rng.randn(512).astype(np.float32)
    v /= np.linalg.norm(v)
    sizes = {enc: size_bytes(encode(v, enc), enc) for enc in ("json", "base64-f32le", "binary-f32le")}
    assert sizes["json"] > sizes["base64-f32le"] > sizes["binary-f32le"]
    assert sizes["binary-f32le"] == 512 * 4
    assert sizes["base64-f32le"] == 4 * ((512 * 4 + 2) // 3)


def test_json_verbosity_scales():
    rng = np.random.RandomState(1)
    small = rng.randn(8).astype(np.float32)
    big = rng.randn(512).astype(np.float32)
    assert size_bytes(encode(big, "json"), "json") > 40 * size_bytes(encode(small, "json"), "json")


def test_decode_json_wrong_length_raises():
    with pytest.raises(ValueError):
        decode("[1,2,3]", "json", 4)


def test_decode_binary_wrong_length_raises():
    with pytest.raises(ValueError):
        decode(b"\x00" * 3, "binary-f32le", 4)


def test_decode_base64_invalid_raises():
    with pytest.raises(ValueError):
        decode("not-base64!!", "base64-f32le", 4)


def test_validate_rejects_nonfinite():
    with pytest.raises(ValueError):
        validate(np.array([np.nan, 1.0, 1.0, 1.0]), 4)


def test_validate_rejects_nonunit_norm():
    with pytest.raises(ValueError):
        validate(np.ones(4), 4)


def test_validate_accepts_normalized():
    v = np.array([0.5, 0.5, 0.5, 0.5], dtype=np.float32)
    validate(v, 4)


def test_json_roundtrip_rejects_non_list():
    with pytest.raises(ValueError):
        decode('"nope"', "json", 4)


def test_encode_unknown_encoding_raises():
    with pytest.raises(ValueError):
        encode(np.zeros(4), "nope")
