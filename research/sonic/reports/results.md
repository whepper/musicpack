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
| **LAION-CLAP `larger_clap_music`** | LAION | **apache-2.0** | 48000 | 64-band log-mel (SWIN) | 512 | decision-gate candidate |

### Provenance lock — LAION-CLAP `larger_clap_music`

Verified from the Hugging Face model card and files (2026-08-11):

- **Model id:** `laion/larger_clap_music` — "an improved CLAP checkpoint, specifically trained on music"
- **License:** **apache-2.0** (HF card); paper arXiv:2211.06687 CC BY 4.0
- **Upstream code:** LAION-CLAP (github.com/LAION-AI/CLAP, MIT)
- **Checkpoint commit (repo sha):** `a0b4534a14f58e20944452dff00a22a06ce629d1`
- **Weights file:** `pytorch_model.bin` — **776 MB**
- **Weights SHA-256:** `5c289311f4a030d768af7ffbfdecd01b008aa64824211899a4e59f4f9d154fd1`
- **Architecture:** `ClapModel`; audio encoder SWIN-like (`hidden_size` 1024, depths [2,2,12,2]), `projection_dim` **512**
- **Preprocessing:** `ClapProcessor` — 48 kHz, 64 mel bands (n_fft 1024, hop 480, freq 50–14000 Hz), native chunk **10 s**, `padding: repeatpad`, `truncation: rand_trunc` (avoided by feeding exact 10 s segments)
- **Embedding:** `get_audio_features().pooler_output` — **L2-normalized 512-dim** per clip; verified bit-identical across runs
- **Deterministic track aggregation:** non-overlapping 10 s segments (final segment kept if ≥5 s), mean-norm pooled; segment length is a profile parameter
- **Environment:** torch + transformers in `.venv-clap` (CPython 3.11); checkpoint downloaded at runtime to gitignored `models/`, never committed

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

Blind pairwise review (`reports/human/human-pairwise-openl3-vs-discogs-effnet.html`,
12 seeds × 2 methods, A = openl3, B = discogs-multi, profile ids hidden):
the reviewer found **B consistently better than A across the seeds**.
Qualitative verdict (informal, not a scored ratings.json), but directionally
unambiguous and fully aligned with the quantitative diagnostics: the
similarity-trained representation (Discogs-EffNet) produces more convincing
recommendations than OpenL3. This is the decisive evidence the benchmark was
designed to produce.

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

### CLAP decision gate (staged 100-track benchmark)

Primary profile only (mean-norm, silence off, settled baseline), same 100
tracks for all three models (OpenL3/Discogs embeddings reused from cache;
only CLAP was newly analyzed, ~10 min).

| metric | OpenL3 | **CLAP-music** | Discogs-multi |
|---|---|---|---|
| same-album@10 | 0.152 | **0.093** | 0.201 |
| same-artist@10 | 0.223 | **0.136** | 0.316 |
| genre_purity@10 | 0.355 | **0.270** | 0.546 |
| album_coherence@10 | 0.036 | **0.038** | 0.049 |

**CLAP-music is materially worse than OpenL3 on every diagnostic** and far
below Discogs-EffNet. Expected in hindsight: CLAP is audio–text
contrastive, not music-similarity-trained; the 10 s chunk-mean pooling
dilutes track identity. The result is unambiguous, so the staged rule did
not require expanding to 200 tracks.

- **Cross-codec:** source↔FLAC 1.000, source↔MPC-Q6 0.99999 mean (10 tracks)
  — stable, like OpenL3.
- **Runtime:** ~0.022× realtime (1.3 s/min; ~6 s per 4-min track),
  peak RSS ≈ 1.5 GB — *faster and lighter than OpenL3*, but moot given quality.
- **Product size:** 776 MB checkpoint (apache-2.0); torch runtime ~1.5 GB.
  Bundling would add ~0.8 GB (or ~2.5 GB with torch); first-use download
  would be the sane distribution choice. Evidence only — no packaging work.
- **Human eval:** blind pairwise pages generated
  (`human-pairwise-clap-vs-discogs-effnet.html`,
  `human-pairwise-clap-vs-openl3.html`, 12 seeds each); given the
  quantitative margin, a close human verdict is not expected, but the pages
  are ready for confirmation.

### Three-way blind listening verdict + agreement analysis

**Reviewer verdict (blind A/B/C, 16 seeds, metadata hidden):**
**Discogs-EffNet is best; OpenL3 is not bad either; CLAP is not preferred.**
This confirms the quantitative picture perceptually: the similarity-trained
reference (Discogs) is the strongest, and among permissive candidates OpenL3
is the closest.

Agreement with the Discogs recommendation lists (reference = Discogs;
same cosine retrieval, seed excluded; unordered set overlap):

| k | metric | OpenL3 | CLAP |
|---|---|---|---|
| 5 | jaccard (all 100 pool) | **0.400** | 0.188 |
| 5 | jaccard (16 review seeds) | **0.425** | 0.212 |
| 10 | jaccard (all 100 pool) | **0.400** | 0.257 |
| 10 | jaccard (16 review seeds) | **0.381** | 0.294 |
| 5 | mean rank of shared items | **2.3** | 2.9 |

OpenL3 agrees with Discogs roughly **2× more than CLAP** on which tracks are
recommended, and orders the shared items closer to Discogs. CLAP is
therefore not merely weaker on metadata-oriented diagnostics — its
recommendation lists are also least aligned with the perceptual reference.

## 5. Recommendation

**Model decision (perceptual verdict): do not freeze a single model as
normative.** Discogs-EffNet is perceptually and quantitatively best, but is
CC BY-NC-SA (non-commercial). Freezing a weaker model merely because it is
permissive would standardize a core feature prematurely. Instead:

- **Freeze the generic Sonic container contract** (`musicpack-sonic` format
  v1): a model-independent, versioned `analysis/sonic.json` carrying an
  explicit profile id, track + album embeddings, base64-f32le, cosine
  distance, null no-embedding, profile-compatibility rules. Profiles evolve
  under this stable container.
- **Default permissive profile: `musicpack-sonic-openl3-v1`** — OpenL3 is
  the closest permissive option to the Discogs reference (agreement 2×
  CLAP's; "not bad" perceptually). It is the recommended *default* for the
  open ecosystem, documented honestly as weaker than Discogs — **not** the
  permanent normative model.
- **Quality reference: `musicpack-sonic-discogs-v1`** (Discogs-EffNet,
  CC BY-NC-SA) — the server may offer it per-collection where the operator
  accepts non-commercial terms. Never the mandatory profile.
- **Rejected: `musicpack-sonic-clap-v1`** — CLAP performed worst on
  quantitative diagnostics, was not preferred in blind listening, and its
  lists agree least with the Discogs reference.
- **Open follow-up:** continue watching for a permissively licensed,
  similarity-trained model; when one reaches acceptable quality it becomes a
  new profile (and potentially the new default) without a container change.

The draft `specs/musicpack-sonic-v1.md` reflects this: the **container** is
stable; the **default model** is OpenL3 but explicitly not frozen as
normative.

## 6. Production integration — runtime + compatibility (completed)

The container freeze and production integration are complete
(`specs/musicpack-sonic-v1.md` is NORMATIVE; `libmusicpack` owns Sonic
semantics; the `musicpack` CLI verifies/attaches sonic documents; MusicPack
Author has a Sonic Analysis panel). The OpenL3 analyzer runs as
`sonic/musicpack-sonic` (C11 + ONNX Runtime, single-threaded):

- **Frontend**: the kapre mel frontend (STFT hann/242, librosa-slaney mel
  256, decibel −80) is deterministic DSP, ported to C and verified against
  the numpy reference (`research/sonic/frontend.py`) to float precision.
- **Model**: only the learned network after the frontend is converted to
  ONNX (`convert_openl3.py`, weights SHA-256 pinned, in-process
  verification cosine 1.0). The ONNX artifact is itself SHA-256-pinned
  (`3b4b7dac…`).
- **Resampler**: a faithful polyphase port of resampy 0.4.3 `kaiser_best`
  (float32 per-tap accumulation, matching resampy bit-for-bit to ~1e-9).

### Research-vs-production compatibility (`compat_measure.py --c-doc`)

The C analyzer's output document compared against the research harness on
the deterministic corpus (chord/harmonics at 44.1 kHz, tone/noise at 48 kHz,
plus a short 2-window edge case):

| track | cosine | maxdiff | meandiff |
|---|---|---|---|
| chord-44k | 0.999998 | 5.34e-04 | 5.24e-05 |
| tone-48k | 1.000000 | 8.04e-05 | 3.86e-06 |
| harmonics-44k | 1.000000 | 2.52e-04 | 2.38e-05 |
| noise-48k | 1.000000 | 2.98e-08 | 5.48e-09 |
| edge-44k (1.2 s, 2 windows) | 1.000000 | 3.51e-04 | 2.13e-05 |

Gates: cosine ≥ 0.9999, meandiff ≤ 1e-4, maxdiff ≤ 2e-3 — **all PASS**. The
residual differences are float32 matmul noise in near-silent mel bins
(log-domain); the reference itself shows the same character versus TF. They
cannot materially change recommendation ordering. Known limitation: a
1.0 s track whose second window is pure silence amplifies this noise (cosine
~0.998) — an artificial worst case, not representative of real content.

### Model acquisition

The post-frontend ONNX is not committed and not bundled. It is produced from
the SHA-256-pinned OpenL3 H5 (`624ee7b1…`) by `convert_openl3.py` and
verified against its own pinned SHA-256 (`3b4b7dac…`) before use. A
package-provided profile id can never trigger a download or model execution.
