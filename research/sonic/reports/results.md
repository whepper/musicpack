# MusicPack Sonic — research results

**Date:** 2026-08-09 · **Status:** evidence collected; profile **not yet frozen**.
Companion raw data: `raw/` (gitignored). Human evaluation is set up and
pending a human reviewer.

## 1. Candidate models

| Model | Source | License | SR | Input | Dims | Role |
|-------|--------|---------|----|-------|------|------|
| OpenL3 `music`/`mel256`/`emb512` | marl/openl3 | code MIT; weights CC BY 4.0 | 48000 | mel256 (kapre) | 512 | primary candidate |
| Discogs-EffNet `multi` | MTG/Essentia | Essentia AGPL-3.0; weights CC BY-NC-SA 4.0 | 16000 | 96-band log-mel | 1280 | eval-only comparator |
| Discogs-EffNet `release` | MTG/Essentia | CC BY-NC-SA 4.0 | 16000 | 96-band log-mel | 1280 | eval-only comparator |

Verified technical facts: OpenL3 window 1 s, 48 kHz (resampy `kaiser_best`),
mel256 kapre (n_fft 2048, hop 242, decibel, pad_end), `center=True`; window
embeddings are **not** L2-normalized; the librosa frontend is incompatible
with librosa ≥ 0.10 (kapre pinned). Discogs-EffNet: 131 mel-frame patches
(2.096 s) striding 61 (0.976 s), deterministic frame-count formula, output
`PartitionedCall:1` (1280-dim), not normalized.

## 2. Benchmark environment

- Host: Darwin/arm64 25.5.0 (Apple Silicon Mac)
- CPython 3.11.1; openl3 0.4.0; tensorflow 2.15.1; kapre 0.3.6;
  librosa 0.10.2; numpy 1.26.4; soundfile 0.14.0
- Essentia comparator (separate venv): essentia-tensorflow 2.1b6.dev1389,
  numpy 2.4.6
- Full pin set: `environment.lock.txt`

## 3. Dataset

A **200-track representative stratified subset** (≤3 tracks/album,
≤2 albums/artist) of a local 2691-track / 210-album / 91-artist library
(100 % Musepack SV8, produced by `flac2mpc`). Evaluated subset: 200 tracks,
70 albums, 49 artists. Genres from APEv2 tags (coarse: Rock, Pop,
Electronic, …). No audio, tags or filenames are committed.

## 4. Results

### Quantitative retrieval (200 tracks, k=10)

| pooling/hop/silence | same_album@10 | same_artist@10 | genre_purity@10 | album_coherence@10 |
|---|---|---|---|---|
| **discogs-multi hop1 mean-norm nosil** | **0.183** | **0.252** | **0.523** | 0.049 |
| discogs-release hop1 mean-norm nosil | 0.183 | 0.241 | 0.504 | 0.050 |
| openl3 hop1 mean-norm nosil | 0.128 | 0.170 | 0.390 | 0.036 |
| openl3 hop0.5 mean-norm nosil | 0.128 | 0.170 | 0.390 | 0.036 |
| openl3 hop1 robust-mean nosil | 0.127 | 0.167 | 0.394 | 0.033 |

(All 16 profiles in `evaluate-musicpac.md`; discogs variants are within
±0.002 of each other, openl3 pooling/hop/silence within ±0.004.)

- **Discogs-EffNet beats OpenL3 on every diagnostic** (same-album +43 %,
  same-artist +48 %, genre purity +34 %). The comparator explicitly trained
  for music similarity is quantitatively stronger — as designed.
- **OpenL3 pooling, hop and silence choices barely matter** (all within
  0.004). hop 0.5 == hop 1.0; mean-norm ≈ mean ≈ robust-mean; silence
  none ≈ rel-20. This strongly supports the cheapest profile.
- These are diagnostics, not ground truth; the absolute values are low
  because each album contributes only ~3 tracks to the 200-track pool.

### Human evaluation

Blind-mode HTML generated (`reports/human/human-eval-musicpac.html`,
**27 seeds × 3 methods**, profile ids hidden, 0–3 scoring + A/B/tie,
ratings downloadable). **Pending a human reviewer** — this is the decisive
test of whether OpenL3's recommendations are musically convincing despite
the quantitative gap.

### Cross-codec stability (20 real tracks)

| metric | mean | min | max |
|---|---|---|---|
| cosine(source, FLAC) | 0.9999 | 0.9999 | 1.0000 |
| cosine(source, MPC Q6) | 0.9998 | 0.9995 | 0.9999 |
| cosine(FLAC, MPC Q6) | 0.9998 | 0.9995 | 1.0000 |

Even *double-encoded* MPC (already-lossy source re-encoded at Q6) keeps a
0.9995+ embedding cosine. **Sonic analysis may safely run after
`flac2mpc`.**

### Runtime (openl3, hop 1.0)

- ~0.055× realtime (3.3 s of analysis per minute of music; 15 s per
  ~4.5-min track); hop 0.5 doubles this for no measured gain
- Peak RSS ≈ 1.9 GB (TensorFlow runtime) — a per-track analysis cost, not
  a streaming one
- For MusicPack Author (analyse once during authoring) this is acceptable:
  a 10-track album ≈ 2.5 min of analysis

### Storage (512-dim, base64-float32-le baseline)

| album | JSON decimal | base64-f32le | binary-f32le |
|---|---|---|---|
| 10-track | 108.2 kB (48.3 kB gz) | 27.8 kB (20.7 kB gz) | 20.5 kB (19.0 kB gz) |
| 20-track | 215.9 kB (95.1 kB gz) | 55.5 kB (41.3 kB gz) | 41.0 kB (37.9 kB gz) |
| 100-track | 1080 kB (470 kB gz) | 277 kB (206 kB gz) | 205 kB (190 kB gz) |

## 5. Recommendation

> **Recommended Sonic v1 profile:** OpenL3 `music`/`mel256`/`emb512`,
> hop 1.0 s, pooling mean-norm, silence gate off, 48 kHz, kapre frontend,
> base64-float32-le storage, cosine distance — the cheapest fully
> permissive profile, with no measurable quality loss vs the expensive
> variants.

**However**, the evidence does **not** support freezing yet:

- The only model explicitly trained for music similarity
  (Discogs-EffNet, CC BY-NC-SA — non-commercial) **outperforms OpenL3 on
  every quantitative diagnostic**. OpenL3 remains the only permissive
  candidate tested.
- Human evaluation (pending) is the decisive test and has not been done.
- Therefore: **"insufficient evidence for a final freeze"** — adopt the
  OpenL3 profile above as the working v1 baseline (permissive, cheap,
  cross-codec-stable), record the Discogs-EffNet gap as a known weakness,
  and **re-evaluate a permissively licensed music-similarity-trained model**
  (e.g. a future CLAP/MERT-class model with an OSI/CC-BY license) before
  the spec becomes normative.

The draft `specs/musicpack-sonic-v1.md` reflects this: DRAFT — RESEARCH
PHASE — NOT YET NORMATIVE.
