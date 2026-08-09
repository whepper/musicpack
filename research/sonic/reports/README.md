# MusicPack Sonic — research report template

This directory holds the **evidence** for the `musicpack-sonic-v1` profile
decision. Raw per-track data lives in `raw/` (gitignored); the aggregate
Markdown report derived from it is committed. Human-evaluation artifacts live
in `human/` (gitignored: they contain track names and ratings).

How a report is produced (see `../benchmark.py --help`):

```sh
research/sonic/.venv/bin/python research/sonic/benchmark.py analyze \
    --library "$MUSIC_LIBRARY" --analyzer all
research/sonic/.venv/bin/python research/sonic/benchmark.py evaluate \
    --library "$MUSIC_LIBRARY"
research/sonic/.venv/bin/python research/sonic/benchmark.py cross-codec \
    --library "$MUSIC_LIBRARY"
research/sonic/.venv/bin/python research/sonic/benchmark.py efficiency \
    --library "$MUSIC_LIBRARY"
research/sonic/.venv/bin/python research/sonic/benchmark.py float-repr \
    --library "$MUSIC_LIBRARY"
research/sonic/.venv/bin/python research/sonic/benchmark.py human \
    --library "$MUSIC_LIBRARY" --seeds <track>...
```

The sections below are the template every report should fill in.

---

## 1. Candidate models

| Model | Source | License | Sample rate | Input | Dims | Notes |
|-------|--------|---------|-------------|-------|------|-------|
| OpenL3 `music`/`mel256`/`emb512` | marl/openl3 | code MIT; weights CC BY 4.0 | 48000 | mel256 | 512 | primary candidate |
| Discogs-EffNet `multi`/`release` | MTG/Essentia | Essentia AGPL; weights CC BY-NC-SA 4.0 | 16000 | mono waveform | TBD (metadata) | optional comparator |

Technical characteristics to record: window length, hop, centering, frontend
(kapre vs librosa), whether window embeddings are L2-normalized by the model,
weight-file SHA-256, exact package versions.

## 2. Benchmark environment

Mac model/architecture, OS version, CPython, pinned dependency versions
(`environment.lock.txt`), TF/Keras versions, hardware.

## 3. Dataset

Aggregate, non-copyrighted description only — never private filenames:

- number of tracks / albums / artists
- genre distribution (top genres by track count)
- audio formats (FLAC/MPC/WAV counts)
- structural notes (compilations present? edge cases?)

## 4. Results

- quantitative retrieval: same-album@N, same-artist@N, genre purity@N,
  album coherence — per pooling strategy and hop size
- human evaluation: seed tracks, mean score 0–3 per method, A-vs-B counts
- cross-codec stability: FLAC vs MPC Q6 cosine (per track, mean/std)
- runtime: wall time/track, realtime factor, peak memory
- storage: JSON decimal vs base64-float32 vs binary, 10/20/100-track sizes,
  with/without package-level compression

## 5. Recommendation

Either:

> **Recommended MusicPack Sonic v1 profile:** …

(with the full profile: model, weights hash, preprocessing, windowing, hop,
silence rule, pooling, normalization, dimensions, distance, storage encoding)

or:

> **Insufficient evidence — further evaluation required.** …

OpenL3 is not forced to win. If the comparator or the dataset does not
support a decision, say so explicitly and say what would.
