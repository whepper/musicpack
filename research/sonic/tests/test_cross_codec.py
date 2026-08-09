"""Cross-codec stability report generation (model-free).

Uses a tiny deterministic "band-energy" embedder so the full
decode -> encode -> decode -> embed -> cosine -> report pipeline is tested
without TensorFlow. Requires the built mpcenc/mpcdec and ffmpeg; skipped in
the CI suite otherwise.
"""

import os
import subprocess

import numpy as np
import pytest

import report as rep
from conftest import REPO_ROOT, SR, require_decoders, synth_pcm
from decode import decode
from pooling import POOL_MEAN_NORM, WindowEmbeddings, pool_windows

MPCENC = next(
    (REPO_ROOT / p for p in (
        "build/mpcenc/mpcenc",
        "build/mpcenc/Release/mpcenc.exe",
        "build/mpcenc/mpcenc.exe",
    ) if (REPO_ROOT / p).is_file()),
    None,
)
FFMPEG = next(
    (c for c in ("ffmpeg", "/usr/local/bin/ffmpeg", "/opt/homebrew/bin/ffmpeg")
     if os.access(c, os.X_OK) or os.path.isfile(c)),
    None,
)
requires_tools = pytest.mark.skipif(
    MPCENC is None or FFMPEG is None, reason="mpcenc or ffmpeg not available"
)

import soundfile as sf


def _band_embed(pcm: np.ndarray, sr: int) -> WindowEmbeddings:
    """Deterministic 8-dim per-second band-energy windows (no ML)."""
    import numpy as np
    hop = sr
    n_windows = max(0, len(pcm) // hop)
    out = []
    for i in range(n_windows):
        seg = pcm[i * hop:(i + 1) * hop]
        spec = np.abs(np.fft.rfft(seg))
        freqs = np.fft.rfftfreq(len(seg), 1.0 / sr)
        bands = np.array([0, 200, 500, 1000, 2000, 4000, 8000, sr / 2 + 1])
        energies = []
        for b in range(len(bands) - 1):
            m = (freqs >= bands[b]) & (freqs < bands[b + 1])
            energies.append(float(np.mean(spec[m] ** 2)) if m.any() else 0.0)
        out.append(np.array(energies, dtype=np.float32))
    if not out:
        return WindowEmbeddings(np.zeros((0, 8), dtype=np.float32), np.zeros(0))
    return WindowEmbeddings(np.stack(out), (np.arange(len(out)) + 0.5) * 1.0)


def _embed_track(path):
    pcm, sr, _ = decode(path)
    return pool_windows(_band_embed(pcm, sr), POOL_MEAN_NORM)


def test_report_rendering(tmp_path):
    rows = [["cos_source_flac", 2, 0.99, 0.01, 0.98, 0.99, 1.0]]
    out = rep.write_cross_codec_report(tmp_path, "musicpack-sonic-v1-abcdef", rows,
                                       [{"index": 0, "cos_source_flac": 0.99}])
    assert out.is_file()
    text = out.read_text()
    assert "# Cross-codec stability" in text
    assert "cos_source_flac" in text
    assert (tmp_path / "raw" / "cross-codec-musicpac.json").is_file()


@require_decoders
@requires_tools
def test_cross_codec_pipeline_and_report(tmp_path):
    import numpy as np
    import soundfile as sf

    src_wav = tmp_path / "src.wav"
    sf.write(str(src_wav), synth_pcm(4.0, seed=6, sr=44100), 44100)

    flac = tmp_path / "src.flac"
    mpc = tmp_path / "src.mpc"
    r = subprocess.run([FFMPEG, "-y", "-v", "error", "-i", str(src_wav), str(flac)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    r = subprocess.run(
        [str(MPCENC), "--silent", "--overwrite", "--quality", "6", str(src_wav), str(mpc)],
        capture_output=True, text=True)
    assert r.returncode == 0, r.stderr

    e0 = _embed_track(src_wav)
    ef = _embed_track(flac)
    em = _embed_track(mpc)
    assert all(v is not None for v in (e0, ef, em))

    from pooling import cosine
    assert cosine(e0, ef) > 0.99   # lossless source == flac
    assert cosine(e0, em) > 0.9    # lossy mpc still close on pure tones

    per_track = [{"index": 0,
                  "cos_source_flac": round(cosine(e0, ef), 5),
                  "cos_source_mpc": round(cosine(e0, em), 5),
                  "cos_flac_mpc": round(cosine(ef, em), 5)}]
    rows = []
    for metric in ("cos_source_flac", "cos_source_mpc", "cos_flac_mpc"):
        s = rep.stats([t[metric] for t in per_track])
        rows.append([metric, s["n"]] + [s[k] for k in ("mean", "std", "min", "median", "max")])
    out = rep.write_cross_codec_report(tmp_path, "musicpack-sonic-v1-deadbeef", rows, per_track)
    text = out.read_text()
    assert "# Cross-codec stability" in text
    assert "cos_source_mpc" in text and "cos_flac_mpc" in text
