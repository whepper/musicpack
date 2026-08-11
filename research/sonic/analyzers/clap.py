"""Decision-gate candidate: LAION-CLAP music checkpoint.

LAION-CLAP (Contrastive Language-Audio Pretraining) ``larger_clap_music``:
  * Apache-2.0 (code and weights) — the only permissively licensed,
    music-specific similarity-capable model found in the survey;
  * audio encoder is a SWIN-like transformer over a 64-band log-mel input;
    48 kHz; native chunk 10 s; projection to 512-dim;
  * ``get_audio_features`` returns an L2-normalized 512-dim embedding per
    clip (one clip per input segment);
  * deterministic: exact 10 s / 48 kHz segments avoid the processor's
    ``rand_trunc`` path; verified bit-identical across runs.

Track embedding: non-overlapping 10 s segments; a trailing segment with
>= MIN_FINAL_SECONDS of audio is zero-padded to 10 s and kept; shorter
remnants are dropped (sub-5 s tracks produce no embedding — the explicit
no-embedding convention). Segment length is a profile parameter
(``pooling.window_seconds`` / ``hop_seconds``).

The checkpoint is downloaded at runtime into the gitignored models/ cache
(SHA-256 verified) and never committed.
"""

from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Dict

import numpy as np

from pooling import WindowEmbeddings
from profile import Profile

from .base import Analyzer

MODEL_ID = "laion/larger_clap_music"
COMMIT_SHA = "a0b4534a14f58e20944452dff00a22a06ce629d1"
WEIGHTS_SHA256 = "5c289311f4a030d768af7ffbfdecd01b008aa64824211899a4e59f4f9d154fd1"
SEGMENT_SECONDS = 10.0
MIN_FINAL_SECONDS = 5.0
SAMPLE_RATE = 48000
EMBEDDING_DIM = 512
LICENSE = "apache-2.0"


class ClapError(RuntimeError):
    pass


class ClapAnalyzer(Analyzer):
    def __init__(self, profile: Profile, model_dir: Path):
        _validate_profile(profile)
        self.profile = profile
        self.model_dir = Path(model_dir) / MODEL_ID
        self._model = None
        self._proc = None

    @property
    def name(self) -> str:
        return "clap-" + (self.profile.model_variant or "music")

    def available(self) -> bool:
        try:
            import torch  # noqa: F401
            import transformers  # noqa: F401
            return True
        except ImportError:
            return False

    # -- weights ----------------------------------------------------------
    def ensure_weights(self) -> None:
        binfile = self.model_dir / "pytorch_model.bin"
        if binfile.is_file():
            if _sha256(binfile) == WEIGHTS_SHA256:
                return
            raise ClapError(
                "CLAP checkpoint failed checksum; delete %s and retry"
                % self.model_dir)
        print("downloading %s (Apache-2.0) ..." % MODEL_ID)
        from huggingface_hub import snapshot_download
        snapshot_download(MODEL_ID, local_dir=str(self.model_dir))
        if _sha256(binfile) != WEIGHTS_SHA256:
            raise ClapError("downloaded CLAP checkpoint failed checksum")

    # -- model ------------------------------------------------------------
    def _load(self):
        from transformers import ClapModel, ClapProcessor
        self._model = ClapModel.from_pretrained(str(self.model_dir))
        self._model.eval()
        self._proc = ClapProcessor.from_pretrained(str(self.model_dir))

    # -- analysis ---------------------------------------------------------
    def window_embeddings(self, pcm: np.ndarray, sample_rate: int) -> WindowEmbeddings:
        import torch
        import resampy

        self.ensure_weights()
        if self._model is None:
            self._load()

        pcm = np.asarray(pcm, dtype=np.float32)
        if len(pcm) == 0:
            return WindowEmbeddings(
                np.zeros((0, EMBEDDING_DIM), dtype=np.float32), np.zeros(0))
        pcm48 = resampy.resample(pcm, int(sample_rate), SAMPLE_RATE,
                                 filter="kaiser_best").astype(np.float32)

        seg_samples = int(SEGMENT_SECONDS * SAMPLE_RATE)
        n_seg = len(pcm48) // seg_samples
        segments = [pcm48[i * seg_samples:(i + 1) * seg_samples]
                    for i in range(n_seg)]
        trailing = pcm48[n_seg * seg_samples:]
        if len(trailing) / SAMPLE_RATE >= MIN_FINAL_SECONDS:
            pad = seg_samples - len(trailing)
            if pad > 0:
                trailing = np.concatenate(
                    [trailing, np.zeros(pad, dtype=np.float32)])
            segments.append(trailing)

        if not segments:
            return WindowEmbeddings(
                np.zeros((0, EMBEDDING_DIM), dtype=np.float32), np.zeros(0))

        with torch.no_grad():
            inputs = self._proc(audio=segments, sampling_rate=SAMPLE_RATE,
                                return_tensors="pt")
            emb = self._model.get_audio_features(
                **inputs).pooler_output.numpy()

        n = emb.shape[0]
        timestamps = ((np.arange(n) + 0.5) * SEGMENT_SECONDS
                      if n else np.zeros(0))
        return WindowEmbeddings(emb.astype(np.float32), timestamps)

    def model_identity(self) -> Dict[str, str]:
        try:
            import torch
            import transformers
            torch_ver = torch.__version__
            tf_ver = transformers.__version__
        except ImportError:
            torch_ver = tf_ver = "unknown"
        return {
            "name": self.name,
            "model": MODEL_ID,
            "checkpoint_commit": COMMIT_SHA,
            "weight_file": "pytorch_model.bin",
            "weight_sha256": WEIGHTS_SHA256,
            "weights_license": LICENSE,
            "torch": torch_ver,
            "transformers": tf_ver,
            "projection_dim": str(EMBEDDING_DIM),
            "sample_rate": str(SAMPLE_RATE),
        }


def segment_count(samples_48k: int) -> int:
    """Number of 10 s segments for a 48 kHz signal: complete segments plus a
    final partial kept when it holds >= MIN_FINAL_SECONDS of audio."""
    seg = int(SEGMENT_SECONDS * SAMPLE_RATE)
    n = samples_48k // seg
    trailing = samples_48k - n * seg
    if trailing / SAMPLE_RATE >= MIN_FINAL_SECONDS:
        n += 1
    return n


def _validate_profile(p: Profile) -> None:
    if p.model != "clap":
        raise ValueError("ClapAnalyzer requires profile.model='clap'")
    if p.model_variant not in ("", "music"):
        raise ValueError("ClapAnalyzer requires model_variant 'music'")
    if p.model_embedding_size != EMBEDDING_DIM:
        raise ValueError("ClapAnalyzer requires model_embedding_size=%d"
                         % EMBEDDING_DIM)


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()
