# MusicPack Author

MusicPack Author is the first-party **desktop GUI for authoring `.mpack`
releases**. It turns a tagged Musepack album — primarily the output of
`flac2mpc` — into a curated, validated `.mpack` directory bundle, presenting
the album as a release/edition being authored rather than exposing
`manifest.json` as the UI.

```text
FLAC
  ↓
flac2mpc
  ↓
tag-rich Musepack album          ← MusicPack Author works on this
  ↓
.mpack
```

The application is part of the MusicPack product family: same visual language
as the `web/` record-shelf client (warm paper/ink editorial palette, hairline
rules, serif headings), Svelte 5 + TypeScript, running as a Tauri 2 desktop
app on macOS (Linux/Windows are not yet a focus).

## Architecture

```text
Svelte 5 app (app/)                     ← in-memory AuthoringDraft, record-shelf UI
   │  invoke() via @tauri-apps/api
   ▼
Tauri commands (src-tauri/src/lib.rs)   ← thin JSON surface
   │  AuthorService (src-tauri/src/author_service.rs)
   ▼
musicpack CLI (JSON modes)              ← inspect / validate-draft / build-draft /
   ▼                                        identify-draft / verify --json
libmusicpack (source of truth)          ← manifest semantics, hashes, BS.1770
```

**`libmusicpack` stays authoritative.** The GUI never reimplements the
`.mpack` format: package semantics, validation, checksums, metadata
reconciliation, MusicBrainz identity and loudness all come from the existing
`musicpack` implementation.

**Why the CLI is wrapped instead of linked directly.** The album-scanning and
package-assembly logic currently lives in the `musicpack` CLI
(`musicpack/main.c`), not in `libmusicpack`. Direct FFI would have meant
either reimplementing that logic in Rust (forbidden: no second metadata
parser / package logic) or first moving it into the library (a real
`libmusicpack` API change). So Phase 1 wraps the CLI behind a clean service:

- every interaction uses **structured JSON** (`inspect --json`, etc.), never
  prose parsing;
- subprocess spawning is isolated inside `author_service.rs` — the only place
  that touches the CLI;
- the `AuthorService` interface is designed so a direct `libmusicpack` binding
  can replace the subprocess later without touching the frontend.

The CLI gained new draft commands for this purpose (see
`musicpack/main.c`): `inspect`, `validate-draft`, `build-draft`,
`identify-draft`, and a `--json` mode on `verify`. `import`/`create`/`info`
behaviour is unchanged (the scan logic was extracted into a shared helper).

## The authoring draft

The GUI edits an in-memory **authoring draft** (application state, not a
MusicPack format) and serializes it to a `musicpack-draft` JSON only to cross
the CLI boundary. It mirrors the `.mpack` v1 logical hierarchy — `album`
(release group), `release` (specific edition), `media[]`, `tracks[]` — plus
`identifiers`, `identity`, `source`, `artwork`, `booklet`, `lyrics`, `extras`.
`release`, `source` and `identity` are kept strictly separate, exactly as the
spec requires. Audio/artwork paths point at files under `sourceRoot`; no
half-created package directory is mutated as the user edits.

## Development

Requirements:

- macOS with **Xcode Command Line Tools** (`xcode-select --install`)
- **Node.js ≥ 20** and npm
- **Rust toolchain** (Tauri 2 backend): `curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh`
- a built `musicpack` CLI (the backend): `cmake -S . -B build && cmake --build build -j --target musicpack_cmd`

Run the app:

```sh
# from the repository root
cmake -S . -B build && cmake --build build -j --target musicpack_cmd

# from author/
npm install
npm run tauri dev
```

`npm run tauri dev` starts Vite on `http://localhost:5174` and opens the
native window. The Rust service locates the `musicpack` binary at
`../build/musicpack/musicpack` (dev default) or on `PATH`; override with the
`MUSICPACK_CLI` environment variable.

Quality commands:

```sh
npm run check          # svelte-check (types + a11y)
npm test               # vitest: unit (node) + component (jsdom)
npm run build:web      # plain web build of the frontend
```

Backend tests: the `author_backend` CTest suite
(`tests/run_author.sh`, UNIX) drives the CLI draft commands end to end.

## Current workflow

1. **Add album** — choose or drag/drop an album directory; `inspect` scans it
   into a draft (tags, disc grouping, track durations/codec via cheap header
   probes, artwork: file-based or embedded). No package is created.
2. **Release metadata** — editable form covering the `.mpack` v1 model:
   release-group (title, artists, type, original date, genres), specific
   release (date, edition, country, label, catalogue number, notes),
   identifiers (MB release-group/release IDs, barcode), source (type, store,
   source ID), and media (disc number, format, title).
3. **Tracks** — disc-grouped list with number, title, per-track artist,
   duration, filename, codec, ISRC and MusicBrainz recording/track IDs;
   basic in-place editing (no full tag editor yet).
4. **Artwork/assets** — choose/change front artwork, add role-tagged artwork,
   and manage booklet/lyrics/extras. Embedded artwork is extracted at build
   time. (Phase 1 artwork files must live inside the album directory.)
5. **Identity** — enter a MusicBrainz release ID (exact match applied) or
   search by barcode for candidates with per-release confidence; applying a
   candidate fetches and applies that release. Confidence is always visible
   (`exact` / `confirmed` / `probable` / `none`) and never silently promoted.
6. **Validation** — a preflight view shows errors and warnings separately,
   reusing `.mpack` validation semantics through `validate-draft`; the
   authoritative gate is `musicpack_manifest_parse()` over a synthesized
   manifest. The GUI never invents required data to go green.
7. **Create MusicPack** — choose an output directory; `build-draft` copies
   and hashes the audio/assets, measures BS.1770-5 loudness (album as one
   concatenated program), writes `manifest.json`, then runs full package
   verification. A package is never reported as successful if verification
   fails. On macOS, “Reveal in Finder” opens the result.

## MusicBrainz identity

Uses the existing `musicpack` MusicBrainz code (`musicpack_mb_match_confidence`
/ `musicpack_mb_apply_release`). Exact-ID identification works now; barcode
candidate listing works; artist/title search is a Phase 2 backend item (the
UI/service boundaries already exist). Offline matching is supported via
`--mb-json`.

## Loudness

Canonical `.mpack` loudness is BS.1770-5, owned by `libmusicpack`. MusicPack
Author does not use ReplayGain tags as canonical loudness; it is measured at
package build time (the authoring view shows “Loudness · at build”).

## Testing

- **Backend** (`tests/run_author.sh`, CTest `author_backend`): inspect a
  valid MPC album, import canonical metadata, preserve release vs source vs
  identity semantics, validation error propagation, successful package
  creation, post-build verification, failed verification surfaced as
  failure, traversal rejection, and MB identity application.
- **Frontend** — vitest `tests/unit` (draft store, API command surface,
  formatting) and jsdom component tests `tests/component` (track list
  rendering, release form editing, validation rendering, create-button state
  following validation).

## Limitations (Phase 1)

- Loudness is measured at build time, not shown live per track.
- Barcode candidate selection exists; artist/title MusicBrainz search does not.
- New artwork/assets must be inside the album directory (no external file
  copying yet).
- No signing/notarization/distribution; macOS development runs only. No CI
  job for the author app yet.

## Future: sonic analysis hook

Sonic analysis is **not** implemented in Phase 1 and no fake fields are put
into `manifest.json`. The UI already reserves the status chip
`Sonic Analysis · not analysed`, and the draft model has no `analysis`
concept. The planned pipeline is:

```text
Import → Metadata / Identity → Assets → Analysis (BS.1770-5 · sonic) → Validate → Create
```

The likely future model is an optional package asset such as
`analysis/sonic.json`, referenced from the manifest with type/path/hash/
algorithm metadata — to be designed and specified separately, not ad hoc.
