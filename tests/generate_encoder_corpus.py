#!/usr/bin/env python3
"""Generate the deterministic encoder regression corpus (Phase 2).

Extends the existing generate_corpus.py signal set with the categories the
bit-exact encoder milestone requires:

* the full existing corpus (silence, quiet, full-scale, impulse, transients,
  tonal, white noise, high-dynamic, asymmetry, phase-inverted, clipping,
  chirp, odd counts, very short) at 32/44.1/48 kHz;
* pink noise (deterministic Voss-style IIR), low-frequency, near-Nyquist
  high-frequency, and multi-tone material;
* long dense inputs (30 s / 60 s) so encode benchmarks are not
  open/header-overhead dominated;
* frame-boundary edge-case sample counts (1151/1152/1153 and 2303/2304/2305)
  around the MPC frame length;
* a true mono WAV.

All signals are synthetic and deterministic (fixed seeds). Run with a
Python 3 that has the `wave` module (stdlib).

Usage: generate_encoder_corpus.py <outdir>
"""

import math
import os
import random
import sys

from generate_corpus import RATES, SIGNALS, make_signal, write_wav

FRAME = 1152  # MPC frame length (36 * 32)

# ---------------------------------------------------------------------------
# extra deterministic signals (the kinds below are not in generate_corpus)
# ---------------------------------------------------------------------------

def pink_noise(rate, dur, seed=2718):
    """Deterministic pink noise (Paul Kellet IIR approximation)."""
    rng = random.Random(seed)
    n = int(rate * dur)
    b = [0.99765, 0.96300, 0.57000, 0.43000, 0.25000]
    a = [0.99886, 0.99576, 0.98952, 0.97682]
    x1 = x2 = x3 = x4 = x5 = 0.0
    out = []
    for _ in range(n):
        white = rng.uniform(-1, 1)
        x1 = b[0] * white + a[0] * x1
        x2 = b[1] * white + a[1] * x2
        x3 = b[2] * white + a[2] * x3
        x4 = b[3] * white + a[3] * x4
        x5 = b[4] * white
        v = (x1 + x2 + x3 + x4 + x5) * 0.11
        out.append((v, v))
    return out


def make_extra_signal(kind, rate, dur, seed=1234):
    n = int(rate * dur)
    out = []
    for i in range(n):
        t = i / rate
        if kind == "pink_noise":
            return pink_noise(rate, dur, seed)
        elif kind == "low_freq":
            l = r = 0.6 * math.sin(2 * math.pi * 30 * t)
        elif kind == "low_freq_stereo":
            l = 0.6 * math.sin(2 * math.pi * 30 * t)
            r = 0.6 * math.sin(2 * math.pi * 33 * t)
        elif kind == "high_freq_near_limit":
            f = 20000.0 if rate >= 44100 else 15000.0
            l = r = 0.4 * math.sin(2 * math.pi * f * t)
        elif kind == "multi_tone":
            l = 0.2 * (math.sin(2 * math.pi * 200 * t) + math.sin(2 * math.pi * 440 * t)
                       + math.sin(2 * math.pi * 1000 * t) + math.sin(2 * math.pi * 3000 * t))
            r = 0.2 * (math.sin(2 * math.pi * 300 * t) + math.sin(2 * math.pi * 660 * t)
                       + math.sin(2 * math.pi * 1500 * t) + math.sin(2 * math.pi * 4500 * t))
        elif kind == "dense":
            rng = random.Random(seed)
            for i in range(n):
                t = i / rate
                l = (0.5 * math.sin(2 * math.pi * 440 * t) + 0.3 * math.sin(2 * math.pi * 2000 * t)
                     + 0.08 * rng.uniform(-1, 1))
                r = (0.4 * math.sin(2 * math.pi * 660 * t) + 0.25 * math.sin(2 * math.pi * 3000 * t)
                     + 0.08 * rng.uniform(-1, 1))
                out.append((l, r))
            return out
        else:
            raise ValueError(kind)
        out.append((l, r))
    return out


def short_frame_boundary(kind, rate, nsamples, seed=7):
    """Deterministic signal of an exact sample count (frame-boundary tests)."""
    rng = random.Random(seed)
    out = []
    for i in range(nsamples):
        t = i / rate
        if kind == "frame_sine":
            l = r = 0.5 * math.sin(2 * math.pi * 440 * t)
        elif kind == "frame_noise":
            v = 0.4 * rng.uniform(-1, 1)
            l = r = v
        else:
            raise ValueError(kind)
        out.append((l, r))
    return out


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "encoder-corpus"
    os.makedirs(outdir, exist_ok=True)

    # 1. the full existing corpus (unchanged names, so the existing compat
    #    test and this one share the deterministic base set).
    for kind, dur in SIGNALS:
        for rate in RATES:
            odd = kind.startswith("odd_")
            base = kind.replace("odd_", "").replace("_short", "")
            samples = make_signal(base, rate, dur)
            if odd:
                samples.append((0.0, 0.0))
            name = "%s_%d" % (kind, rate)
            write_wav(os.path.join(outdir, name + ".wav"), samples, rate)
            print(name)

    # 2. extended spectral/noise material.
    for kind in ("pink_noise", "low_freq", "low_freq_stereo",
                 "high_freq_near_limit", "multi_tone"):
        for rate in RATES:
            name = "%s_%d" % (kind, rate)
            write_wav(os.path.join(outdir, name + ".wav"),
                      make_extra_signal(kind, rate, 2.0), rate)
            print(name)

    # 3. long dense inputs (benchmark work: fixed open/header overhead).
    for rate, dur in ((44100, 30), (44100, 60), (48000, 30)):
        name = "long_%ds_%d" % (dur, rate)
        write_wav(os.path.join(outdir, name + ".wav"),
                  make_extra_signal("dense", rate, dur, seed=99), rate)
        print(name)

    # 4. frame-boundary edge cases (MPC frame length 1152).
    for kind in ("frame_sine", "frame_noise"):
        for n in (FRAME - 1, FRAME, FRAME + 1, 2 * FRAME - 1, 2 * FRAME, 2 * FRAME + 1):
            name = "%s_%d_%d" % (kind, n, 44100)
            write_wav(os.path.join(outdir, name + ".wav"),
                      short_frame_boundary(kind, 44100, n), 44100)
            print(name)

    # 5. a true mono WAV.
    samples = make_signal("mono_sine", 44100, 2.0)
    mono = [(s[0],) for s in samples]
    write_wav(os.path.join(outdir, "mono_44100.wav"), mono, 44100, nch=1)
    print("mono_44100")

    print("done -> %s" % outdir)


if __name__ == "__main__":
    main()
