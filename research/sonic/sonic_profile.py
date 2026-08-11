"""Sonic analysis profile: the complete, comparable description of how an
embedding was produced.

Embeddings are comparable only when their full profile is identical. The
profile therefore covers model, weights, input preprocessing, sample rate,
windowing, hop, centering, silence handling, pooling, normalization and
dimensions. ``Profile.fingerprint`` is a canonical SHA-256 over the whole
profile; ``Profile.id`` is a compact stable identifier derived from it.

Two vectors produced under different profiles must never be compared — see
``Profile.compatible_with``.
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import asdict, dataclass, field
from typing import Any, Dict

PROFILE_ID_PREFIX = "musicpack-sonic-v1"
DISTANCE_COSINE = "cosine"

POOL_MEAN_NORM = "mean-norm"
POOL_MEAN = "mean"
POOL_ROBUST_MEAN = "robust-mean"
POOL_STRATEGIES = (POOL_MEAN_NORM, POOL_MEAN, POOL_ROBUST_MEAN)


@dataclass(frozen=True)
class SilenceParams:
    """Deterministic energy-based window exclusion.

    Windows whose RMS falls below ``threshold_db`` are excluded from the
    embedding aggregation. When ``relative_to_median`` is set, the threshold
    is ``median(window_rms) + threshold_db`` (so ``threshold_db`` is a delta
    below the track's own level, e.g. -20 dB); otherwise it is an absolute
    dBFS floor. ``window_seconds`` must match the pooling window.
    """

    enabled: bool = False
    threshold_db: float = -20.0
    relative_to_median: bool = True
    window_seconds: float = 1.0


@dataclass(frozen=True)
class PoolingParams:
    """Deterministic window-to-track aggregation."""

    strategy: str = POOL_MEAN_NORM  # one of POOL_STRATEGIES
    hop_seconds: float = 1.0
    window_seconds: float = 1.0
    robust_trim: float = 0.0  # fraction of extreme windows trimmed (robust-mean)
    silence: SilenceParams = field(default_factory=SilenceParams)


@dataclass(frozen=True)
class Profile:
    """Everything that affects a sonic embedding.

    ``model_weights_sha256`` is empty until the weights have been downloaded
    and recorded (see analyzers.openl3); it is part of the fingerprint, so
    recording a different weight file invalidates all prior comparisons.
    """

    model: str = "openl3"
    model_variant: str = ""  # e.g. "multi" / "release" for discogs-effnet
    model_content: str = "music"
    model_input_repr: str = "mel256"
    model_embedding_size: int = 512
    model_sample_rate: int = 48000
    frontend: str = "kapre"  # "kapre" | "librosa"
    center: bool = True
    model_weights_sha256: str = ""
    distance: str = DISTANCE_COSINE
    pooling: PoolingParams = field(default_factory=PoolingParams)

    @property
    def dimensions(self) -> int:
        return self.model_embedding_size

    @classmethod
    def from_dict(cls, d: Dict[str, Any]) -> "Profile":
        """Rebuild a Profile from ``canonical()``/``asdict`` output."""
        pooling = d.get("pooling") or {}
        silence = pooling.get("silence") or {}
        return cls(
            model=d.get("model", "openl3"),
            model_variant=d.get("model_variant", ""),
            model_content=d.get("model_content", "music"),
            model_input_repr=d.get("model_input_repr", "mel256"),
            model_embedding_size=d.get("model_embedding_size", 512),
            model_sample_rate=d.get("model_sample_rate", 48000),
            frontend=d.get("frontend", "kapre"),
            center=d.get("center", True),
            model_weights_sha256=d.get("model_weights_sha256", ""),
            distance=d.get("distance", DISTANCE_COSINE),
            pooling=PoolingParams(
                strategy=pooling.get("strategy", POOL_MEAN_NORM),
                hop_seconds=pooling.get("hop_seconds", 1.0),
                window_seconds=pooling.get("window_seconds", 1.0),
                robust_trim=pooling.get("robust_trim", 0.0),
                silence=SilenceParams(
                    enabled=silence.get("enabled", False),
                    threshold_db=silence.get("threshold_db", -20.0),
                    relative_to_median=silence.get("relative_to_median", True),
                    window_seconds=silence.get("window_seconds", 1.0),
                ),
            ),
        )

    def canonical(self) -> Dict[str, Any]:
        return asdict(self)

    def canonical_json(self) -> str:
        """Stable JSON (sorted keys) over the entire profile."""
        return json.dumps(self.canonical(), sort_keys=True, separators=(",", ":"))

    def fingerprint(self) -> str:
        """Full-profile SHA-256 (hex). Any change => different fingerprint."""
        return hashlib.sha256(self.canonical_json().encode("utf-8")).hexdigest()

    @property
    def id(self) -> str:
        return "%s-%s" % (PROFILE_ID_PREFIX, self.fingerprint()[:16])

    def compatible_with(self, other: "Profile") -> bool:
        """Vectors are comparable only when the full profiles match."""
        return self.fingerprint() == other.fingerprint()

    def model_key(self) -> str:
        """Fingerprint of everything that affects *window* embeddings (the
        model part, not pooling/silence). Used for the window cache so that
        pooling experiments reuse model output."""
        model_only = asdict(self)
        model_only["pooling"] = None
        return hashlib.sha256(
            json.dumps(model_only, sort_keys=True, separators=(",", ":")).encode("utf-8")
        ).hexdigest()
