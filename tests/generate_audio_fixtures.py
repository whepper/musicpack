#!/usr/bin/env python3
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause
"""Generate the committed decode-pipeline fixtures under tests/fixtures/audio/.

These exercise the native FLAC/WAV decoder (libmusicpack `audio` module)
across bit depths, sample rates, container variants and rejection cases:

  flac16-44k.flac      16-bit stereo 44.1 kHz
  flac24-48k.flac      24-bit stereo 48 kHz
  flac24-96k.flac      24-bit stereo 96 kHz
  flac-mono-44k.flac   mono 16-bit 44.1 kHz
  wav16-44k.wav        16-bit stereo 44.1 kHz PCM
  wav24-44k.wav        24-bit stereo 44.1 kHz PCM
  wav24-ext.wav        24-bit stereo WAVE_FORMAT_EXTENSIBLE PCM
  wav-float.wav        32-bit IEEE-float stereo (loudness-only)
  wav-adpcm.wav        ADPCM WAV (must be rejected)
  wav-truncated.wav    16-bit PCM WAV cut short mid-data (graceful EOF)

FLAC synthesis uses ffmpeg (a one-time developer tool; the outputs below are
committed so CI and tests never need ffmpeg). The WAV edge cases are built
by hand in Python. Fixtures contain short generated sine tones only.

Usage: python3 tests/generate_audio_fixtures.py [out-dir]
"""

import argparse
import math
import os
import struct
import subprocess
import sys


def run(args):
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print("FAIL %s: %s" % (args, r.stderr), file=sys.stderr)
        sys.exit(1)


def ffmpeg_available():
    try:
        subprocess.run(["ffmpeg", "-version"], capture_output=True, check=True)
        return True
    except (OSError, subprocess.CalledProcessError):
        return False


def make_flac(path, freq, rate, bits, channels, duration=2.0):
    run([
        "ffmpeg", "-v", "error", "-y",
        "-f", "lavfi",
        "-i", f"sine=frequency={freq}:duration={duration}:sample_rate={rate}",
        "-ac", str(channels), "-sample_fmt",
        "s16" if bits == 16 else "s32",
        "-c:a", "flac", path,
    ])


def sine_frames(freq, rate, channels, bits, duration=2.0, amp=0.5, seed=7):
    import random
    rng = random.Random(seed)
    n = int(rate * duration)
    out = []
    for i in range(n):
        t = i / rate
        v = amp * math.sin(2 * math.pi * freq * t) + 0.02 * rng.uniform(-1, 1)
        out.append(v)
    return out


def write_pcm_wav(path, rate, channels, bits, frames, extensible=False):
    """frames: list of (list per channel) float samples in [-1,1]."""
    n = len(frames)
    bytes_per = bits // 8
    if bits == 24 and extensible:
        fmt_tag = 0xFFFE
        fmt = struct.pack("<HHIIHH", 0xFFFE, channels, rate,
                          rate * channels * bytes_per, channels * bytes_per,
                          bits) + struct.pack("<HHI", 22, 24, 0) + \
              struct.pack("<HH", 0x0001, 0x0000) + \
              struct.pack("<BBBBBBBBBBBB", 0x00, 0x00, 0x00, 0x00, 0x10, 0x80,
                          0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b)
        fmt_size = 40
    else:
        fmt_tag = 1
        fmt = struct.pack("<HHIIHH", 1, channels, rate,
                          rate * channels * bytes_per, channels * bytes_per, bits)
        fmt_size = 16
    data = bytearray()
    for v in frames:
        for c in range(channels):
            val = max(-1.0, min(1.0, v[c]))
            if bits == 8:
                data.append(int(val * 127) + 128)
            elif bits == 16:
                data += struct.pack("<h", int(val * 32767))
            elif bits == 24:
                iv = int(val * 8388607)
                data += bytes((iv & 0xff, (iv >> 8) & 0xff, (iv >> 16) & 0xff))
    riff_size = 4 + (8 + fmt_size) + (8 + len(data))
    with open(path, "wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", riff_size))
        f.write(b"WAVE")
        f.write(b"fmt ")
        f.write(struct.pack("<I", fmt_size))
        f.write(fmt)
        f.write(b"data")
        f.write(struct.pack("<I", len(data)))
        f.write(bytes(data))


def write_float_wav(path, rate, channels, frames):
    fmt = struct.pack("<HHIIHH", 3, channels, rate,
                      rate * channels * 4, channels * 4, 32)
    data = bytearray()
    for v in frames:
        for c in range(channels):
            data += struct.pack("<f", v[c])
    riff_size = 4 + (8 + 16) + (8 + len(data))
    with open(path, "wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", riff_size))
        f.write(b"WAVE")
        f.write(b"fmt ")
        f.write(struct.pack("<I", 16))
        f.write(fmt)
        f.write(b"data")
        f.write(struct.pack("<I", len(data)))
        f.write(bytes(data))


def write_adpcm_wav(path, rate, channels):
    # fmt tag 2 = ADPCM (4-bit), must be rejected by the native reader.
    fmt = struct.pack("<HHIIHH", 2, channels, rate, rate * channels * 1,
                      channels, 4)
    with open(path, "wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", 4 + (8 + 16) + (8 + 16)))
        f.write(b"WAVE")
        f.write(b"fmt ")
        f.write(struct.pack("<I", 16))
        f.write(fmt)
        f.write(b"data")
        f.write(struct.pack("<I", 16))
        f.write(b"\x00" * 16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out_dir", nargs="?", default=None)
    args = ap.parse_args()

    if not ffmpeg_available():
        sys.exit("error: ffmpeg is required to regenerate the audio fixtures")

    out = args.out_dir or os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "fixtures", "audio")
    os.makedirs(out, exist_ok=True)

    make_flac(os.path.join(out, "flac16-44k.flac"), 440, 44100, 16, 2)
    make_flac(os.path.join(out, "flac24-48k.flac"), 330, 48000, 24, 2)
    make_flac(os.path.join(out, "flac24-96k.flac"), 220, 96000, 24, 2)
    make_flac(os.path.join(out, "flac-mono-44k.flac"), 550, 44100, 16, 1)

    stereo16 = sine_frames(440, 44100, 2, 16)
    stereo16 = [[v, v * 0.9] for v in stereo16]
    stereo24 = sine_frames(330, 44100, 2, 24)
    stereo24 = [[v, v * 0.8] for v in stereo24]
    write_pcm_wav(os.path.join(out, "wav16-44k.wav"), 44100, 2, 16, stereo16)
    write_pcm_wav(os.path.join(out, "wav24-44k.wav"), 44100, 2, 24, stereo24)
    write_pcm_wav(os.path.join(out, "wav24-ext.wav"), 44100, 2, 24, stereo24,
                  extensible=True)
    write_float_wav(os.path.join(out, "wav-float.wav"), 44100, 2,
                    [[v[0], v[1]] for v in stereo16])
    write_adpcm_wav(os.path.join(out, "wav-adpcm.wav"), 44100, 2)

    # truncated: wav16-44k cut mid-data
    src = os.path.join(out, "wav16-44k.wav")
    with open(src, "rb") as f:
        full = f.read()
    with open(os.path.join(out, "wav-truncated.wav"), "wb") as f:
        f.write(full[: len(full) // 2])

    print("generated audio fixtures under %s" % out)


if __name__ == "__main__":
    main()
