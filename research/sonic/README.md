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
specification in `specs/musicpack-sonic-v1.md`
(**DRAFT — RESEARCH PHASE — NOT YET NORMATIVE**).

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

The optional Discogs-EffNet comparator needs a **separate** venv
(`essentia-tensorflow` pulls numpy 2.x, which conflicts with openl3's
numpy<2):

```sh
research/sonic/bootstrap_essentia.sh     # optional, evaluation-only
```

Pinned requirements: `research/sonic/requirements-openl3.txt` and, for the
optional comparator, `research/sonic/requirements-essentia.txt`. The exact
installed environment is frozen in `reports/environment.lock.txt`.

### Candidate models and what is pinned

**OpenL3** (primary): `music` / `mel256` / `emb512`. Window 1 s, target
48 kHz (resampled with resampy `kaiser_best`), mel256 kapre frontend
(n_fft 2048, hop 242, decibel, pad_end), `center=True`. Verified against
openl3 0.4.0: window embeddings are **not** L2-normalized (norms ~50); the
librosa frontend is incompatible with librosa>=0.10 (kapre is the pinned
choice); there is no `audio_crop` option in 0.4.0. Weights: CC BY 4.0,
SHA-256 `624ee7b1...` recorded in the profile.

**Discogs-EffNet** (comparator, evaluation-only): `multi` and `release`
variants. 16 kHz mono, internal 96-band log-mel (frame 512, hop 256,
slaneyMel), patches of 131 mel frames (2.096 s) striding 61 (0.976 s),
output 1280-dim (`PartitionedCall:1`). The frame count for n16 samples is
deterministic: `nFrames = 1+ceil((n16-256)/256)`, `nPatches =
max(0, 1+floor((nFrames-131)/61))`. Weights: CC BY-NC-SA 4.0, SHA-256
`2c964064...` (multi) / `bd044fe5...` (release). The `pooling.hop_seconds`
is ignored for this model (its patch hop is fixed); pooling strategies and
the silence gate still apply to its windows.

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
