"""Primary candidate: OpenL3 music/mel256/emb512.

All parameters that affect the embedding are taken from ``Profile`` and are
pinned explicitly — never library defaults. The weight file is downloaded
once into ``model_dir`` (gitignored) with its SHA-256 recorded and verified;
``model_weights_sha256`` becomes part of the profile fingerprint.

OpenL3 facts pinned here (recorded in the report, verified against
openl3 0.4.0):
  * window length 1.0 s; internal target sample rate 48000 (resampled with
    resampy 'kaiser_best' when the input is not 48k);
  * mel256 kapre frontend: n_fft=2048, hop=242, n_mels=256, sr=48000,
    decibel, pad_end; input is the raw waveform frame;
  * output embeddings are NOT L2-normalized (final layer is pooling+flatten),
    so per-window normalization (pooling strategy A) genuinely differs from
    the plain mean (strategy B);
  * with ``center=True`` the first sample sits at the centre of the first
    window and timestamps are ``arange(n_windows) * hop_seconds``;
  * openl3 0.4.0 has no ``audio_crop`` option (it was removed after 0.3.x).

Tracks shorter than the window produce no windows and thus no embedding
(handled deterministically here).
"""

from __future__ import annotations

import gzip
import hashlib
import os
import urllib.request
from pathlib import Path
from typing import Dict, Optional

import numpy as np

from pooling import WindowEmbeddings
from profile import Profile

from .base import Analyzer

WEIGHTS_BASENAME = "openl3_audio_mel256_music.h5"
WEIGHTS_GZ = "openl3_audio_mel256_music-v0_4_0.h5.gz"
WEIGHTS_URL = "https://github.com/marl/openl3/raw/models/" + WEIGHTS_GZ
WEIGHTS_SHA256 = "624ee7b1dd5ff87e18073f66fd8b2052bebb8ac70210e9c0937c0c940c63e9d6"
WEIGHTS_GZ_SHA256 = "e379f41e880b78e157c199ebddac80d3764e6a46282bcccdbe875bcf538f9afd"
OPENL3_VERSION = "0.4.0"


class OpenL3Error(RuntimeError):
    pass


class OpenL3Analyzer(Analyzer):
    def __init__(self, profile: Profile, model_dir: Path, batch_size: int = 32):
        _validate_profile(profile)
        self.profile = profile
        self.model_dir = Path(model_dir)
        self.batch_size = int(batch_size)
        self._model = None

    @property
    def name(self) -> str:
        return "openl3"

    def available(self) -> bool:
        try:
            import openl3  # noqa: F401
            return True
        except ImportError:
            return False

    # -- weights ----------------------------------------------------------
    def weight_path(self) -> Path:
        return self.model_dir / WEIGHTS_BASENAME

    def ensure_weights(self) -> None:
        target = self.weight_path()
        self.model_dir.mkdir(parents=True, exist_ok=True)
        if target.is_file():
            if _sha256(target) == WEIGHTS_SHA256:
                return
            raise OpenL3Error(
                "OpenL3 weight file %s failed checksum; delete it and retry"
                % target)
        gz = self.model_dir / WEIGHTS_GZ
        if not gz.is_file() or _sha256(gz) != WEIGHTS_GZ_SHA256:
            print("downloading OpenL3 music/mel256/512 weights ...")
            urllib.request.urlretrieve(WEIGHTS_URL, gz)
        if _sha256(gz) != WEIGHTS_GZ_SHA256:
            raise OpenL3Error("downloaded weights failed checksum")
        with gzip.open(gz, "rb") as src, open(target, "wb") as dst:
            dst.write(src.read())
        if _sha256(target) != WEIGHTS_SHA256:
            raise OpenL3Error("decompressed weights failed checksum")

    # -- model ------------------------------------------------------------
    def _load_model(self):
        from openl3.models import load_audio_embedding_model

        os.environ["OPENL3_MODEL_DIR"] = str(self.model_dir)
        return load_audio_embedding_model(
            self.profile.model_input_repr,
            self.profile.model_content,
            self.profile.model_embedding_size,
            frontend=self.profile.frontend,
        )

    def _model_or_load(self):
        if self._model is None:
            self.ensure_weights()
            self._model = self._load_model()
        return self._model

    # -- analysis ---------------------------------------------------------
    def window_embeddings(self, pcm: np.ndarray, sample_rate: int) -> WindowEmbeddings:
        from openl3.core import get_audio_embedding

        duration = len(pcm) / sample_rate
        if duration < self.profile.pooling.window_seconds:
            return WindowEmbeddings(
                np.zeros((0, self.profile.dimensions), dtype=np.float32),
                np.zeros(0),
            )

        model = self._model_or_load()
        emb, ts = get_audio_embedding(
            np.asarray(pcm, dtype=np.float32),
            int(sample_rate),
            model=model,
            input_repr=self.profile.model_input_repr,
            content_type=self.profile.model_content,
            embedding_size=self.profile.model_embedding_size,
            center=self.profile.center,
            hop_size=self.profile.pooling.hop_seconds,
            batch_size=self.batch_size,
            frontend=self.profile.frontend,
            verbose=0,
        )
        return WindowEmbeddings(
            np.asarray(emb, dtype=np.float32),
            np.asarray(ts, dtype=np.float64),
        )

    def model_identity(self) -> Dict[str, str]:
        try:
            import openl3
            import tensorflow as tf

            openl3_ver = getattr(openl3, "__version__", OPENL3_VERSION)
            tf_ver = tf.__version__
        except ImportError:
            openl3_ver, tf_ver = OPENL3_VERSION, "unknown"
        try:
            import kapre

            kapre_ver = getattr(kapre, "__version__", "unknown")
        except ImportError:
            kapre_ver = "unknown"
        return {
            "name": self.name,
            "openl3": openl3_ver,
            "tensorflow": tf_ver,
            "kapre": kapre_ver,
            "weight_file": WEIGHTS_BASENAME,
            "weight_sha256": WEIGHTS_SHA256,
            "weights_cc_license": "CC BY 4.0",
        }


def _validate_profile(p: Profile) -> None:
    if p.model != "openl3":
        raise ValueError("OpenL3Analyzer requires profile.model='openl3'")
    if p.model_content != "music":
        raise ValueError("OpenL3Analyzer requires model_content='music'")
    if p.model_input_repr != "mel256":
        raise ValueError("OpenL3Analyzer requires model_input_repr='mel256'")
    if p.model_embedding_size != 512:
        raise ValueError("OpenL3Analyzer requires model_embedding_size=512")
    if p.frontend != "kapre":
        # openl3's librosa frontend calls librosa.feature.melspectrogram
        # positionally, which breaks on librosa>=0.10 (keyword-only args).
        # kapre is the working, pinned frontend for this profile.
        raise ValueError(
            "OpenL3Analyzer requires frontend='kapre' "
            "(openl3's librosa frontend is incompatible with librosa>=0.10)"
        )


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()
