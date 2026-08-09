"""Optional, evaluation-only comparator: MTG/Essentia Discogs-EffNet models.

WARNING — licensing and production status:
  * This adapter exists ONLY to tell us whether a music-similarity-trained
    representation beats OpenL3 at collection-local recommendations.
  * Essentia is AGPL-3.0 (commercial licensing separate).
  * The MTG pretrained models are CC BY-NC-SA 4.0 (non-commercial,
    share-alike) unless separately licensed.
  * Essentia is therefore NEVER a MusicPack runtime dependency, and the
    model weights are never committed. Both are downloaded at runtime into
    the gitignored models/ cache with recorded SHA-256.
  * The benchmark harness and test suite run without this module.

Model behaviour (verified against essentia-tensorflow 2.1b6.dev1389):
  * 16 kHz mono input (Essentia resamples internally, quality 4);
  * internal 96-band log-mel frontend (frameSize 512, hop 256,
    slaneyMel, log10, scaled), the musicnn-style discogs frontend;
  * patches of 131 mel frames (2.096 s) striding 61 mel frames (0.976 s),
    last patch discarded; output is ``PartitionedCall:1`` (1280-dim);
  * the number of output frames for n16 samples is deterministic:
        nFrames = 1 + ceil((n16 - 256) / 256)
        nPatches = max(0, 1 + floor((nFrames - 131) / 61))
  * embeddings are NOT L2-normalized by the model (row norms ~25).
"""

from __future__ import annotations

import hashlib
import tempfile
import urllib.request
from pathlib import Path
from typing import Dict, Optional

import numpy as np

from pooling import WindowEmbeddings
from profile import Profile

from .base import Analyzer

PATCH_WINDOW_SECONDS = 33536.0 / 16000.0  # 131 mel frames * 256 samples
PATCH_HOP_SECONDS = 15616.0 / 16000.0     # 61 mel frames * 256 samples
EMBEDDING_DIM = 1280
SAMPLE_RATE = 16000

BASE_URL = "https://essentia.upf.edu/models/feature-extractors/discogs-effnet/"
# variant -> (pb filename, metadata json filename, sha256 of the .pb)
MODELS = {
    "multi": (
        "discogs_multi_embeddings-effnet-bs64-1.pb",
        "2c964064951217e1e345461cf88884086a21f4bca2ae0d48187ee75edc263cd7",
    ),
    "release": (
        "discogs_release_embeddings-effnet-bs64-1.pb",
        "bd044fe53b5d874d52374e023fa7befa1d1afdfd601ff5fe10824cacadaf4dc6",
    ),
}


def model_sha256(variant: str) -> str:
    return MODELS[variant][1]


class DiscogsError(RuntimeError):
    pass


class DiscogsAnalyzer(Analyzer):
    def __init__(self, profile: Profile, model_dir: Path, batch_size: int = 64):
        _validate_profile(profile)
        self.profile = profile
        self.model_dir = Path(model_dir)
        self.batch_size = int(batch_size)

    @property
    def name(self) -> str:
        return "discogs-" + (self.profile.model_variant or "effnet")

    def available(self) -> bool:
        try:
            import essentia  # noqa: F401
            return True
        except ImportError:
            return False

    # -- weights ----------------------------------------------------------
    def weight_path(self) -> Path:
        fname, _sha = MODELS[self.profile.model_variant]
        return self.model_dir / fname

    def ensure_weights(self) -> None:
        path = self.weight_path()
        self.model_dir.mkdir(parents=True, exist_ok=True)
        fname, sha = MODELS[self.profile.model_variant]
        if path.is_file():
            if _sha256(path) == sha:
                return
            raise DiscogsError(
                "model %s failed checksum; delete it and retry" % path)
        print("downloading Discogs-EffNet %s weights (CC BY-NC-SA 4.0) ..."
              % self.profile.model_variant)
        urllib.request.urlretrieve(BASE_URL + fname, path)
        if _sha256(path) != sha:
            raise DiscogsError("downloaded model failed checksum")

    # -- analysis ---------------------------------------------------------
    def window_embeddings(self, pcm: np.ndarray, sample_rate: int) -> WindowEmbeddings:
        from essentia.standard import MonoLoader, TensorflowPredictEffnetDiscogs

        self.ensure_weights()
        if len(pcm) < 1:
            return WindowEmbeddings(np.zeros((0, EMBEDDING_DIM), dtype=np.float32),
                                    np.zeros(0))

        pcm16 = np.clip(np.asarray(pcm, dtype=np.float32) * 32767.0,
                        -32768.0, 32767.0).astype(np.int16)
        with tempfile.NamedTemporaryFile(suffix=".wav") as tf:
            import wave
            with wave.open(tf.name, "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(int(sample_rate))
                w.writeframes(pcm16.tobytes())
            audio = MonoLoader(filename=tf.name, sampleRate=SAMPLE_RATE,
                               resampleQuality=4)()

        model = TensorflowPredictEffnetDiscogs(
            graphFilename=str(self.weight_path()), output="PartitionedCall:1",
            batchSize=self.batch_size)
        emb = np.asarray(model(audio))
        # essentia collapses single/empty outputs to 1-D; normalise to 2-D
        emb = emb.reshape(-1, EMBEDDING_DIM).astype(np.float32)
        n = emb.shape[0]
        timestamps = (np.arange(n) * PATCH_HOP_SECONDS + 0.5 * PATCH_WINDOW_SECONDS
                      if n else np.zeros(0))
        return WindowEmbeddings(emb, timestamps)

    def model_identity(self) -> Dict[str, str]:
        try:
            import essentia
            ver = getattr(essentia, "__version__", "unknown")
        except ImportError:
            ver = "unknown"
        fname, sha = MODELS[self.profile.model_variant]
        return {
            "name": self.name,
            "variant": self.profile.model_variant,
            "essentia": ver,
            "weight_file": fname,
            "weight_sha256": sha,
            "weights_cc_license": "CC BY-NC-SA 4.0",
        }


def _validate_profile(p: Profile) -> None:
    if p.model != "discogs-effnet":
        raise ValueError("DiscogsAnalyzer requires profile.model='discogs-effnet'")
    if p.model_variant not in MODELS:
        raise ValueError("DiscogsAnalyzer requires model_variant in %s"
                         % sorted(MODELS))
    if p.model_embedding_size != EMBEDDING_DIM:
        raise ValueError("DiscogsAnalyzer requires model_embedding_size=%d"
                         % EMBEDDING_DIM)


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()
