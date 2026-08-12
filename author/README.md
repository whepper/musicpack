# MusicPack Author

MusicPack Author is the first-party **desktop GUI for authoring `.mpack`
releases**. It turns a tagged lossless FLAC album — or an already-tagged
Musepack album — into a curated, validated `.mpack` directory bundle,
presenting the album as a release/edition being authored rather than exposing
`manifest.json` as the UI. Since Phase 3 (the MVP) it also **encodes** FLAC/WAV
sources to Musepack SV8 (q6 default) in-app, so a terminal is never required.

```text
FLAC
  ↓
encode to Musepack (Author, ffmpeg + mpcenc)    ← Phase 3: new in-app stage
  ↓
tag-rich Musepack album                          ← or an existing MPC album
  ↓
.mpack (validated MusicPack v1 directory bundle)
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
musicpack backend                       ← inspect / validate-draft / encode-draft /
   ▼                                        build-draft / identify-draft / verify --json
libmusicpack (source of truth)          ← manifest semantics, hashes, BS.1770
   │
   ├─ mpcenc (static sidecar)           ← Musepack encoder (encode-draft)
    └─ ffmpeg (external, deterministic)  ← FLAC/WAV decode for encoding
```

The `musicpack` backend runs either as the **bundled sidecar** inside the
standalone `.app` (`BackendLocation::Bundled`) or, in development, from the
CMake build tree / `MUSICPACK_CLI` / `PATH` (`BackendLocation::Development`).
Packaged apps never consult PATH: see [Backend resolution](#backend-resolution)
below.

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

The CLI gained draft commands for this purpose (see `musicpack/main.c`):
`inspect`, `validate-draft`, `encode-draft`, `build-draft`, `identify-draft`,
and a `--json` mode on `verify`. `import`/`create`/`info` behaviour is
unchanged (the scan logic was extracted into a shared helper).
`author-api-version` is the machine-readable capability handshake the GUI
uses to verify backend compatibility (see [Backend compatibility](#backend-compatibility));
it is at version **2** since `encode-draft` was added.

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
- to use the encode stage: a built `mpcenc` (`cmake --build build -j --target mpcenc`, or on PATH) and **ffmpeg** (`brew install ffmpeg`)

Run the app:

```sh
# from the repository root
cmake -S . -B build && cmake --build build -j --target musicpack_cmd mpcenc

# from author/
npm install
npm run tauri dev
```

`npm run tauri dev` starts Vite on `http://localhost:5174` and opens the
native window. In development the Rust service resolves the `musicpack`
binary as `MUSICPACK_CLI` → `../build/musicpack/musicpack` →
`../build-static/musicpack/musicpack` → `PATH`; override with the
`MUSICPACK_CLI` environment variable. See
[Backend resolution](#backend-resolution) for the full policy.

Quality commands:

```sh
npm run check          # svelte-check (types + a11y)
npm test               # vitest: unit (node) + component (jsdom)
npm run build:web      # plain web build of the frontend
```

Backend tests: the `author_backend` and `author_encode` CTest suites
(`tests/run_author.sh`, `tests/run_encode.sh`, UNIX) drive the CLI draft
commands end to end, including the `author-api-version` handshake and the
FLAC→MPC encode stage (`author_encode` skips when ffmpeg is unavailable).

## Standalone macOS build

Build a self-contained, copy-anywhere `MusicPack Author.app` with one command
from the repository root:

```sh
./scripts/build-author-macos.sh
```

The resulting application bundle is:

```text
author/src-tauri/target/release/bundle/macos/MusicPack Author.app
```

The standalone application bundles its MusicPack authoring backend **and the
`mpcenc` encoder** and does not require a separate `musicpack` or `mpcenc`
installation. It also does not require CMake, Node.js, Rust, the source
repository, or `MUSICPACK_CLI`; copy the `.app` to another compatible Mac and
launch it. `/usr/bin/curl` (MusicBrainz) remains external. FFmpeg (decode) is
also external, but packaged builds discover it independently of PATH as
documented below.

The script (1) builds **fully static** `musicpack` and `mpcenc` binaries in
`build-author/`, (2) verifies with `otool -L` that they reference only macOS
system libraries, (3) stages them as Tauri sidecars, (4) runs the Svelte
frontend build and `npm run tauri build`, and (5) runs
`scripts/smoke-author-macos.sh` against the finished `.app`.

### Supported macOS architectures

Phase 1.1 builds for the **development machine's architecture** — arm64
(`aarch64-apple-darwin`) or x86_64 (`x86_64-apple-darwin`) — which is the
`host` reported by `rustc -vV`.

A universal binary is achievable later: build the C backend with
`-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` (CMake emits a universal Mach-O for
the sidecar), build the Rust side per-target
(`cargo build --target aarch64-apple-darwin --target x86_64-apple-darwin`),
and pass `--target universal-apple-darwin` to `tauri build` (which lipos the
app, or lipo the sidecars manually). CI would use `macos-latest` (arm64) plus
`macos-13` (x86_64) runners. This is documented as future work, not built in
this phase.

### Dynamic-library strategy

The backend is built with `-DMPC_BUILD_SHARED=OFF`, so `musicpack` links
`libmusicpack` and `libmusepack` **statically**; `mpcenc` is statically built
the same way. The resulting Mach-Os reference only `/usr/lib/libSystem.B.dylib`
— there are no third-party dylibs to bundle, no install-name rewriting, and no
Homebrew paths. The build hard-fails if any sidecar references anything
outside `/usr/lib`/`/System/Library` (`scripts/verify-backend-dylibs.sh`).

The only external tools the backend shells out to are `/usr/bin/curl` (system,
for MusicBrainz lookups) and **ffmpeg** (for FLAC/WAV decode during encoding).
FFmpeg is deliberately not bundled: the installed Homebrew FFmpeg 9.0 reports
`--enable-nonfree`, says it is not legally redistributable, and links a large
Homebrew dylib closure. The package audit rejects any bundled `ffmpeg*` file.

## Backend resolution

Resolution is decided once at startup and split into two explicit regimes so
a packaged app can never silently run an unrelated `musicpack`:

| Context                        | Order                                             |
|--------------------------------|---------------------------------------------------|
| Packaged app (release build)   | **bundled sidecar only** (`Contents/MacOS/musicpack`) |
| Development (`tauri dev`)      | `MUSICPACK_CLI` → `build/musicpack/musicpack` → `build-static/musicpack/musicpack` → `PATH` |

The encoder resolves the same way but separately: a packaged app uses the
`mpcenc` sidecar next to the bundled CLI; development uses `MUSICPACK_MPCENC`
→ the CMake build tree (`build/mpcenc/mpcenc`) → PATH. FFmpeg is never bundled.
In a packaged app it resolves in this deterministic order:

1. `MUSICPACK_FFMPEG`, when it names an existing file (use an absolute path).
2. `/opt/homebrew/bin/ffmpeg` (Homebrew Apple Silicon).
3. `/usr/local/bin/ffmpeg` (Homebrew Intel).
4. `/opt/local/bin/ffmpeg` (MacPorts).

It never searches PATH in a packaged app, including when started through
Finder. Development retains `MUSICPACK_FFMPEG` → PATH for convenience. FFmpeg
is only required when the user runs the encode stage.

- The packaged sidecar is located through Tauri's runtime path API
  (`BaseDirectory::Executable`), not guessed filesystem paths.
- If the bundled backend is missing, the app starts but every backend
  operation (and the startup banner) reports an actionable
  *"reinstall MusicPack Author"* error. It never falls back to PATH.
- The pure resolution logic lives in `AuthorService::resolve_bundled` /
  `resolve_development` and is covered by unit tests
  (`cargo test` in `author/src-tauri`), including the rule that production
  never consults environment, build tree, or PATH.

## Backend compatibility

The GUI and the bundled backend evolve together, so `musicpack` reports a
machine-readable capability handshake:

```sh
musicpack author-api-version --json
# {"musicpackVersion":"0.1.0","authorApi":1}
```

On the first backend operation the `AuthorService` runs this and rejects a
mismatched `authorApi` with a clear error
(`IncompatibleBackend`). Compatibility is coupled to the explicit authoring
API version, not to patch versions — this also protects development mode if
`MUSICPACK_CLI` points at an older executable. The frontend surfaces the
handshake result at startup via the `backend_info` command
(`BackendBanner`).

## Security

- **Content Security Policy.** `tauri.conf.json` sets a strict CSP:
  `default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self'
  data:; connect-src 'self' ipc: http://ipc.localhost ws://localhost:5174
  http://localhost:5174`. No `unsafe-eval`; no `unsafe-inline`. `data:` covers
  artwork previews; `ws/http://localhost:5174` is the Vite dev-server/HMR
  origin and is inert in the packaged app. MusicBrainz lookups happen in the
  native backend (via `curl`), so the webview is never granted internet
  access.
- **Capabilities (least privilege).** `capabilities/default.json` grants only
  `core:default` (drag/drop + IPC), `dialog:allow-open` (directory/image
  pickers), and `opener:allow-reveal-item-in-dir` ("Reveal in Finder"). No
  shell, filesystem, HTTP, or arbitrary command-execution permissions exist;
  all backend interaction flows through the typed Tauri commands and
  `AuthorService`.

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
   duration, filename, codec, sample rate/bit depth, ISRC and MusicBrainz
   recording/track IDs; basic in-place editing (no full tag editor yet).
4. **Encode to Musepack** — for lossless (FLAC/WAV) sources: encodes every
   track to Musepack SV8 (default q6, quality selectable in an advanced
   area) via ffmpeg + the bundled `mpcenc`, showing current track, overall
   progress, current stage and errors, with cancellation. Unknown/custom
   source tags pass through to the `.mpc` APEv2 tags. Your source files are
   never modified. (Skip this step to build a FLAC-backed package; already-
   Musepack albums skip it entirely.)
5. **Artwork/assets** — choose/change front artwork, add role-tagged artwork,
   and manage booklet/lyrics/extras. Embedded artwork is extracted at build
   time. (Phase 1 artwork files must live inside the album directory.)
6. **Identity** — enter a MusicBrainz release ID (exact match applied) or
   search by barcode for candidates with per-release confidence; applying a
   candidate fetches and applies that release. Confidence is always visible
   (`exact` / `confirmed` / `probable` / `none`) and never silently promoted.
7. **Validation** — a preflight view shows errors and warnings separately,
   reusing `.mpack` validation semantics through `validate-draft`; the
   authoritative gate is `musicpack_manifest_parse()` over a synthesized
   manifest. The GUI never invents required data to go green.
8. **Create MusicPack** — choose an output location (the package name is
   pre-filled from the album metadata, e.g. `Artist - Album.mpack`);
   `build-draft` copies and hashes the audio/assets, measures BS.1770-5
   loudness (album as one concatenated program), writes `manifest.json`, then
   runs full package verification. A package is never reported as successful
   if verification fails. On macOS, “Reveal in Finder” opens the result.

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
  failure, traversal rejection, MB identity application, and the
  `author-api-version` handshake.
- **Encode stage** (`tests/run_encode.sh`, CTest `author_encode`): the full
  FLAC→MPC q6 flow against the committed 2-disc fixture — staged `.mpc`
  naming, transformed draft, projected + passthrough APEv2 tags (via
  ffprobe), multi-disc build + `verify`, unsupported sample rate, mixed-source
  refusal, missing-tool pre-flight, and SIGTERM cancel + staging cleanup.
  Skipped when ffmpeg is unavailable.
- **Rust** (`cargo test` in `author/src-tauri`): backend resolution
  (MUSICPACK_CLI override, build-tree fallback, PATH fallback, bundled
  sidecar, actionable missing-backend errors, and the rule that packaged
  resolution never consults environment/tree/PATH) plus the author-API
  handshake parser and gate, mpcenc/ffmpeg resolution, `encode_spawn`
  argument passing, and staging-cleanup refusal of foreign paths.
- **Frontend** — vitest `tests/unit` (draft store, API command surface
  incl. `backend_info`, `encode_tracks`/`encode_cancel`/`cleanup_staging`,
  formatting incl. `defaultPackageName`/`needsEncoding`) and jsdom component
  tests `tests/component` (track list rendering, release form editing,
  validation rendering, create-button state, EncodePanel progress/error/
  cancel/swap).
- **Packaging** — `scripts/smoke-author-macos.sh` (run by
  `scripts/build-author-macos.sh`): `.app` exists, bundled backend and
  `mpcenc` present and executable, backend runs from its bundled location and
  reports `authorApi: 2`, only system dylibs referenced, and a harmless
  structured backend operation succeeds.

## Limitations (Phase 3 MVP)

- Loudness is measured at build time, not shown live per track.
- **Metadata edited after encoding** updates the manifest but not the
  `.mpc` APEv2 tags already written at encode time — edit before encoding, or
  re-import the album.
- **Encoding requires ffmpeg.** Packaged apps use `MUSICPACK_FFMPEG` or the
  fixed Homebrew/MacPorts locations above, never PATH; development uses PATH
  after the explicit override. It is never bundled. The app is still fully
  usable for already-encoded Musepack albums without it.
- FLAC/WAV are the supported encode sources; ALAC/APE/etc. are future work.
  Sources above 48 kHz cannot be encoded to Musepack (fixed-rate codec) and
  are surfaced as warnings only.
- Barcode candidate selection exists; artist/title MusicBrainz search does not.
- New artwork/assets must be inside the album directory (no external file
  copying yet).
- The standalone `.app` is built for the host architecture only (arm64 or
  x86_64, not yet universal).
- No signing/notarization/distribution: the bundle is ad-hoc signed by
  Tauri for local runs; notarized, signed releases and a GitHub Actions
  packaging job are future work (the build/smoke scripts are structured so a
  CI job is a thin wrapper).

## Sonic analysis

Sonic analysis computes a content-based audio embedding per track (and a
deterministic album embedding) into the package's optional
`analysis/sonic.json` (container format `musicpack-sonic` v1 — frozen and
model-independent; see `specs/musicpack-sonic-v1.md`). The UI exposes a
**model-independent** "Sonic Analysis" panel: profile *MusicPack OpenL3 v1*
(`musicpack-sonic-openl3-v1`, the default permissive profile — not a
permanent normative model), with states *not analysed → analysing n/m
tracks (cancelable) → ready / ready-with-warnings / error*, and a
re-analyse action. Analysis is never started automatically; a package can
always be built without sonic.

```text
Sonic Analysis panel (Author)
      ↓  sonic_analyze / sonic_cancel (Tauri commands)
AuthorService → spawns `musicpack-sonic` (the analyzer binary)
      ↓  job JSON (draft audio paths + app-data model/cache/output dirs)
      ↓  progress events (sonic-progress) + cancellation (SIGTERM)
sonic.json written to the app data directory (outside the package)
      ↓  Draft.sonicAnalysis.path
build-draft (create_package) copies it to analysis/sonic.json, validates it
      and writes the manifest's analysis[] reference (sha256-protected)
```

### The analyzer (`musicpack-sonic`)

The analyzer lives in `sonic/` (C11 + ONNX Runtime, single-threaded for
determinism): it decodes MPC/FLAC/WAV to mono float32, resamples with a
faithful port of resampy `kaiser_best`, runs the mel frontend (kapre
STFT/mel/decibel) and the SHA-256-pinned post-frontend ONNX graph, pools
with mean-norm and aggregates the album equal-track. Compatibility against
the research harness is measured by `research/sonic/compat_measure.py
--c-doc` (gates: cosine ≥ 0.9999, meandiff ≤ 1e-4, maxdiff ≤ 2e-3 — all
PASS on the deterministic corpus). libmusicpack remains the authority on
Sonic semantics; the analyzer never reimplements them.

The model is **not bundled** in the `.app`. The ONNX Runtime runtime is
bundled, but the ~18 MB post-frontend ONNX artifact is downloaded once on
first use and verified against a pinned SHA-256 (`3b4b7dac…`, 18,742,941
bytes) before activation. Acquisition is trusted Author application logic
(`src-tauri/src/sonic_model.rs`) — a package-provided profile id can never
trigger a download or model execution. The model cache lives at:

```text
Application Support/MusicPack Author/sonic/models/musicpack-sonic-openl3-v1/openl3_post.onnx
```

(resolved through the platform-native Tauri app-data path, never hardcoded).
Offline with a valid cached model, analysis works normally; offline without
one, the panel reports that the model could not be downloaded and the package
can still be built without sonic. Per-track embeddings are cached by audio
SHA-256 + profile + weights, so re-analysis of unchanged audio is free.

The model artifact is generated **reproducibly** from the pinned OpenL3 0.4.0
weights (CC BY 4.0, marl/openl3) by `research/sonic/convert_openl3.py`;
normal users download the already-produced, SHA-pinned artifact from the
immutable release asset (`scripts/publish-sonic-model.sh`), never a `latest`
asset and never a Python/ONNX-conversion step.

### Backend resolution

The analyzer resolves like the CLI but separately: a packaged app uses the
`musicpack-sonic` sidecar next to the bundled CLI; development uses
`MUSICPACK_SONIC`, then the CMake build tree (`build/sonic/musicpack-sonic`).
It is only required when the user actually runs a sonic analysis.

### Standalone macOS implications

- The `.app` bundle gains the `musicpack-sonic` sidecar and a relocatable
  **ONNX Runtime dylib** (`Contents/Frameworks/libonnxruntime*.dylib`,
  loaded via `@loader_path/../Frameworks`). arm64 bundles ONNX Runtime
  1.28.0; x86_64 uses 1.23.0 (the last Intel-macOS ONNX Runtime release) —
  both pinned + checksummed by `scripts/build-author-macos.sh`.
- The ~18 MB post-frontend **model** is fetched once on first use
  (pinned + SHA-256-verified) into the app data directory; it is not in the
  bundle.
- The build runs `scripts/audit-author-macos.sh` as a gate: it fails on a
  missing piece, a Homebrew/local/external dependency, an absolute rpath, or
  a mixed architecture.
- Analysis RAM is far below the research TensorFlow stack (~1.9 GB): the
  ONNX Runtime path runs a single-threaded session with a few hundred MB.
- arm64 and x86_64 both build (host-architecture only); a universal build
  remains future work (the analyzer is compiled per-host like the CLI
  sidecar, and ONNX Runtime would need a universal build).

### Clean-machine smoke procedure

On a clean macOS user account (or equivalent isolated environment):

1. `./scripts/build-author-macos.sh` → `MusicPack Author.app`.
2. Confirm no Homebrew ONNX Runtime is reachable via loader paths
   (`echo $DYLD_LIBRARY_PATH` empty; `brew list | grep onnx` nothing).
3. Launch Author, import an album, click **Analyse Sonic**.
4. Confirm the first-use model acquisition (~18 MB) with progress and
   SHA-256 verification.
5. Confirm the analysis completes and the `.mpack` builds.
6. `musicpack verify <album>.mpack --json` — confirm `analysis/sonic.json`
   is present and valid.
7. Quit/relaunch Author and re-analyse — confirm the cached model is reused
   with no network access.

(Verified on this machine up to and including the build/audit/smoke gates and
the analyzer's relocatable load; the full GUI click-through + first-use
download is the remaining manual step on a clean Mac.)
