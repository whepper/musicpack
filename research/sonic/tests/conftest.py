"""Shared helpers for the model-free research tests.

These tests require only numpy (and the research modules); no TensorFlow,
OpenL3 weights, or Essentia. They are the standard, always-green path and
run in CI.
"""

import os
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from pooling import WindowEmbeddings  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[3]
SR = 22050


def synth_pcm(duration: float, freq: float = 440.0, seed: int = 0, amp: float = 0.5, sr: int = SR):
    """Deterministic mono PCM: sum of sines + a little noise."""
    rng = np.random.RandomState(seed)
    t = np.arange(int(duration * sr)) / sr
    s = amp * 0.5 * np.sin(2 * np.pi * freq * t)
    s += amp * 0.2 * np.sin(2 * np.pi * freq * 1.5 * t)
    s += amp * 0.05 * rng.randn(len(t))
    return s.astype(np.float32)


def silent_pcm(duration: float):
    """Near-silent PCM (a whisper of noise ~ -90 dBFS)."""
    rng = np.random.RandomState(99)
    n = int(duration * SR)
    return (rng.randn(n) * 1e-5).astype(np.float32)


def make_windows(n: int, dim: int = 8, seed: int = 0, hop: float = 1.0):
    rng = np.random.RandomState(seed)
    E = rng.randn(n, dim).astype(np.float32)
    return WindowEmbeddings(E, (np.arange(n) + 0.5) * hop)


require_decoders = pytest.mark.skipif(
    not any((REPO_ROOT / p).is_file() for p in (
        "build/mpcdec/mpcdec",
        "build/mpcdec/Release/mpcdec.exe",
        "build/mpcdec/mpcdec.exe",
    )),
    reason="mpcdec binary not built (run cmake build first)",
)


def _openl3_importable() -> bool:
    try:
        import openl3  # noqa: F401
        return True
    except ImportError:
        return False


openl3 = pytest.mark.skipif(
    not _openl3_importable(),
    reason="openl3/TensorFlow not installed (run bootstrap_env.sh)",
)


def _essentia_importable() -> bool:
    try:
        import essentia  # noqa: F401
        return True
    except ImportError:
        return False


discogs = pytest.mark.skipif(
    not _essentia_importable(),
    reason="essentia-tensorflow not installed (eval-only; run bootstrap of .venv-essentia)",
)

require_ffmpeg = pytest.mark.skipif(
    not any(
        os.access(c, os.X_OK)
        for c in (
            os.environ.get("FFMPEG", "/usr/local/bin/ffmpeg"),
            "/opt/homebrew/bin/ffmpeg",
            "/usr/bin/ffmpeg",
        )
    ),
    reason="ffmpeg not found",
)
