"""Embedding storage encodings and validation.

Three representations are compared for the Sonic v1 profile (see
``benchmark.py float-repr``):
  * "json"        — decimal float array (portable, verbose)
  * "base64-f32le"— little-endian float32 bytes, base64 (compact, portable)
  * "binary-f32le"— raw little-endian float32 bytes (smallest, needs an
                    out-of-band length; for a separate vector file)

All three are lossless for float32 values. Validation is strict: exact
dimensions, finite values, and (for profile embeddings) unit L2 norm within
tolerance.
"""

from __future__ import annotations

import base64
import json
import struct
from typing import Optional

import numpy as np

ENCODINGS = ("json", "base64-f32le", "binary-f32le")
NORM_TOLERANCE = 1e-3


def encode(v: np.ndarray, encoding: str):
    v = np.asarray(v, dtype=np.float32)
    if encoding == "json":
        return json.dumps([float(x) for x in v], separators=(",", ":"))
    if encoding == "base64-f32le":
        return base64.b64encode(v.tobytes()).decode("ascii")
    if encoding == "binary-f32le":
        return v.tobytes()
    raise ValueError("unknown encoding: %r" % encoding)


def decode(payload, encoding: str, dimensions: int) -> np.ndarray:
    if encoding == "json":
        if isinstance(payload, bytes):
            payload = payload.decode("utf-8")
        values = json.loads(payload)
        if not isinstance(values, list) or len(values) != dimensions:
            raise ValueError("malformed embedding: expected %d values" % dimensions)
        v = np.array(values, dtype=np.float32)
    elif encoding == "base64-f32le":
        raw = base64.b64decode(payload, validate=True)
        v = _raw_to_vector(raw, dimensions)
    elif encoding == "binary-f32le":
        v = _raw_to_vector(payload, dimensions)
    else:
        raise ValueError("unknown encoding: %r" % encoding)
    validate(v, dimensions, check_norm=False)
    return v


def _raw_to_vector(raw, dimensions: int) -> np.ndarray:
    if len(raw) != dimensions * 4:
        raise ValueError(
            "malformed embedding: expected %d bytes, got %d"
            % (dimensions * 4, len(raw))
        )
    return np.frombuffer(raw, dtype="<f4").copy()


def validate(v: np.ndarray, dimensions: int, check_norm: bool = True) -> None:
    """Strict validation of a stored/profile embedding. Raises ValueError on
    any problem."""
    v = np.asarray(v, dtype=np.float32)
    if v.ndim != 1 or v.shape[0] != dimensions:
        raise ValueError(
            "embedding must be 1-D with %d dims, got shape %s"
            % (dimensions, v.shape)
        )
    if not np.all(np.isfinite(v)):
        raise ValueError("embedding contains non-finite values")
    if check_norm:
        n = float(np.linalg.norm(v))
        if abs(n - 1.0) > NORM_TOLERANCE:
            raise ValueError("embedding is not unit-normalized (norm=%.4f)" % n)


def normalized(v: np.ndarray, dimensions: int) -> np.ndarray:
    """Validate and return the embedding as float32."""
    validate(v, dimensions)
    return np.asarray(v, dtype=np.float32)


def size_bytes(payload, encoding: str) -> int:
    """Byte size of an encoded payload (of the encoded form)."""
    if isinstance(payload, (bytes, bytearray)):
        return len(payload)
    return len(payload.encode("utf-8"))
