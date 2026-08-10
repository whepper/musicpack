"""Decode audio to mono float32 PCM.

The benchmark always analyses *decoded PCM*, never compressed bytes, so the
sonic result does not depend on the source being FLAC, Musepack or WAV
except for normal lossy-codec differences:

    FLAC ─┐
    MPC ──┼→ PCM → analyzer
    WAV ──┘

FLAC/WAV are read with soundfile (libsndfile). MPC is decoded with the
`mpcdec` CLI (``--mpcdec``), a deterministic decode path. The audio content
SHA-256 (of the source file bytes) is the cache identity.
"""

from __future__ import annotations

import hashlib
import os
import subprocess
import tempfile
from pathlib import Path
from typing import Optional, Tuple

import numpy as np
import soundfile as sf

Decoded = Tuple[np.ndarray, int, float]  # (mono float32 PCM, sample_rate, seconds)


class DecodeError(RuntimeError):
    pass


SUPPORTED = (".wav", ".flac", ".mpc")


def audio_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def find_mpcdec(hint: Optional[Path]) -> str:
    if hint is not None:
        return str(hint)
    root = Path(__file__).resolve().parents[2]  # repo root
    for candidate in (
        root / "build" / "mpcdec" / "mpcdec",
        root / "build" / "mpcdec" / "Release" / "mpcdec.exe",
        root / "build" / "mpcdec" / "mpcdec.exe",
    ):
        if candidate.is_file():
            return str(candidate)
    from shutil import which

    found = which("mpcdec")
    if found:
        return found
    raise DecodeError("mpcdec not found; pass --mpcdec PATH")


def decode(path: Path, mpcdec: Optional[Path] = None) -> Decoded:
    path = Path(path)
    ext = path.suffix.lower()
    if ext == ".mpc":
        pcm, sr = _decode_mpc(path, mpcdec)
    elif ext in (".wav", ".flac"):
        data, sr = sf.read(str(path), dtype="float32", always_2d=True)
        pcm = data.mean(axis=1).astype(np.float32)  # to mono
    else:
        raise DecodeError("unsupported audio format: %s" % path.suffix)
    return pcm, int(sr), float(len(pcm)) / int(sr)


def _decode_mpc(path: Path, mpcdec: Optional[Path]) -> Tuple[np.ndarray, int]:
    exe = find_mpcdec(mpcdec)
    with tempfile.TemporaryDirectory(prefix="mpcsonic-") as tmp:
        out = os.path.join(tmp, "out.wav")
        proc = subprocess.run(
            [exe, str(path), out], capture_output=True, text=True
        )
        if proc.returncode != 0 or not os.path.isfile(out):
            raise DecodeError(
                "mpcdec failed on %s: %s" % (path.name, proc.stderr.strip())
            )
        data, sr = sf.read(out, dtype="float32", always_2d=True)
        return data.mean(axis=1).astype(np.float32), int(sr)
