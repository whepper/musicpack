"""Decode-layer tests (FLAC/WAV via soundfile; MPC via the built tools).

Marked so the always-green CI suite skips them (they need the C build or
libsndfile FLAC). The benchmark always analyses decoded PCM.
"""

import os
import subprocess

import numpy as np
import pytest
import soundfile as sf

from conftest import REPO_ROOT, SR, require_decoders, synth_pcm
from decode import DecodeError, audio_sha256, decode

MPCENC = next(
    (REPO_ROOT / p for p in (
        "build/mpcenc/mpcenc",
        "build/mpcenc/Release/mpcenc.exe",
        "build/mpcenc/mpcenc.exe",
    ) if (REPO_ROOT / p).is_file()),
    None,
)
requires_mpcenc = pytest.mark.skipif(MPCENC is None, reason="mpcenc binary not built")
requires_soundfile_flac = pytest.mark.skipif(
    "FLAC" not in (sf.available_formats() or {}),
    reason="libsndfile without FLAC support",
)


def test_wav_roundtrip(tmp_path):
    wav = tmp_path / "t.wav"
    sf.write(str(wav), synth_pcm(2.0), SR)
    pcm, sr, dur = decode(wav)
    assert sr == SR
    assert dur == pytest.approx(2.0, abs=1e-3)
    assert pcm.ndim == 1


@requires_soundfile_flac
def test_flac_roundtrip(tmp_path):
    flac = tmp_path / "t.flac"
    sf.write(str(flac), synth_pcm(2.0, seed=4), SR, format="FLAC")
    pcm, sr, dur = decode(flac)
    assert sr == SR
    assert dur == pytest.approx(2.0, abs=1e-3)
    assert np.all(np.isfinite(pcm))


def test_audio_sha256_changes_with_content(tmp_path):
    a = tmp_path / "a.bin"
    b = tmp_path / "b.bin"
    a.write_bytes(b"hello")
    b.write_bytes(b"hello!")
    assert audio_sha256(a) != audio_sha256(b)
    assert audio_sha256(a) == audio_sha256(a)


@require_decoders
@requires_mpcenc
def test_mpc_roundtrip_via_cli(tmp_path):
    enc_sr = 44100  # Musepack supports 32/37.8/44.1/48 kHz only
    src = tmp_path / "src.wav"
    mpc = tmp_path / "src.mpc"
    sf.write(str(src), synth_pcm(4.0, seed=6, sr=enc_sr), enc_sr)
    r = subprocess.run(
        [MPCENC, "--silent", "--overwrite", "--quality", "6", str(src), str(mpc)],
        capture_output=True, text=True,
    )
    assert r.returncode == 0, r.stderr
    pcm, sr, dur = decode(mpc)
    assert sr == enc_sr
    assert dur == pytest.approx(4.0, abs=0.06)  # MP pads to whole frames
    assert np.all(np.isfinite(pcm))


def test_unsupported_extension(tmp_path):
    f = tmp_path / "t.mp3"
    f.write_bytes(b"nope")
    with pytest.raises(DecodeError):
        decode(f)
