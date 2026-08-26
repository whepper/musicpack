#!/usr/bin/env python3
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause
"""Synthesize tests/reference/thrasher-like-10x3s.mpack.

A tiny, deterministic, license-clean stand-in for the real-world package
that exposed the missing-duration queue jump: ten distinct-length tracks
with manifest durations present and correct (the healthy-metadata half of
that investigation). Duration-less variants are deliberately NOT shipped;
they belong to the separate server-side ingest backfill work.

All signals are synthetic fixed-seed sines; nothing is copyrighted.
Stdlib only. Requires the built `mpcenc` binary.

Usage: make_thrasher_like_fixture.py <outdir> [path/to/mpcenc]
"""

import hashlib
import json
import math
import os
import struct
import subprocess
import sys
import tempfile
import wave

RATE = 44100
# Distinct tenths so every cumulative offset differs (no collapse possible).
DURATIONS = [3.0 + 0.1 * i for i in range(10)]
TITLES = [
    "First Light", "Second Wind", "Third Degree", "Fourth Wall",
    "Fifth Avenue", "Sixth Sense", "Seventh Seal", "Eighth Note",
    "Ninth Cloud", "Tenth Frame",
]


def write_wav(path, seconds, seed):
    n = int(seconds * RATE)
    frames = bytearray()
    for i in range(n):
        t = i / RATE
        env = 0.5 * math.exp(-t / max(0.4, seconds * 0.35))  # gentle decay
        l = env * math.sin(2 * math.pi * (220 + 11 * seed) * t)
        r = env * math.sin(2 * math.pi * (246 + 13 * seed) * t)
        frames += struct.pack("<hh", int(l * 28000), int(r * 28000))
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(frames))
    return n


def main(outdir, mpcenc):
    if os.path.exists(outdir):
        print(f"refusing to overwrite existing {outdir}", file=sys.stderr)
        return 1
    tracks = []
    with tempfile.TemporaryDirectory() as tmp:
        for i, seconds in enumerate(DURATIONS):
            wav = os.path.join(tmp, f"raw{i + 1}.wav")
            samples = write_wav(wav, seconds, i)
            mpc = os.path.join(outdir, "audio", f"{i + 1:02d} - {TITLES[i]}.mpc")
            os.makedirs(os.path.dirname(mpc), exist_ok=True)
            subprocess.run(
                [mpcenc, "--standard", "--silent", "--overwrite", wav, mpc],
                check=True,
            )
            sha = hashlib.sha256(open(mpc, "rb").read()).hexdigest()
            duration = round(samples / RATE, 5)
            # Sanity: authored manifest duration must match encoded audio.
            assert abs(duration - seconds) < 1e-4, (duration, seconds)
            tracks.append({
                "track": i + 1,
                "title": TITLES[i],
                "artists": [{"name": "Fixture Artist", "role": "main"}],
                "duration": duration,
                "audio": {"path": os.path.relpath(mpc, outdir), "sha256": sha},
            })

    manifest = {
        "format": "musicpack",
        "version": 1,
        "album": {
            "title": "Thrasher Like (fixture)",
            "artists": [{"name": "Fixture Artist", "role": "main"}],
            "releaseType": "album",
            "originalReleaseDate": "2026-08-26",
            "genres": ["Test"],
        },
        "release": {
            "releaseDate": "2026-08-26",
            "country": "XW",
            "label": "MusicPack Fixtures",
        },
        # NOTE: `identity`/`source` omitted on purpose — closed enums;
        # provenance for this synthetic corpus is this generator script.
        "media": [{"disc": 1, "tracks": tracks}],
    }
    with open(os.path.join(outdir, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    total = sum(DURATIONS)
    print(f"wrote {outdir}: 10 tracks, {total:.1f}s total")
    return 0


if __name__ == "__main__":
    default_enc = os.path.join(os.path.dirname(__file__), "..", "build", "mpcenc", "mpcenc")
    enc = sys.argv[2] if len(sys.argv) > 2 else os.path.abspath(default_enc)
    raise SystemExit(main(os.path.abspath(sys.argv[1]), enc))
