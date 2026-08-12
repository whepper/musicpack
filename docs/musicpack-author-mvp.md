# MusicPack Author — MVP

Status: **MVP complete.** MusicPack Author turns a tagged lossless FLAC album
into a validated `.mpack` release directory, including a Musepack q6 encode
stage, without requiring the user to touch a terminal.

This document records the MVP architecture, the GUI technology decision, the
authoring pipeline, metadata handling, encoding integration, package
generation and validation, sonic-analysis status, limitations and the planned
extensions.

## 1. What MusicPack Author is

A first-party desktop application for **authoring `.mpack` releases**:

```text
SOURCE                        →  .mpack
tagged FLAC album                valid, verified release directory
   ↓ inspect                        ↓ assembled from encoded Musepack
   ↓ review / edit metadata         ↓ sha256-protected manifest
   ↓ encode to Musepack q6          ↓ optional sonic analysis
   ↓ build + validate
```

It is **not** a generic batch transcoder and **not** a GUI wrapper around a
shell script. Authoring is a pipeline with a clear internal representation at
every stage, and every error is attributable to a specific stage. The GUI
orchestrates services; it does not contain authoring logic.

The `.mpack` v1 specification (`specs/musicpack-v1.md`) and its JSON Schema
(`specs/musicpack-v1.schema.json`) are authoritative. MusicPack Author never
invents a parallel schema; `libmusicpack` owns all package semantics.

## 2. GUI technology choice

**Tauri 2 (Rust) + Svelte 5 (TypeScript), on macOS.** This decision predates
this MVP (Phase 1.1) and was re-confirmed here:

| Criterion | Choice |
|---|---|
| mature / actively maintained | Tauri 2 is stable and widely adopted; Svelte 5 is current |
| easy packaging on macOS | single `scripts/build-author-macos.sh` produces a self-contained `.app` |
| filesystem integration | Tauri `dialog`/`opener` plugins; native sidecars |
| process execution | Rust `std::process` around the `musicpack` CLI (isolated in `AuthorService`) |
| path to Windows/Linux | Tauri is cross-platform; the core authoring logic is backend C + plain TS |

**Electron was deliberately not chosen.** The application needs little from a
browser runtime: no large web ecosystem, and the heavy lifting (package
semantics, checksums, loudness, MusicBrainz identity) already lives in the
`musicpack` CLI. Electron would add an order of magnitude more bundle size and
a Node runtime with no architectural benefit.

**Why the GUI talks to the `musicpack` CLI, not a Rust FFI.** The album-scan
and package-assembly logic lives in the CLI (`musicpack/main.c`), not in
`libmusicpack`. Direct FFI would mean reimplementing that logic in Rust
(forbidden — no second metadata parser/package logic) or first moving it into
the library. So `AuthorService` (the only Rust code that spawns the CLI) talks
structured JSON, and the frontend never parses CLI text. A future direct
`libmusicpack` binding can replace the subprocess without touching the
frontend.

## 3. Authoring pipeline

```text
SOURCE → inspect → normalize metadata → encode → assemble → validate → publish
```

Core model (application state, not a MusicPack format): the **authoring
draft** — `musicpack-draft` JSON that mirrors the `.mpack` v1 hierarchy
(`album` release group, `release` edition, `media[]`, `tracks[]`,
`identifiers`, `identity`, `source`, artwork/`booklet`/`lyrics`/`extras`)
while keeping `release`, `source` and `identity` strictly separate, exactly as
the spec requires. The draft crosses the CLI boundary as JSON only.

| Stage | Backend surface | Responsibility |
|---|---|---|
| Source inspection | `inspect --json` | scan a directory into a draft; tag union, disc grouping, stream probes, artwork |
| Metadata normalization | `inspect` + `validate-draft` | map source tags to the `.mpack` model with deterministic precedence |
| Encode | `encode-draft` | FLAC/WAV → tagged Musepack SV8 q6 in a staging directory |
| Assemble | `build-draft` | copy encoded audio + artwork/assets, measure BS.1770-5 loudness, write `manifest.json` |
| Validate | `musicpack verify --json` | full package verification (mandatory gate) |
| Publish/export | GUI | reveal the `.mpack` in Finder; server ingestion is the next milestone |

`encode-draft` is the stage added by this MVP. It:

1. validates the draft (never encodes an invalid one),
2. pre-flights every source (FLAC/WAV only, sample rate ∈ 32/37.8/44.1/48 kHz,
   encoder + ffmpeg available),
3. encodes each track into `staging/audio/` via **ffmpeg decode → `mpcenc`**
   and writes APEv2 tags,
4. copies artwork (external + embedded) and assets into the staging area,
5. returns a **transformed draft** whose `audioPath` values point at the
   encoded `.mpc` files and whose `sourceRoot` is the staging directory.

Progress is streamed as JSON events (`encode-progress`): per-track
`stage`/`track` lines and a final `done`/`error`/`cancelled` event. The GUI
shows current track, overall progress, current stage and errors, and allows
cancellation (SIGTERM → exit 130, staging removed).

## 4. Metadata mapping

Source FLAC Vorbis Comments are mapped to canonical `.mpack` fields by
`libmusicpack` (`musicpack_tag_map_album` / `musicpack_tag_map_track`) and
projected back onto each encoded `.mpc` as APEv2 tags
(`musicpack_manifest_to_ape_tags`):

| Source key | `.mpack` model | Encoded `.mpc` APEv2 |
|---|---|---|
| TITLE | `track.title` | `Title` |
| ARTIST | `track.artists[]` (role `main`) | `Artist` |
| ALBUM | `album.title` | `Album` |
| ALBUMARTIST | `album.artists[]` (role `main`) | `Album Artist` |
| TRACKNUMBER | `track.number` | `Track` (as `n/total`) |
| TRACKTOTAL / TOTALTRACKS | — | folded into `Track` |
| DISCNUMBER | `media[].disc` | `Disc` (as `n/total`) |
| DISCTOTAL / TOTALDISCS | — | folded into `Disc` |
| DATE / YEAR | `release.releaseDate` / `originalReleaseDate` | `Year` |
| GENRE | `album.genres[]` | `Genre` (multi-value kept) |
| COMPOSER | `track.artists[]` (role `composer`) | `Composer` |
| ISRC | `track.identifiers.isrc` | `ISRC` |
| MUSICBRAINZ_* IDs | `identifiers`, `track.identifiers` | `MusicBrainz … Id` |
| PUBLISHER/LABEL | `release.label` | `Label` |
| CATALOGNUMBER | `release.catalogueNumber` | `CatalogNumber` |
| BARCODE | `identifiers.barcode` | `Barcode` |
| SOURCE / SOURCEID | `source` / `track.source` | `Source` / `SourceId` |

**Unknown / custom tags are preserved, not dropped.** Any source tag without a
canonical mapping is passed through verbatim to the `.mpc` APEv2 tag of the
same key (semantic preservation, mirroring `flac2mpc`): multiple values stay
separate, and tags like `COMMENT`, `LYRICS`, `MOOD`, or domain-specific fields
survive the encode. The passthrough exclusion list is exactly the canonical
mapped keys plus the ReplayGain/R128 and cover-art keys that MusicPack handles
explicitly.

**ReplayGain is not copied.** Per `specs/musicpack-v1.md` §5, canonical
MusicPack loudness is **ITU-R BS.1770-5**, measured at build time as an album
program (`musicpack` measures per-track and album loudness from decoded PCM).
Classic ReplayGain tags are import-time compatibility data only and are never
treated as canonical; stale source ReplayGain/R128 values are excluded from
passthrough so they cannot conflict with measured values.

**Artwork.** External cover files (`cover.jpg`, `folder.jpg`, …) are kept as
the `front` role; embedded FLAC PICTURE blocks are extracted at build time
into `artwork/` with their MIME-based extension. `musicpack create`/`import`
and `build-draft` all preserve artwork; the encode stage re-stages it.

**FLAC-specific structure** (stream info, MD5, block layout) has no APEv2
representation and is not transferred — this is inherent to the tag systems,
not a MusicPack choice.

## 5. Source precedence

Deterministic, enforced by the pipeline:

1. **Explicit user edits** in the Author UI (the draft the user submits).
2. **MusicPack-normalized metadata** derived from source tags (the `inspect`
   union + `validate-draft`).
3. **Existing source tags** (Vorbis/APEv2, first-wins per field across the
   album — a minimal tag on the first file never shadows a rich tag later).
4. **Filename/path inference** as a last resort only (track numbers/titles
   when untagged).

Filename guesses never overwrite high-quality MusicBrainz metadata: MB IDs in
source tags seed the draft's `identifiers`/`identity`, and `identify-draft`
only applies an MB release when the match is evidence-backed
(`exact`/`confirmed`/`probable`), never silently promoted.

## 6. Encoding integration

Musepack encoding uses the **maintained Musepack toolchain** — the repo's own
`mpcenc` (quality scale, default **q6**). No encoder is embedded in Author and
no encoder behavior is changed.

Per track, the backend (`encode-draft`) runs:

```text
ffmpeg -v error -y -i <source.flac> -vn -f wav <staging/track.wav>
mpcenc --quality 6.0 --overwrite --silent <staging/track.wav> <staging/audio/NN - Title.mpc>
# then libmusicpack writes the APEv2 tags and SHA-256 is computed
```

Notes:

- **Sample-rate gate.** Musepack is a fixed-rate subband codec: only
  32/37.8/44.1/48 kHz sources can be encoded. Unsupported rates fail with
  `UNSUPPORTED_SAMPLE_RATE` and are surfaced as a *warning* in
  `validate-draft` (warnings never block a build by themselves).
- **Source formats.** FLAC (priority) and WAV are supported for encoding; the
  input layer is structured so further lossless formats (e.g. ALAC) slot in
  later. Albums mixing already-encoded Musepack with FLAC are refused by the
  encode stage (`UNSUPPORTED_SOURCE`) rather than half-encoded.
- **Binary discovery.** The bundled `.app` carries `mpcenc` as a static
  sidecar next to `musicpack`. Development uses `MUSICPACK_MPCENC` → the CMake
  build tree (`build/mpcenc/mpcenc`) → PATH. FFmpeg is an external tool
  (`MUSICPACK_FFMPEG` → PATH), like `/usr/bin/curl` for MusicBrainz.
- **Cancellation and cleanup.** SIGTERM kills the active encode and removes
  the partial staging directory; a cancelled run never leaves a partial
  bundle. On success the staging directory is kept only long enough for
  `build-draft`, then removed by the GUI (`cleanup_staging`). Source files are
  **never modified** — all authoring work happens in staging/output locations.

## 7. `.mpack` generation

`build-draft` (unchanged semantics) assembles the v1 directory bundle from the
(possibly encoded) draft:

```text
Album.mpack/
├── manifest.json
├── audio/      (encoded Musepack SV8, sha256-protected)
├── artwork/    (role-tagged)
├── booklet/ lyrics/ extras/
└── analysis/   (optional sonic.json)
```

Audio object names are `NN - Title.mpc` for single-disc albums and
`D-NN - Title.mpc` on multi-disc albums (object paths must be unique across
the package). Package naming is a sensible default derived from normalized
metadata (`"Artist - Album"`, filesystem-invalid characters sanitized) but the
**manifest remains the authoritative identity**, never the filename.

Loudness is measured with the BS.1770-5 meter over the album as one
concatenated program (`musicpack_meter`), recorded per track and per album,
never aggregated from per-track values.

## 8. Validation

Validation is mandatory and never assumed:

1. `validate-draft` — targeted authoring errors/warnings *and* the
   authoritative gate: a synthesized manifest is parsed through
   `musicpack_manifest_parse()`, so the real `.mpack` v1 rules apply.
2. `build-draft` **refuses to assemble a draft that fails validation**, and
   after writing `manifest.json` runs `musicpack_package_verify()` on the
   result — a package is never reported successful if verification fails.
3. The GUI's Create dialog re-verifies on demand and shows `MusicPack valid`
   or the actionable error list (missing files, checksum mismatch, malformed
   manifest, duplicate/invalid numbering, enumeration violations).

## 9. Sonic analysis

**Integrated (not deferred).** Sonic analysis computes a versioned audio
embedding per track (OpenL3 profile `musicpack-sonic-openl3-v1`) plus a
deterministic album embedding into the package's optional
`analysis/sonic.json` (`specs/musicpack-sonic-v1.md`). Analysis is always
optional, never auto-started, and a package can be built without it. The
analyzer (`musicpack-sonic` sidecar) is model-independent: the ~18 MB model is
acquired once, SHA-256-verified, cached, and offline re-analysis reuses the
cache. Audio is analysed once after encoding from the best source stage
(decoded PCM of the `.mpc`).

## 10. Testing

Backend (CTest):

- `author_backend` (`tests/run_author.sh`) — inspect, validation semantics,
  build/verify, identity, handshake.
- `author_encode` (`tests/run_encode.sh`) — the encode stage against the
  committed 2-disc fixture: staged `.mpc` naming, transformed draft, APEv2
  tag assertions via ffprobe (projected + passthrough), multi-disc build +
  `verify`, unsupported sample rate, mixed-source refusal, missing-tool
  pre-flight, SIGTERM cancel + staging cleanup. Skips without ffmpeg.
- `author_app_audit` — packaging gate for the standalone `.app`.

Rust (`cargo test` in `author/src-tauri`) — backend resolution (bundled/
development, mpcenc/ffmpeg), the author-API handshake (now version 2),
`encode_spawn` argument passing, and staging cleanup refusal of foreign
paths.

Frontend — vitest unit tests (API command surface incl. `encode_tracks`/
`encode_cancel`/`cleanup_staging`, formatting incl. `defaultPackageName`/
`needsEncoding`/`sanitizeFilename`) and jsdom component tests
(`encode-panel.test.ts`: hidden for already-MPC albums, progress rendering,
transformed-draft swap, error + retry, cancel clearing staging, quality
pass-through).

Real-world fixture: `tests/reference/author-fixture/Neon Skyline/` — a small
**generated** two-disc album (no copyrighted material) with multiple tracks,
full Vorbis tags, MusicBrainz-style identifiers, album-artist vs track-artist
difference, a featuring credit, an unknown custom tag, external `cover.jpg`
and embedded artwork. Regenerated by `tests/generate_author_fixture.py`.

## 11. Logging

The app logs the backend's structured output; `encode-draft` emits
application-version, tool versions (`musicpack`, `mpcenc`, `ffmpeg`), input
file count, selected quality, analysis state and the validation outcome as
JSON events. No personal filesystem data beyond paths required for debugging.

## 12. Known limitations (MVP)

- **Metadata edits after encoding** apply to the manifest but not to the
  already-written `.mpc` APEv2 tags; edit before encoding, or re-import.
  (`encode-draft` writes tags at encode time; `build-draft` copies the encoded
  files verbatim.)
- **FFmpeg is an external requirement** for encoding (not bundled; like
  `curl`). The app is unusable for FLAC sources without it.
- Source formats limited to FLAC/WAV for encoding; ALAC/APE/etc. are future
  work. 88.2/96/176.4/192 kHz sources cannot be encoded to Musepack (fixed
  sample-rate codec) and are only surfaced as warnings.
- `.mpack` files already containing FLAC remain valid; the encode stage is
  offered for lossless sources but a FLAC-based package can still be built.
- No automatic MusicBrainz network lookup at import (only explicit
  `identify-draft`), no artist/title search.
- The standalone `.app` is host-architecture only; no signing/notarization.
- Windows/Linux builds are not yet exercised (core logic is portable; the
  shell tests are UNIX-only).

## 13. Future extensions

- Server upload / publish to `musicpack-server`.
- MusicBrainz import-time search and auto-lookup.
- More lossless source formats and high-rate resampling with an explicit
  policy.
- Post-encode re-tagging from the final manifest (closing the §12 gap).
- Universal (arm64 + x86_64) app bundle, signing + notarization, CI packaging.
- WavPack/hybrid support, alternate codecs, packed `MPAK` container — all
  deliberately out of scope for the MVP but designed for in the `.mpack`
  storage abstraction.
