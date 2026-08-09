# MusicPack Sonic Analysis — research workspace

Research and prototype phase for the future **`musicpack-sonic-v1`** audio
embedding profile. This is **not** production MusicPack architecture. It
exists to decide *how* an `.mpack` should describe "what the music sounds
like" so that `musicpack-server` can later do collection-local,
content-based discovery (similar tracks / albums / artists, radio, "explore
this sound").

```text
audio ──decoded PCM──▶ embedding model ──▶ window embeddings
    ──deterministic pooling──▶ track embedding
    ──deterministic aggregation──▶ album embedding
```

The benchmark compares candidate representations (OpenL3 vs the
Discogs-EffNet Essentia models) and experiments with pooling, hop size,
silence handling, album weighting, storage encoding and cross-codec
stability. The evidence is written up under `reports/` and feeds the draft
specification in `specs/musicpack-sonic-v1.md`.

## Rules of this workspace

- **No restricted model weights, copyrighted audio, or proprietary assets
  are committed.** The OpenL3 weights (CC BY 4.0) and the Discogs-EffNet
  weights (CC BY-NC-SA 4.0, MTG) are downloaded at runtime into gitignored
  cache directories, with SHA-256 recorded.
- **Essentia is never a MusicPack dependency.** `requirements-essentia.txt`
  is optional and exists only to tell us whether a music-similarity-trained
  representation beats OpenL3. The benchmark runs without it.
- **The test suite works without any restricted/optional model.** The
  standard path is OpenL3-free, model-free pytest.
- Nothing here changes `.mpack` v1, MusicPack Author, or the server. This
  phase produces evidence and a draft specification only.

## Environment

The system Python is 3.14; the research stack needs CPython 3.11 because
`openl3==0.4.0` (last release, unmaintained) depends on the TensorFlow 2 /
Keras 2 / kapre stack, whose macOS-arm64 wheels stop at CPython 3.11.
`openl3` is also sdist-only and its `setup.py` breaks on modern Python and
pulls every model weight during build — `patch_openl3.py` produces a
deterministic patched tree (only `setup.py`/`models.py` change; the model
code is untouched) and the weights are fetched at runtime instead.

```sh
research/sonic/bootstrap_env.sh          # venv + pinned deps + patched openl3
research/sonic/.venv/bin/python research/sonic/benchmark.py --help
```

Pinned requirements: `research/sonic/requirements-openl3.txt` and, for the
optional comparator, `research/sonic/requirements-essentia.txt`. The exact
installed environment is frozen in `reports/environment.lock.txt`.

## Layout

```text
research/sonic/
├── README.md                  this file
├── bootstrap_env.sh           reproducible venv + patched openl3 install
├── patch_openl3.py            openl3 0.4.0 sdist patch (py3.12+ / no weights)
├── requirements-openl3.txt    pinned OpenL3 stack (CPython 3.11)
├── requirements-essentia.txt  optional Discogs-EffNet comparator stack
├── benchmark.py               subcommand CLI (analyze/evaluate/cross-codec/…)
├── library.py                 scan a music directory into album/artist/genre
├── decode.py                  FLAC / MPC / WAV → PCM (+ duration)
├── profile.py                 Profile → canonical fingerprint → profile.id
├── pooling.py                 mean / mean-norm / robust-mean + silence gate
├── album.py                   equal- vs duration-weighted album aggregation
├── storage.py                 embedding encodings (float32) + validation
├── metrics.py                 retrieval@N / genre purity / coherence
├── report.py                  aggregate reports + human-eval HTML (blind)
├── cache.py                   two-level embedding cache (audio+profile keyed)
├── analyzers/
│   ├── base.py                Analyzer interface
│   ├── openl3.py              primary candidate (music/mel256/512)
│   └── essentia_discogs.py    optional comparator (evaluation only)
├── fixtures/                  synthetic deterministic albums (generated)
├── reports/                   templates + aggregate results (committed)
│   ├── README.md
│   ├── environment.lock.txt
│   ├── raw/                   per-track quantitative data (gitignored)
│   └── human/                 human-eval HTML + ratings (gitignored)
└── tests/                     pytest (model-free by default)
```

Gitignored at runtime: `.venv/`, `wheels/`, `cache/` (window embeddings),
`models/` (downloaded weights), `ratings/`, `fixtures/*.flac`,
`reports/raw/`, `reports/human/`.

## Benchmark dataset

Point the harness at any local music directory — it is never committed:

```sh
python research/sonic/benchmark.py analyze \
    --library /path/to/music \
    --analyzer openl3 \
    --hop 1.0 \
    --pooling mean-norm
```

Metadata for the quantitative diagnostics comes from the directory structure
(`artist/album/track.*`) and embedded tags (via `ffprobe`); no copyrighted
data enters the repository. A 100–1000 track library with several artists,
multiple albums per artist, related subgenres, acoustic/rock/ambient
material, compilations and edge cases gives the strongest evidence.

## Cross-codec methodology

Given the same source track encoded as FLAC and MPC Q6, both are decoded to
PCM and analysed; the cosine between the two embeddings is the cross-codec
stability score. This answers whether sonic analysis can safely run *after*
`flac2mpc` (the intended MusicPack Author workflow).

## Licensing notes

| Asset        | License                                             | Committed? |
|--------------|-----------------------------------------------------|------------|
| OpenL3 code  | MIT                                                 | (installed)|
| OpenL3 weights | CC BY 4.0                                         | no — runtime download |
| Essentia     | AGPL-3.0 (commercial licensing separate)            | (optional) |
| Discogs-EffNet weights | CC BY-NC-SA 4.0 (MTG)                   | no — runtime download |
| Synthetic fixtures | repo-generated, public-domain-by-construction | yes (generated on demand) |

The Discogs-EffNet comparator exists **only** to validate whether OpenL3
gives convincing recommendations; neither Essentia nor its models are
acceptable as the mandatory foundation of an unrestricted open MusicPack
ecosystem.
