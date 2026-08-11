"""Analyzer interface: turns decoded PCM into window embeddings.

Concrete analyzers (openl3, essentia_discogs) implement
``window_embeddings`` and report their model identity for the report. All
profile-affecting parameters live in ``Profile``, never hidden in the
analyzer.
"""

from __future__ import annotations

from abc import ABC, abstractmethod
from typing import Dict

import numpy as np

from pooling import WindowEmbeddings
from sonic_profile import Profile


class Analyzer(ABC):
    """A concrete embedding model wrapped for the benchmark."""

    profile: Profile

    @property
    @abstractmethod
    def name(self) -> str:
        """Short analyzer label, e.g. 'openl3' or 'discogs-multi'."""

    @abstractmethod
    def window_embeddings(self, pcm: np.ndarray, sample_rate: int) -> WindowEmbeddings:
        """Embed ``pcm`` (mono float32) into a sequence of window
        embeddings over time."""

    @abstractmethod
    def model_identity(self) -> Dict[str, str]:
        """Version/weight identity recorded in reports and the profile
        (e.g. package version, weight file, weight SHA-256)."""

    @abstractmethod
    def available(self) -> bool:
        """True when the analyzer's dependencies are installed."""

    def describe_unavailable(self) -> str:
        return "%s not available in this environment" % self.name
