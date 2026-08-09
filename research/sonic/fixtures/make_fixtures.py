#!/usr/bin/env python3
"""Generate a small deterministic synthetic music library for exercising the
benchmark harness (no copyrighted audio; public-domain-by-construction).

Layout produced under fixtures/music/:
    <artist>/<album>/NN - Title.wav

Tracks are sums of sines whose base frequency characterises the album, so
embeddings cluster per album — enough for the diagnostics to run meaningfully
on synthetic data.

Usage:
  python research/sonic/fixtures/make_fixtures.py [--out fixtures/music]
"""

import argparse
import math
import os
import wave
from pathlib import Path

SR = 44100


def synth(duration_s: float, base: float, seed: int, amp: float = 0.5) -> bytes:
    import random
    rng = random.Random(seed)
    n = int(duration_s * SR)
    frames = bytearray()
    for i in range(n):
        t = i / SR
        s = amp * 0.45 * math.sin(2 * math.pi * base * t)
        s += amp * 0.18 * math.sin(2 * math.pi * base * 1.5 * t)
        s += amp * 0.18 * math.sin(2 * math.pi * base * 2.7 * t)
        s += amp * 0.05 * rng.uniform(-1, 1)
        # add a slow amplitude envelope so windows are not identical
        s *= 0.5 + 0.5 * math.sin(2 * math.pi * (i / n) * 0.5)
        v = int(s * 32767)
        frames += v.to_bytes(2, "little", signed=True)
        frames += v.to_bytes(2, "little", signed=True)  # stereo (l == r)
    return bytes(frames)


def write_wav(path: Path, duration_s: float, base: float, seed: int):
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(synth(duration_s, base, seed))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "music"))
    args = ap.parse_args()
    out = Path(args.out)

    # (artist, album, [(track, base_freq, duration_s), ...])
    library = [
        ("Artist A", "Album One", [(1, 220.0, 8.0), (2, 220.0, 8.0), (3, 220.0, 8.0)]),
        ("Artist A", "Album Two", [(1, 260.0, 8.0), (2, 260.0, 8.0), (3, 260.0, 8.0)]),
        ("Artist B", "Album Three", [(1, 400.0, 8.0), (2, 400.0, 8.0),
                                     (3, 400.0, 8.0), (4, 400.0, 8.0)]),
        ("Artist C", "Album Four", [(1, 80.0, 8.0), (2, 80.0, 8.0),
                                    (3, 80.0, 8.0), (4, 80.0, 8.0)]),
        ("Artist D", "Edge Cases", [(1, 440.0, 0.5),  # shorter than window
                                    (2, 110.0, 8.0)]),
    ]
    seed = 0
    count = 0
    for artist, album, tracks in library:
        for num, base, dur in tracks:
            rel = "%s/%s/%02d - Title.wav" % (artist, album, num)
            write_wav(out / rel, dur, base, seed)
            count += 1
            seed += 1
    print("generated %d tracks under %s" % (count, out))


if __name__ == "__main__":
    main()
