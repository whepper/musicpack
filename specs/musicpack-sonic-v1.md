# MusicPack Sonic Analysis — document format and profiles

> **Status:** the **container contract** (`musicpack-sonic` format, version
> 1) is **stable** and the recommended production direction. The **default
> embedding model is NOT frozen as normative**: Discogs-EffNet is the
> perceptual/quantitative quality reference but is non-commercial; OpenL3
> is the recommended permissive default profile, documented honestly as
> weaker; a future permissively licensed, similarity-trained model may
> replace the default without a container change.
>
> No production implementation exists yet: no `.mpack` v1 package carries
> sonic analysis, MusicPack Author does not compute it, and the server does
> not index it. Evidence: `research/sonic/reports/results.md`.
>
> Version 1 (container). Status: container stable; model choice open.

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
per track, a deterministic album embedding, and the exact **profile** that
produced them. It does **not** store recommendations. Relationships such as
`similarAlbums`, `similarArtists`, `similarTracks` are collection-dependent
and are derived by `musicpack-server`.

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
deterministic pooling          (per profile)
  ↓
track embedding
  ↓  deterministic aggregation  (per profile)
album embedding
```

Track and album sonic identity belong to the `.mpack`. Artist vectors and
all recommendation results are derived by the server from the stored
track/album vectors.

## 3. Format vs profile — two different things

A central decision of this specification:

```text
musicpack-sonic document format v1   — the stable container (this spec)
musicpack-sonic-openl3-v1            — one embedding profile (model-scoped)
musicpack-sonic-discogs-v1           — another profile (eval/reference)
```

The **document format** is model-independent and stable: the JSON shape,
vector encoding, distance metric, album aggregation and compatibility rules.
**Profiles** name a specific model + preprocessing + pooling and may evolve
under the same format. The document carries a profile id; the server never
compares vectors whose profile ids differ.

> **Embeddings are comparable only when their complete sonic analysis
> profile is compatible.**

Profile ids are **model-scoped** and stable strings (e.g.
`musicpack-sonic-openl3-v1`), not dynamic hashes. The detailed parameter
object and weights checksum *define* the id: an incompatible parameter
change requires a new profile id/version, not a container change. The
analysis document carries the full parameters for auditing in addition to
the id.

## 3.1 Normative default profile — `musicpack-sonic-openl3-v1`

The recommended permissive profile. **Not** a claim that OpenL3 is forever
the best recommendation model; it is the closest permissive option to the
quality reference (see §12).

```json
{
  "id": "musicpack-sonic-openl3-v1",
  "model": {
    "name": "openl3",
    "version": "0.4.0",
    "contentType": "music",
    "inputRepr": "mel256",
    "embeddingSize": 512,
    "sampleRate": 48000,
    "resampling": "resampy kaiser_best",
    "frontend": "kapre",
    "mel": { "nFft": 2048, "hop": 242, "nMels": 256, "decibel": true, "padEnd": true },
    "weightsSha256": "624ee7b1dd5ff87e18073f66fd8b2052bebb8ac70210e9c0937c0c940c63e9d6",
    "license": "code MIT; weights CC BY 4.0"
  },
  "window": { "seconds": 1.0, "center": true },
  "hop": { "seconds": 1.0 },
  "silence": { "enabled": false },
  "pooling": { "strategy": "mean-norm", "normalization": "l2" },
  "dimensions": 512,
  "distance": "cosine",
  "albumAggregation": "equal-track-mean-l2"
}
```

Profile parameters are explicit, never library defaults: window 1.0 s,
48 kHz target, `center=true`, mel256 kapre frontend (n_fft 2048, hop 242,
decibel, pad_end), mean-norm pooling, L2, 512 dims, cosine.

## 3.2 Other profiles

- **`musicpack-sonic-discogs-v1`** — Discogs-EffNet `multi`
  (1280-dim, 16 kHz, internal 96-band log-mel, 131-frame patches @ 0.976 s
  hop). **Quality reference, CC BY-NC-SA — never the mandatory profile.**
  May be offered per-collection by the server where the operator accepts
  non-commercial terms.
- **`musicpack-sonic-clap-v1`** — LAION-CLAP `larger_clap_music`
  (512-dim, 48 kHz, 10 s segments). **Evaluated and rejected** (worst
  diagnostics, not preferred perceptually, least agreement with the Discogs
  reference). Retained only as documentation; a package claiming this
  profile must still be parseable per the generic contract.

## 4. Document structure

The sonic document lives at `analysis/sonic.json` in the package
(reference in the manifest's optional `analysis[]` entries).

```json
{
  "format": "musicpack-sonic",
  "version": 1,
  "profile": "musicpack-sonic-openl3-v1",

  "analyzer": {
    "tool": "musicpack",
    "toolVersion": "..."
  },

  "album": {
    "embedding": {
      "encoding": "base64-f32le",
      "dimensions": 512,
      "data": "..."
    },
    "tracksContributing": 10
  },

  "tracks": [
    {
      "disc": 1,
      "track": 1,
      "embedding": {
        "encoding": "base64-f32le",
        "dimensions": 512,
        "data": "..."
      }
    }
  ]
}
```

### 4.1 Decisions on the shape

- **Per-track key**: `disc` + `track` matches the manifest's `media[].
  tracks[]` addressing; stable across package edits.
- **Album embedding is stored** (equal track weighting → mean → L2), plus
  `tracksContributing`. Cheap, explicit, auditable; the server reads one
  document instead of aggregating. The exact aggregation rule is part of
  the profile (`albumAggregation`).
- **Explicit no-embedding**: a track with no meaningful embedding is
  `"embedding": null` — never a fabricated zero vector. `album.embedding`
  skips null tracks and records the count that contributed.
- **Provenance**: `analyzer.tool`/`toolVersion`; timestamps omitted by
  default for determinism, matching the `.mpack` convention.

## 5. Float representation

Fixed for v1: **base64-float32-little-endian**.

- numerical precision: IEEE-754 binary32 (float32);
- byte order: little-endian;
- encoding: base64 (`base64-f32le`), standard alphabet, no line breaks;
- dimensions: exactly `profile.dimensions`;
- normalization: unit L2 norm within tolerance `1e-3` (validated on read;
  malformed vectors rejected).

Measured on 512-dim embeddings: ~4× smaller than JSON decimal, within ~35%
of raw binary; lossless for float32. `binary-f32le` and `json` (decimal)
remain permitted alternatives for specific consumers; int8 quantization is
not used unless evaluation proves it worthwhile.

## 6. Silence / non-musical handling

A deterministic energy-based rule may exclude low-energy windows
(relative-to-median RMS, or an absolute dBFS floor). The `openl3-v1`
profile disables the gate (benchmarked equivalent on a 200-track library);
the gate stays a profile parameter so collections with long intros/outros
can re-enable it. Tracks shorter than the window, near-silent, spoken word,
hidden tracks and noise behave deterministically and yield `null` if no
window survives — never a fabricated vector.

## 7. What is intentionally not stored

The `.mpack` sonic document stores: track sonic vectors, album sonic
vector, analysis profile/provenance, explicit no-embedding markers.

`musicpack-server` derives everything relational: similar tracks/albums,
artist representations (from album/track vectors), similar artists,
genre/style affinity (from metadata of nearby tracks/albums), radio
candidates. Artist embeddings are server-derived, never stored; genre
affinity emerges from the user's own collection, not a fixed ML ontology.

## 8. Auxiliary descriptors (optional, separate)

Tempo/BPM, key, mode are useful for radio transitions and discovery UX but
are not the primary similarity representation. They are optional and
omitted from the core profile until a permissively licensed, reasonably
deterministic implementation is demonstrated (Essentia/AGPL is not
acceptable as a mandatory dependency).

## 9. Future vector indexing (design note)

Exact cosine similarity is sufficient for small libraries (all-pairs over a
few thousand vectors is trivial); a future ANN index may be added for
larger collections. No vector-database dependency is introduced; start
simple.

## 10. Resolved and open design questions

Resolved by this phase:

1. Document location: **`analysis/sonic.json`**, referenced from the
   manifest's `analysis[]` entries.
2. Album vector: **stored** (equal-track mean → L2), per §4.1.
3. Cross-codec: confirmed safe after `flac2mpc` (FLAC↔MPC-Q6 cosine
   ≥ 0.9998, OpenL3).
4. Default model: **OpenL3** (permissive, closest to the Discogs
   reference) but **not normative** — see §12.

Still open:

5. Whether `hop_seconds` stays a profile parameter for model-fixed hops
   (Discogs-EffNet uses a fixed 0.976 s patch hop).
6. Whether the weight SHA-256 is mandatory in every profile or can be
   implied by `model` + `modelVersion` for public immutable weights.
7. Production runtime for the analyzer (ONNX conversion spike vs isolated
   helper) — pending the production-integration phase.
8. The future default: when a permissively licensed, similarity-trained
   model reaches acceptable quality, it becomes a new profile and candidate
   default without a container change.

## 11. Licensing constraints

The **default profile must be permissive and open**:

- **OpenL3** (code MIT, weights CC BY 4.0) is the default permissive
  profile — the closest permissive option to the quality reference.
- **Discogs-EffNet / Essentia** (Essentia AGPL-3.0; MTG weights
  CC BY-NC-SA 4.0) are not acceptable as the mandatory foundation of an
  unrestricted open MusicPack ecosystem; they remain a per-collection
  quality-reference profile and an evaluation comparator only.
- **LAION-CLAP** (Apache-2.0) was evaluated and **rejected on quality**:
  worst diagnostics, not preferred in blind listening, least agreement with
  the Discogs reference.

## 12. Known limitation and model decision

The benchmark and blind listening established:

> Discogs-EffNet produced materially better recommendation quality in
> quantitative diagnostics and blind human evaluation, but cannot be the
> normative MusicPack profile because of its licensing constraints.

LAION-CLAP was evaluated and rejected because it performed worse than
OpenL3 on the project benchmark and agreed least with the Discogs
reference.

**Model decision:** do not standardize a single model as normative while
the only stronger model is non-commercial. The container contract is
frozen; OpenL3 is the recommended permissive default, documented honestly
as weaker; the server may offer Discogs-EffNet per-collection; and the
project keeps watching for a permissively licensed, similarity-trained
model. v1 is a stable interoperable representation — not a claim that
OpenL3 will forever be the best recommendation model.
