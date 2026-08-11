# MusicPack Sonic Analysis — document format and profiles

> **Status: NORMATIVE for the container v1 freeze.** The **document/container
> contract** (`musicpack-sonic` format, version 1) is **stable and frozen**.
> The **embedding model is NOT frozen as normative**: Discogs-EffNet is the
> perceptual/quantitative quality reference but is non-commercial; OpenL3 is
> the default permissive profile, documented honestly as weaker; a future
> permissively licensed, similarity-trained model may replace the default
> without a container change.
>
> The container is model-independent. Profiles are model-scoped identifiers
> (e.g. `musicpack-sonic-openl3-v1`) that evolve under the stable container;
> only an incompatible change to the *container itself* requires
> `musicpack-sonic` format version 2.

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
musicpack-sonic-discogs-v1           — another profile (research/reference)
musicpack-sonic-clap-v1              — a rejected profile (documentation)
```

The **document format** is model-independent and stable: the JSON shape,
vector encoding, distance metric, album aggregation and compatibility rules.
**Profiles** name a specific model + preprocessing + pooling and may evolve
under the same format. The document carries a profile id; the server never
compares vectors whose profile ids differ.

> **Embeddings are comparable only when their complete sonic analysis
> profile is compatible.**

Profile ids are **model-scoped** stable strings (e.g.
`musicpack-sonic-openl3-v1`), not dynamic hashes. The detailed parameter
object and weights checksum *define* the id: an incompatible parameter
change requires a new profile id/version, not a container change.

### 3.1 Registered profiles (v1)

| Profile id | Status | Model | Dims | Distance | Encoding |
|---|---|---|---|---|---|
| `musicpack-sonic-openl3-v1` | **supported default** | OpenL3 0.4.0 `music`/`mel256`/`emb512` | 512 | cosine | base64-f32le |
| `musicpack-sonic-discogs-v1` | registered, research-only | Discogs-EffNet `multi` | 1280 | cosine | base64-f32le |
| `musicpack-sonic-clap-v1` | registered, rejected | LAION-CLAP `larger_clap_music` | 512 | cosine | base64-f32le |

`musicpack-sonic-openl3-v1` is the default permissive MusicPack Sonic
profile, **not** a permanent normative recommendation model. Discogs-EffNet
produced materially better quantitative and perceptual similarity in
research but cannot be the mandatory profile (CC BY-NC-SA); OpenL3 was the
closest permissive option tested; CLAP was evaluated and not preferred.
Future permissively licensed, similarity-trained models can become new
profiles without changing the container format.

A profile not in the registry is **unknown**: the document remains
structurally readable and is validated against the parameters it declares
itself, but it is marked **unsupported for local semantic validation and
comparison** (see §10).

#### 3.1.1 `musicpack-sonic-openl3-v1` — pinned definition

OpenL3 0.4.0, `contentType music`, `inputRepr mel256`, 512-dim. Every
parameter is explicit — never a library default:

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

Pinned: window 1.0 s, `center=true`, hop 1.0 s, mel256 kapre frontend
(n_fft 2048, hop 242, decibel, pad_end), mean-norm pooling, L2, 512 dims,
cosine, album aggregation `equal-track-mean-l2` (equal-track mean → L2 over
contributing tracks), silence gate disabled. The weights SHA-256
`624ee7b1…` is mandatory for this profile. **Any profile-defining behavior
change requires a new profile id**; defaults never change based on the
installed library version.

#### 3.1.2 `musicpack-sonic-discogs-v1` — research/reference only

Discogs-EffNet `multi` (1280-dim, 16 kHz, internal 96-band log-mel,
131-frame patches @ 0.976 s hop). **Quality reference, CC BY-NC-SA — never
the mandatory profile.** Essentia (AGPL-3.0) is not a MusicPack dependency;
Discogs weights are never bundled and never auto-downloaded by MusicPack
Author; the profile is not the default. Registered in the profile registry
so packages claiming it can be structurally validated; it remains
**unsupported for comparison** and may be offered per-collection by a server
whose operator accepts non-commercial terms.

#### 3.1.3 `musicpack-sonic-clap-v1` — documented, rejected

LAION-CLAP `larger_clap_music` (512-dim, 48 kHz, 10 s segments). **Evaluated
and rejected**: worst diagnostics, not preferred in blind listening, least
agreement with the Discogs reference. Registered for provenance/research
compatibility; never a default or recommended profile; not productionized.

## 4. Package location and manifest reference

The sonic document lives at `analysis/sonic.json` inside the package:

```text
Album.mpack/
├── manifest.json
├── audio/
├── artwork/
├── booklet/
├── lyrics/
├── extras/
└── analysis/
    └── sonic.json
```

The canonical path is **`analysis/sonic.json`**. The manifest carries an
optional `analysis[]` reference (see §11). Existing `.mpack` packages
without sonic analysis remain completely valid; no `.mpack` v2 is created
for this.

## 5. Document structure

```json
{
  "format": "musicpack-sonic",
  "version": 1,

  "profile": {
    "id": "musicpack-sonic-openl3-v1",
    "dimensions": 512,
    "distance": "cosine",
    "encoding": "base64-f32le"
  },

  "analyzer": {
    "tool": "musicpack",
    "toolVersion": "7.0.1"
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
    },
    {
      "disc": 1,
      "track": 2,
      "embedding": null
    }
  ]
}
```

### 5.1 Field rules (normative)

- `format` — string, must be `"musicpack-sonic"`.
- `version` — integer, must be `1`. Unknown majors are rejected cleanly.
- `profile` — object, required. Members: `id` (string, required; see §6),
  `dimensions` (positive integer, required), `distance` (string, required),
  `encoding` (string, required; must be a supported v1 encoding).
- `analyzer` — object, required. Members: `tool` (string, required),
  `toolVersion` (string, required). Analyzer provenance must always be
  explicit; timestamps are omitted by default for determinism, matching the
  `.mpack` convention.
- `album` — object, required. Members: `embedding` (embedding object or
  `null`), `tracksContributing` (non-negative integer, required).
- `tracks` — array, required, **exactly one entry per manifest track**
  (`disc`+`track` addressing, matching `media[].tracks[]`). Each entry is an
  object with `disc` (positive integer), `track` (positive integer) and
  `embedding` (embedding object or `null`). Entries must be unique; entries
  must reference only manifest tracks.
- embedding object — `encoding` (string), `dimensions` (integer),
  `data` (base64 string). `encoding` and `dimensions` must equal the
  document's `profile.encoding` / `profile.dimensions`.

**Explicit no-embedding**: a track with no meaningful embedding is
`"embedding": null` — **never** a fabricated zero vector. The album
aggregation skips null tracks. `tracksContributing` is the count of
non-null track embeddings.

### 5.2 Album aggregation (normative)

For v1 the album embedding is stored, equal-track weighted:
`album = L2-normalize(mean of the contributing track vectors)`. The exact
rule is part of the profile (`albumAggregation`). The document stores the
resulting album vector plus `tracksContributing`; the server reads one
document instead of aggregating. `album.embedding` must be `null` when
`tracksContributing == 0` and a valid vector otherwise.

## 6. Profile identity (normative)

A profile id must match:

```text
musicpack-sonic- <model-segments> - v <digits>
```

- fixed prefix `musicpack-sonic-`;
- one or more `[a-z0-9]` segments separated by single `-` (the model name;
  no uppercase, no empty segments);
- a final `-v<digits>` version suffix with `digits >= 1`.

Examples: `musicpack-sonic-openl3-v1`, `musicpack-sonic-discogs-v1`,
`musicpack-sonic-clap-v1`, `musicpack-sonic-<new-model>-v1`.

Profile ids are stable strings, not hashes. They are scoped to a specific
frozen model/preprocessing/pooling definition (including the model weights
checksum where one is defined). Changing any profile-defining behavior
requires a new id. Vectors must never be compared across profile ids unless
an explicit conversion mechanism exists.

## 7. Vector representation (normative)

Fixed for v1: **`base64-f32le`**.

- numerical precision: IEEE-754 binary32 (float32);
- byte order: little-endian;
- encoding: base64 (standard alphabet, mandatory padding, no line breaks);
- dimensions: exactly `profile.dimensions`;
- normalization: unit L2 norm within tolerance `1e-3` (validated on read).

Validation rejects: NaN, Infinity, malformed base64, wrong decoded byte
length, wrong dimensions, and vectors whose L2 norm deviates from `1.0` by
more than `1e-3` (the norm is computed in double precision over the decoded
float32 values).

`binary-f32le` and decimal `json` remain conceivable alternatives for
specific consumers in a future format revision; v1 supports only
`base64-f32le`.

## 8. Size limits (normative)

A sonic document is untrusted input. Bounds:

- document JSON size: ≤ 16 MiB (same bound as `manifest.json`);
- vector dimensions: ≤ 1,048,576 (1 Mi dimensions, 4 MiB per vector);
- per-vector base64 data: bounded by the dimensions limit;
- `tracks[]` length: exactly the manifest track count when validated
  against a manifest; ≤ 4096 when validated standalone.

## 9. Unknown profile handling (normative)

A profile id that is syntactically valid but **not in the registry** must
not make a package unreadable:

- the document is still parsed structurally and validated against the
  parameters it declares itself (its own `profile.dimensions`,
  `profile.encoding`, `profile.distance`, vector encoding, byte length,
  finiteness, L2 norm, track uniqueness, album semantics, size limits);
- it is **not** validated against registry metadata (none exists);
- the package remains valid; verification reports the profile as
  **unsupported** (a warning), and the server must not compare its vectors
  with any other profile.

A registered but research-only/rejected profile (`discogs-v1`, `clap-v1`) is
structurally validated against its registered metadata and reported as
**unsupported for comparison**. A malformed document claiming a *known*
profile fails verification.

## 10. Security model (normative)

`analysis/sonic.json` is untrusted package input. Reading and verifying a
package only parses and validates data:

- paths in `analysis[]` obey the same canonical path rules as every other
  asset (no traversal, no absolute paths, `/`-separated, containment
  checked with realpath);
- document and vector size limits prevent allocation bombs;
- NaN/Inf and non-normalized floats are rejected;
- unsupported encodings are rejected;
- unexpected profile metadata never triggers anything: a package-provided
  profile id can never cause model downloading, model execution, or
  arbitrary plugin loading — analyzer selection happens only in trusted
  MusicPack Author configuration.

## 11. Manifest `analysis[]` (normative)

The manifest carries an optional, typed, extensible reference:

```json
{
  "analysis": [
    {
      "type": "sonic",
      "profile": "musicpack-sonic-openl3-v1",
      "path": "analysis/sonic.json",
      "sha256": "<64 lowercase hex>"
    }
  ]
}
```

- `type` — string, required. `"sonic"` is the v1 analysis type. Unknown
  future types are forward-compatible: preserved on read/write, validated
  structurally (path safety, sha256 form) but not semantically.
- `profile` — string, required when `type == "sonic"`. Must equal the sonic
  document's `profile.id`.
- `path` — package-relative, canonical `analysis/sonic.json`, validated by
  the standard path rules; unique across all package asset paths.
- `sha256` — lowercase hex, required when `type == "sonic"`; the document
  file is integrity-checked like any other asset.

Embeddings are never placed in `manifest.json`; the manifest only
references the document.

## 12. What is intentionally not stored

The `.mpack` sonic document stores: track sonic vectors, album sonic
vector, analysis profile/provenance, explicit no-embedding markers.

`musicpack-server` derives everything relational: similar tracks/albums,
artist representations (from album/track vectors), similar artists,
genre/style affinity (from metadata of nearby tracks/albums), radio
candidates. Artist embeddings are server-derived, never stored; genre
affinity emerges from the user's own collection, not a fixed ML ontology.

## 13. Auxiliary descriptors (optional, separate)

Tempo/BPM, key, mode are useful for radio transitions and discovery UX but
are not the primary similarity representation. They are optional and
omitted from the core profile until a permissively licensed, reasonably
deterministic implementation is demonstrated (Essentia/AGPL is not
acceptable as a mandatory dependency).

## 14. Future vector indexing (design note)

Exact cosine similarity is sufficient for small libraries (all-pairs over a
few thousand vectors is trivial); a future ANN index may be added for
larger collections. No vector-database dependency is introduced; start
simple.

## 15. Resolved design questions

Resolved by this freeze:

1. Document location: `analysis/sonic.json`, referenced from the manifest's
   `analysis[]` entries.
2. Album vector: stored (equal-track mean → L2), per §5.2.
3. Cross-codec: confirmed safe after `flac2mpc` (FLAC↔MPC-Q6 cosine
   ≥ 0.9995, OpenL3).
4. Default model: OpenL3 (permissive, closest to the Discogs reference) but
   **not normative** — a future permissively licensed, similarity-trained
   model becomes a new profile and candidate default without a container
   change.
5. `hop_seconds` stays a profile parameter (model-fixed hops, e.g.
   Discogs-EffNet's 0.976 s patch hop, are expressed per profile).
6. The model weights SHA-256 is mandatory in every profile definition that
   has immutable weights (the openl3-v1 definition carries it).
7. Production runtime: resolved by the production-integration phase (ONNX
   conversion spike vs isolated helper); the container is runtime-agnostic.
8. The container is **model-independent by contract**: a package must be
   readable, parseable and (structurally) valid regardless of which profile
   it claims.

## 16. Licensing constraints

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

## 17. Known limitation and model decision

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

Evidence: `research/sonic/reports/results.md`.
