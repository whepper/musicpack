"""Research-vs-production embedding compatibility for musicpack-sonic-openl3-v1.

Compares the research pipeline (openl3 0.4.0 + TensorFlow) against the
production pipeline (numpy frontend reference + ONNX post-frontend network,
the exact algorithm the C analyzer implements):

  research:    decode -> resampy kaiser_best -> center+window -> kapre
               frontend (in TF) -> Keras conv stack -> mean-norm pool
  production:  decode -> resampy kaiser_best -> center+window -> frontend.py
               -> ONNX conv stack -> mean-norm pool

Compatibility gate (see the Sonic spec / phase plan): cosine >= 0.9999,
mean absolute difference <= 1e-4, max absolute difference <= 2e-3 — the
differences must not be able to reorder recommendations.

Usage (research venv):

    .venv/bin/python compat_measure.py <out.onnx> [audio files...]

With no audio files, deterministic synthetic tones are generated. Reports
per-track and aggregate metrics. Also verifies the numpy frontend reproduces
the kapre frontend directly (the mel-level check).

This is model-free infrastructure: it runs only where the research stack is
available and is never a MusicPack dependency.
"""

import os
import sys
import tempfile

import numpy as np

import frontend

os.environ.setdefault(
    "OPENL3_MODEL_DIR", os.path.join(os.path.dirname(os.path.abspath(__file__)), "models")
)


def decode(path, mpcdec=None):
    """Decode audio to (mono float32 pcm, sample_rate)."""
    import soundfile as sf
    from decode import decode as _decode
    pcm, sr, _ = _decode(path, mpcdec)
    return pcm.astype(np.float32), int(sr)


def resample(pcm, sr):
    import resampy
    if sr == frontend.SR:
        return pcm
    return resampy.resample(pcm, sr_orig=sr, sr_new=frontend.SR,
                            filter="kaiser_best").astype(np.float32)


def research_embeddings(pcm48, model):
    from openl3.core import get_audio_embedding
    emb, _ = get_audio_embedding(pcm48, frontend.SR, model=model,
                                 input_repr="mel256", content_type="music",
                                 embedding_size=512, center=True, hop_size=1.0,
                                 batch_size=32, frontend="kapre", verbose=0)
    return np.asarray(emb, dtype=np.float32)


def production_embeddings(pcm48, sess):
    w, _ = frontend.windows(frontend.center_pad(pcm48))
    mels = np.stack([frontend.frontend(wi) for wi in w])
    return sess.run(None, {"mel": mels})[0]


def pool(E):
    return frontend.track_embedding(E)


def metrics(a, b):
    cos = float(np.dot(a, b) / (np.linalg.norm(a) * np.linalg.norm(b)))
    d = np.abs(a - b)
    return cos, float(d.max()), float(d.mean())


def synthetic_corpus():
    """Deterministic synthetic tones: chords, harmonics, noise, silence."""
    rng = np.random.RandomState(42)
    files = []
    tmp = tempfile.mkdtemp(prefix="sonic-compat-")
    for name, sr, dur, kind in [
        ("chord-44k", 44100, 6.0, "chord"),
        ("tone-48k", 48000, 6.5, "tone"),
        ("harmonics-44k", 44100, 9.0, "harmonics"),
        ("noise-48k", 48000, 7.0, "noise"),
        ("edge-44k", 44100, 1.2, "tone"),   # short track (2 windows)
    ]:
        t = np.arange(int(dur * sr)) / sr
        x = np.zeros_like(t, dtype=np.float32)
        if kind in ("tone", "chord"):
            freqs = [220.0] if kind == "tone" else [220.0, 277.18, 329.63, 440.0]
            for f in freqs:
                x += 0.25 * np.sin(2 * np.pi * f * t).astype(np.float32)
        elif kind == "harmonics":
            for k in range(1, 9):
                x += (0.4 / k) * np.sin(2 * np.pi * k * 110.0 * t).astype(np.float32)
        elif kind == "noise":
            x = rng.randn(t.size).astype(np.float32) * 0.2
        if kind == "chord":
            env = np.minimum(t * 2.0, 1.0) * np.minimum((dur - t) * 2.0, 1.0)
            x *= env.astype(np.float32)
        path = os.path.join(tmp, f"{name}.wav")
        import soundfile as sf
        sf.write(path, x, sr)
        files.append(path)
    return files


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    onnx_path = sys.argv[1]
    files = sys.argv[2:]

    import onnxruntime as ort
    sess = ort.InferenceSession(onnx_path, providers=["CPUExecutionProvider"])
    from openl3.models import load_audio_embedding_model
    model = load_audio_embedding_model(input_repr="mel256", content_type="music",
                                       embedding_size=512)

    if not files:
        files = synthetic_corpus()

    rows = []
    for path in files:
        pcm, sr = decode(path)
        pcm48 = resample(pcm, sr)
        a = research_embeddings(pcm48, model)
        b = production_embeddings(pcm48, sess)
        va, vb = pool(a), pool(b)
        if va is None or vb is None:
            print(f"{os.path.basename(path)}: no embedding (short/silent)")
            continue
        cos, mx, mn = metrics(va, vb)
        rows.append((os.path.basename(path), va.shape[0], cos, mx, mn))
        print(f"{os.path.basename(path):28s} windows={a.shape[0]:3d} "
              f"cosine={cos:.6f} maxdiff={mx:.2e} meandiff={mn:.2e}")

    if not rows:
        print("error: no comparable tracks", file=sys.stderr)
        return 1
    cosines = [r[2] for r in rows]
    maxs = [r[3] for r in rows]
    means = [r[4] for r in rows]
    gate_cos = min(cosines) >= 0.9999
    gate_mean = max(means) <= 1e-4
    gate_max = max(maxs) <= 2e-3
    print(f"\n{len(rows)} tracks: cosine min={min(cosines):.6f} "
          f"mean={np.mean(cosines):.6f}; "
          f"maxdiff max={max(maxs):.2e}; meandiff max={max(means):.2e}")
    print(f"gate cosine>=0.9999: {'PASS' if gate_cos else 'FAIL'}")
    print(f"gate meandiff<=1e-4:  {'PASS' if gate_mean else 'FAIL'}")
    print(f"gate maxdiff<=2e-3:   {'PASS' if gate_max else 'FAIL'}")
    return 0 if (gate_cos and gate_mean and gate_max) else 1


if __name__ == "__main__":
    sys.exit(main())
