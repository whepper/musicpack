"""Two-level on-disk embedding cache.

  Level 1 — window embeddings, keyed by (audio content sha256, model_key).
            Reused across pooling experiments so the model never re-runs for
            a hop/pooling sweep.
  Level 2 — pooled track embeddings, keyed by (audio sha256, full profile
            fingerprint including pooling and silence). Changing any profile
            parameter, hop, pooling choice, silence rule or audio changes the
            key.

``audio_sha256`` is the SHA-256 of the audio *content bytes* (the source
file, before decoding), so an unchanged track never re-decodes/re-analyzes.
"""

from __future__ import annotations

import io
import os
import threading
from pathlib import Path
from typing import Optional

import numpy as np

from pooling import WindowEmbeddings
from profile import Profile

MISSING = object()


class Cache:
    def __init__(self, root: Path):
        self.root = Path(root)
        self._lock = threading.Lock()

    # -- level 1: window embeddings --------------------------------------
    def window_path(self, audio_sha256: str, model_key: str) -> Path:
        return self.root / "windows" / model_key[:16] / (audio_sha256 + ".npz")

    def load_windows(self, audio_sha256: str, model_key: str) -> Optional[WindowEmbeddings]:
        path = self.window_path(audio_sha256, model_key)
        if not path.is_file():
            return None
        try:
            data = np.load(path)
            return WindowEmbeddings(data["embeddings"], data["timestamps"])
        except (ValueError, OSError, KeyError):
            return None

    def store_windows(
        self, audio_sha256: str, model_key: str, windows: WindowEmbeddings
    ) -> None:
        path = self.window_path(audio_sha256, model_key)
        os.makedirs(path.parent, exist_ok=True)
        buf = io.BytesIO()
        np.savez(buf, embeddings=windows.embeddings, timestamps=windows.timestamps)
        self._atomic_write(path, buf.getvalue())

    # -- level 2: pooled track embeddings --------------------------------
    def track_path(self, audio_sha256: str, profile: Profile) -> Path:
        return self.root / "tracks" / profile.fingerprint()[:16] / (audio_sha256 + ".npz")

    def load_track(self, audio_sha256: str, profile: Profile):
        """Returns the pooled float32 vector, MISSING if not cached, or None
        if cached as "analyzed but no embedding produced"."""
        path = self.track_path(audio_sha256, profile)
        if not path.is_file():
            return MISSING
        try:
            data = np.load(path)
            if "embedding" not in data:
                return None
            return data["embedding"].astype(np.float32)
        except (ValueError, OSError, KeyError):
            return MISSING

    def store_track(self, audio_sha256: str, profile: Profile, vector: Optional[np.ndarray]) -> None:
        path = self.track_path(audio_sha256, profile)
        os.makedirs(path.parent, exist_ok=True)
        buf = io.BytesIO()
        if vector is None:
            np.savez(buf, no_embedding=1)
        else:
            np.savez(buf, embedding=np.asarray(vector, dtype=np.float32))
        self._atomic_write(path, buf.getvalue())

    def _atomic_write(self, path: Path, payload: bytes) -> None:
        tmp = path.with_suffix(path.suffix + ".tmp")
        tmp.write_bytes(payload)
        with self._lock:
            os.replace(tmp, path)
