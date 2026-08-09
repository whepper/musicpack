# MusicPack Sonic Analysis `.mpack` profile — draft v1

> **DRAFT — RESEARCH PHASE — NOT YET NORMATIVE.**
>
> This document is the working draft produced by the sonic-analysis research
> phase (`research/sonic/`). It does **not** yet bind any implementation:
> no `.mpack` v1 package carries sonic analysis, MusicPack Author does not
> compute it, and the server does not index it. The final profile
> (model, pooling, hop, silence rule) will be selected from benchmark
> evidence (`research/sonic/reports/`) before this becomes normative.
> Sections marked "pending evidence" are explicitly unresolved.
>
> Version 1 (draft). Status: research.

## 1. Scope and purpose

MusicPack sonic analysis makes **content-based music discovery** possible
inside a self-hosted MusicPack collection without a centralized
recommendation service:

```text
MusicPack Author
      ↓
analyse decoded PCM once
      ↓
.mpack stores stable sonic representation
      ↓
musicpack-server indexes it
      ↓
collection-local recommendations
```

The package stores **how the music sounds** — a versioned audio embedding
per track, and a deterministic album embedding — plus the exact **profile**
that produced them. It does **not** store recommendations. Relationships
such as `similarAlbums`, `similarArtists`, `similarTracks` are
collection-dependent and are derived by `musicpack-server`.

### Intended discovery features

Similar tracks, similar albums, similar artists, genre/style affinity
derived from nearby music, track radio, album radio, "explore this sound",
and eventually sonic journeys between musical regions — all driven by the
stored vectors.

## 2. Conceptual pipeline

```text
audio
  ↓  decode to PCM (FLAC/MPC/WAV — never compressed bytes)
embedding model
  ↓  window embeddings over time
deterministic pooling          (mean / mean-norm / robust-mean + silence gate)
  ↓
track embedding
  ↓  deterministic aggregation  (equal vs duration weighting)
album embedding
```

Track and album sonic identity belong to the `.mpack`. Artist vectors and
all recommendation results are derived by the server from the stored
track/album vectors.

## 3. The profile — the comparability contract

> **Embeddings are comparable only when their complete sonic analysis
> profile is compatible.**

The profile covers every factor that can change an embedding:

```text
model
model variant
model weights (version / SHA-256)
input preprocessing
sample rate
windowing (window length)
hop
centering
frontend
silence handling (rule + threshold)
pooling strategy
normalization
dimensions
distance metric
```

Two vectors computed under different profiles must **never** be compared;
the server must reject such comparisons.

`profile.id` is a compact stable identifier derived from the full profile
(e.g. the SHA-256 of the canonical parameter JSON), so any parameter change
yields a different id and thus incompatible vectors. The analysis document
carries the full parameters for auditing and reproducibility, not just the
id.

> **Pending evidence:** the concrete values below reflect what the research
> phase currently favours; the benchmark report decides the final profile.
> These numbers are illustrative, not yet normative.

### Illustrative candidate profile

```json
{
  "id": "musicpack-sonic-v1-<fingerprint>",
  "model": {
    "name": "openl3",
    "contentType": "music",
    "inputRepr": "mel256",
    "embeddingSize": 512,
    "sampleRate": 48000,
    "frontend": "kapre",
    "weightsSha256": "624ee7b1…",
    "package": "openl3==0.4.0",
    "tensorflow": "2.15.1",
    "license": "code MIT; weights CC BY 4.0"
  },
  "window": { "seconds": 1.0, "center": true },
  "hop": { "seconds": 1.0 },
  "silence": {
    "enabled": true,
    "rule": "relative-to-median-rms-db",
    "thresholdDb": -20.0,
    "windowSeconds": 1.0
  },
  "pooling": { "strategy": "mean-norm", "normalization": "l2" },
  "dimensions": 512,
  "distance": "cosine"
}
```

## 4. Document structure

The conceptual shape below is the working draft; it is reviewed critically
in §4.1.

```json
{
  "format": "musicpack-sonic",
  "version": 1,
  "profile": { "...": "as §3" },
  "album": {
    "embedding": "<base64-float32-le>"
  },
  "tracks": [
    {
      "disc": 1,
      "track": 1,
      "embedding": "<base64-float32-le>"
    }
  ]
}
```

### 4.1 Critical review of the shape

- **Per-track key**: `disc` + `track` matches the manifest's `media[].
  tracks[]` addressing and is stable across package edits. It is preferred
  over positional indices. (Decision to keep.)
- **Album embedding stored vs derived**: the draft **stores** the album
  vector. It is cheap (one more vector), explicit, auditable, and lets the
  server read one document instead of aggregating N tracks. The alternative
  — derive album = `normalized mean(track vectors)` at index time — is
  simpler and immune to a stale album vector, at the cost of recomputation.
  Both were benchmarked (equal vs duration weighting); the draft stores the
  equal-weighted vector and records `aggregation` in the profile.
- **Explicit no-embedding**: a track that produces no meaningful embedding
  (near-silent, shorter than the window, analysis failure) is represented
  with `"embedding": null` — an explicit, auditable "no representation",
  never a fabricated zero vector. `album.embedding` skips such tracks and
  records the number that contributed.
- **Missing fields this draft adds**: float encoding (§5), profile id and
  full parameters (§3), and provenance (`tool`, `toolVersion`, analysis
  date excluded by default for determinism, matching the `.mpack`
  convention).

## 5. Float representation

Research evidence (512-dim embeddings, 10/20/100-track albums,
gzip-compressed):

| album | JSON decimal | base64-f32le | binary-f32le |
|---|---|---|---|
| 10-track | 108 kB (48 kB gz) | 27.8 kB (20.7 kB gz) | 20.5 kB (19 kB gz) |
| 100-track | 1080 kB (470 kB gz) | 277 kB (206 kB gz) | 205 kB (190 kB gz) |

The draft chooses **base64-float32-little-endian** as the stored encoding:

- portable (plain JSON, no separate file / container coordination);
- ~4× smaller than JSON decimal, within ~35% of raw binary;
- lossless for float32 values; deterministic decode.

The specification therefore fixes:

- **numerical precision**: IEEE-754 binary32 (float32);
- **byte order**: little-endian;
- **encoding**: base64 (`base64-f32le`), standard alphabet, no line breaks;
- **dimensions**: exactly `profile.dimensions`;
- **normalization**: unit L2 norm within tolerance `1e-3` (validated on
  read; malformed vectors are rejected).

`binary-f32le` (a separate vector file) and `json` (decimal) remain
permitted alternatives for specific consumers but are not the default.
Integer/int8 quantization is **not** used unless evaluation proves it
worthwhile.

## 6. Silence / non-musical handling

A deterministic energy-based rule excludes low-energy windows from the
embedding aggregation so intro/outro silence does not dominate a track:

- RMS (dBFS) is computed per window (window length = profile window,
  centred on each window timestamp);
- default rule: **relative to the track's own median window RMS**, windows
  more than `thresholdDb` below the median are excluded
  (`relative-to-median-rms-db`, `thresholdDb = -20.0`);
- an absolute dBFS floor (`absolute-rms-db`) is also defined for
  content-independent exclusion.

Tracks shorter than the analysis window, near-silent tracks, spoken word,
hidden tracks and noise still produce **deterministic** behaviour: if no
window survives (or the model yields no window), the track is stored as
`"embedding": null` — never a fabricated vector.

> **Pending evidence:** the exact threshold choice is benchmarked
> (`research/sonic/`); the winning rule becomes normative.

## 7. What is intentionally not stored

The `.mpack` sonic document stores:

```text
track sonic vectors
album sonic vector
analysis profile / provenance
explicit no-embedding markers
```

`musicpack-server` derives everything relational:

```text
similar tracks
similar albums
artist representations        (from album/track vectors)
similar artists
genre/style affinity          (from metadata of nearby tracks/albums)
radio candidates
```

Artist embeddings are **server-derived** from album/track vectors, not
stored. Genre affinity is derived from the **metadata of nearby
tracks/albums**, not from a fixed ML genre ontology — the recommendation
vocabulary emerges from the user's actual collection.

## 8. Auxiliary descriptors (optional, separate)

Tempo/BPM, musical key and mode are useful for radio transitions,
DJ-like sequencing, filtering and discovery UX — but they are **not** the
primary similarity representation and are kept separate from the core
embedding.

> **Pending evidence:** a permissively licensed, reasonably deterministic
> implementation must be demonstrated before these become part of Sonic v1.
> Essentia (AGPL) is not acceptable as a mandatory dependency for this.
> If the open implementation quality is questionable, these remain optional
> and omitted from the core profile.

## 9. Future vector indexing (design note)

For small libraries, **exact cosine similarity** is entirely sufficient
(an all-pairs pass over a few thousand vectors is trivial). For larger
collections a future ANN index could be added. This research phase adds
**no** vector-database dependency; the first production implementation
should start simple (exact cosine) and only add ANN if measurements demand
it.

## 10. Open design questions

1. Where the sonic document lives in the `.mpack` (a
   `sonic.json` in the package root, or an `analysis/` asset) — decided in
   the production-integration phase, not here.
2. Whether the album vector should be stored (§4.1) or derived — the draft
   stores it; the benchmark's album-aggregation evidence may revisit this.
3. Whether `hop_seconds` stays a profile parameter given it can be model
   fixed (Discogs-EffNet has a fixed 0.976 s patch hop).
4. Cross-codec tolerance: whether sonic analysis may run after `flac2mpc`
   — expected to be confirmed by the cross-codec benchmark (MPC Q6 vs FLAC
   cosine).
5. Exact silence-threshold values and pooling strategy from the benchmark.
6. Whether the weight SHA-256 is mandatory in every profile or can be
   implied by `model` + `modelVersion` when the weights are public and
   immutable.

## 11. Licensing constraints

The mandatory profile must be permissive and open:

- **OpenL3** (code MIT, weights CC BY 4.0) is the primary candidate.
- **Discogs-EffNet / Essentia** (Essentia AGPL-3.0; MTG weights
  CC BY-NC-SA 4.0) are **not** acceptable as the mandatory foundation of an
  unrestricted open MusicPack ecosystem. They are used only as an
  evaluation comparator in `research/sonic/` and are never a MusicPack
  runtime dependency.
