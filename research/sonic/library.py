"""Scan an arbitrary local music directory into albums/tracks.

Metadata for the quantitative diagnostics comes from the directory layout
(``artist/album/track.*``) enriched by embedded tags via ``ffprobe`` when
available. This is deterministic: directory structure is always the base,
tags only override title/artist/album/genre when present.

No copyrighted data enters the repository; the harness only ever consumes a
user-supplied path.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional

from decode import SUPPORTED

_TRACK_NUM = re.compile(r"^\s*(\d{1,3})")


@dataclass
class Track:
    path: Path
    artist: str
    album: str
    title: str = ""
    genres: List[str] = field(default_factory=list)
    duration: Optional[float] = None
    track_number: int = 0
    disc: int = 1
    sha256: str = ""

    @property
    def id(self) -> str:
        return str(self.path)

    @property
    def label(self) -> str:
        return "%s — %s" % (self.artist, self.title or self.path.stem)


@dataclass
class Album:
    artist: str
    title: str
    tracks: List[Track] = field(default_factory=list)

    @property
    def id(self) -> str:
        return "%s / %s" % (self.artist, self.title)


def _title_from_filename(stem: str) -> str:
    m = _TRACK_NUM.match(stem)
    if m:
        return stem[m.end():].lstrip("- ._")
    return stem


def _ffprobe_meta(path: Path) -> Dict[str, str]:
    try:
        proc = subprocess.run(
            ["ffprobe", "-v", "quiet", "-print_format", "json", "-show_format", str(path)],
            capture_output=True, text=True, timeout=20,
        )
        if proc.returncode != 0:
            return {}
        data = json.loads(proc.stdout)
        tags = data.get("format", {}).get("tags", {}) or {}
        return {str(k).lower(): str(v) for k, v in tags.items()}
    except (OSError, subprocess.SubprocessError, ValueError, json.JSONDecodeError):
        return {}


def _parse_genres(raw: Optional[str]) -> List[str]:
    if not raw:
        return []
    parts = re.split(r"[;/,\uFF0C]", raw)
    return [p.strip() for p in parts if p.strip()]


def _ffprobe_duration(path: Path) -> Optional[float]:
    try:
        proc = subprocess.run(
            ["ffprobe", "-v", "quiet", "-print_format", "json", "-show_format", str(path)],
            capture_output=True, text=True, timeout=20,
        )
        if proc.returncode != 0:
            return None
        dur = json.loads(proc.stdout).get("format", {}).get("duration")
        return float(dur) if dur else None
    except (OSError, subprocess.SubprocessError, ValueError, json.JSONDecodeError):
        return None


def scan(root: Path, use_tags: bool = True) -> List[Album]:
    """Walk ``root`` and group audio files into (artist, album) buckets."""
    root = Path(root).resolve()
    by_key: Dict[tuple, Album] = {}

    for dirpath, _dirnames, filenames in os.walk(root):
        dirpath = Path(dirpath)
        rel = dirpath.relative_to(root)
        for name in sorted(filenames):
            ext = Path(name).suffix.lower()
            if ext not in SUPPORTED:
                continue
            path = dirpath / name
            album_name = rel.name if rel.parts else root.name
            artist = (
                rel.parent.name if len(rel.parts) >= 2 else album_name
            )
            key = (artist, album_name)
            album = by_key.setdefault(key, Album(artist=artist, title=album_name))
            album.tracks.append(_make_track(path, artist, album_name, use_tags))
    return sorted(by_key.values(), key=lambda a: (a.artist.lower(), a.title.lower()))


def _make_track(path: Path, artist: str, album: str, use_tags: bool) -> Track:
    meta = _ffprobe_meta(path) if use_tags else {}
    stem = path.stem
    track = Track(
        path=path,
        artist=meta.get("artist") or artist,
        album=meta.get("album") or album,
        title=meta.get("title") or _title_from_filename(stem),
        genres=_parse_genres(meta.get("genre")),
        duration=_ffprobe_duration(path) if use_tags else None,
        track_number=int(_TRACK_NUM.match(stem).group(1)) if _TRACK_NUM.match(stem) else 0,
    )
    return track
