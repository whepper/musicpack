# MusicPack

MusicPack is an open, self-hosted music ecosystem built around a simple idea:
**an album should be a first-class digital object — not merely a folder full
of loosely related files.**

A specific CD, vinyl pressing, remaster or digital edition is more than a set
of tracks: it is a particular release with its own identity, artwork,
mastering, catalogue information and provenance. MusicPack keeps that identity
inside the album object itself, so a digital collection can feel like a
**digital record shelf** — intentional, curated, and collectible again. Two
editions of the same album are meaningful rather than redundant, because the
library understands why they are different.

The project deliberately gives **Musepack** a starring role. Musepack is a
mature, open and exceptionally efficient lossy codec — still one of the best
ever designed for transparent music playback — and it is the audio foundation
everything here is built on:

```text
Musepack  = the codec      (.mpc / SV7 / SV8)
MusicPack = the ecosystem  (.mpack packages, server, web client, tools)
```

The guiding rule of this repository:

> **Modernize the ecosystem aggressively; change the codec conservatively.**

## What this repository is

- **The Musepack codec (foundation)** — the decoder (`libmpcdec`), the encoder
  core (`libmpcenc`, `libmpcpsy`), a WAV helper (`libwavformat`), and the CLI
  codec tools. The decoder supports SV7/SV8; the encoder produces SV8.
- **The `.mpack` album model** — `libmusicpack` owns the package format: a
  release-group → release/edition → media → track hierarchy, assets, SHA-256
  integrity and BS.1770-5 loudness; the `musicpack` CLI authors, imports and
  verifies packages. The format is codec-aware rather than Musepack-only —
  Musepack is the foundation codec, but FLAC-backed packages are valid v1.
- **The server** — `musicpack-server` indexes an `.mpack` collection into a
  SQLite collector library and serves it over a read-only HTTP API v1 with
  **direct streaming** (the original audio bytes, never transcoded).
- **The web client** — `web/` is the first-party "digital record shelf": browse
  your collection by album and edition, then play it with Musepack
  demand-driven WASM decoding, BS.1770 album normalization, gapless album
  playback and Media Session integration.
- **The desktop authoring GUI** — `author/` is **MusicPack Author**, a Tauri 2
  desktop app (macOS/Apple Silicon is the current packaged target) that turns a
  tagged lossless album into a curated, validated `.mpack` release through a
  real authoring pipeline: inspect → metadata review → Musepack q6 encoding
  (bundled static `mpcenc`, FLAC/WAV decoded via ffmpeg) → `.mpack` assembly →
  validation. ffmpeg is not bundled; packaged macOS builds find it
  deterministically via `MUSICPACK_FFMPEG` or `/opt/homebrew/bin`,
  `/usr/local/bin`, `/opt/local/bin` — never the shell PATH. It drives the
  existing `musicpack` implementation through the
  CLI's JSON draft modes (`inspect`, `validate-draft`, `encode-draft`,
  `build-draft`, `identify-draft`) behind a clean service, so `libmusicpack`
  stays the only authority on package semantics. Source files are never
  modified; package finalization is transactional and verified before it is
  reported successful.
- **WASM + demo** — the decoder builds to WebAssembly (Emscripten); `demo/` is
  a low-level playback proof-of-concept kept as a development/test artifact.

---

## The web client — a digital record shelf

`web/` is the first genuinely usable MusicPack client. It is
deliberately a *digital record shelf*, not a streaming catalogue:

- sign in once with a server token (exchanged for an **HttpOnly session
  cookie**; the token is never stored in the browser)
- an artwork-first **album shelf** with infinite scrolling, search and
  "recently added"
- an **album page** that shows which exact release/edition you are looking at
  (edition chips, disc-grouped track lists, an expandable release-information
  panel with identity, provenance, codec and BS.1770 details)
- **playback** through one controller over two backends: the Musepack
  demand-driven engine (SharedArrayBuffer reader → decoder workers → an
  AudioWorklet ring) for `.mpc`, and the browser's native media stack for
  FLAC and other supported codecs
- BS.1770 album/track normalization (default −16 LUFS, true-peak capped),
  **gapless album transitions**, a queue, Media Session, and a responsive
  mobile/desktop UI

The client is a static Svelte 5 + Vite + TypeScript build served by
`musicpack-server --static-dir web/app/dist`. See `web/README.md` for the
stack decision, the `npm run dev` / `npm run build` workflow, and the
Vitest/Playwright suites.

## libmusicpack — the `.mpack` package model

`libmusicpack` owns MusicPack package semantics: the `.mpack` v1 manifest,
album/release-group → release/edition → media → track model, assets, SHA-256
integrity, BS.1770-5 loudness (measured per track and as an album program)
and directory-bundle storage. It depends on `libmusepack` (for the Musepack
track handoff) but never the reverse. It builds as `libmusicpack`, exported
as `MusicPack::Package`:

```cmake
find_package(MusicPack CONFIG REQUIRED)
target_link_libraries(app PRIVATE MusicPack::Package)
```

```c
#include <musicpack/musicpack.h>

musicpack_package *pkg = musicpack_package_open_dir("Album.mpack", 0);
const musicpack_manifest *m = musicpack_package_manifest(pkg);
musicpack_report rep;
musicpack_package_verify(pkg, &rep, 0, 0);

/* Musepack handoff: expose a track's .mpc to the codec layer */
mpc_reader reader;
musicpack_package_track_open_reader(pkg, 0, 0, &reader);
musepack_decoder *d = musepack_decoder_open(&reader, 0);
```

The `musicpack` CLI (`musicpack info|verify|identify|create|import|update-metadata`,
plus the authoring draft commands `inspect`/`validate-draft`/`encode-draft`/
`build-draft`/`identify-draft`) builds, inspects and validates directory-form
packages. `info` shows collector identity (release type, edition, release/
original dates, country, label, catalogue number, medium, barcode);
`create`/`import` accept release options.
`import` reads embedded metadata (Vorbis Comments from FLAC, APEv2 from
`.mpc`) to fill album/track/release/identifier/source fields — explicit flags
override tags, and it never invents edition/country/label/catalogue/type from
filenames. `identify` matches a package against MusicBrainz (exact release ID
lookup, or barcode search via `curl`; `--mb-json` applies an offline release
document) and enriches empty fields with honest `identity.confidence`
(`exact`/`confirmed`/`probable`/`none`) — importing never requires a network.
`update-metadata` reconciles the manifest from the tracks' tags and, with
`--sync-tags`, writes the manifest back into `.mpc` APEv2 tags and refreshes
checksums (unknown top-level manifest fields preserved). The normative spec and
machine-readable schema live in `specs/musicpack-v1.md` and
`specs/musicpack-v1.schema.json`; committed reference packages (a Musepack
album and a FLAC album) are under `tests/reference/`.

The v1 contract is strictly enforced by the parser: a manifest must be a
single strict JSON document (duplicate keys and malformed nested objects are
rejected, numbers are parsed finitely and range-safely), every
manifest-referenced file requires a SHA-256, referenced paths must be unique
and stay contained within the package root, and unknown root-level fields are
documented to survive a round-trip. A committed conformance corpus (3 valid
manifests, 42 invalid manifests, 8 invalid asset cases) gates the contract
across platforms. See `docs/mpack-v1-contract-hardening.md` for the enforced
rules.

### Sonic analysis — content-based discovery

MusicPack sonic analysis stores a versioned audio embedding per track (and a
deterministic album embedding) in the package, so a server can later offer
content-based similarity and radio without a centralized recommendation
service. The **container format is frozen and model-independent**
(`specs/musicpack-sonic-v1.md`): `analysis/sonic.json`, referenced from the
manifest's optional `analysis[]`, with model-scoped **profiles**
(`musicpack-sonic-openl3-v1` = default permissive profile, not normative;
`musicpack-sonic-discogs-v1` = research-only quality reference;
`musicpack-sonic-clap-v1` = rejected). libmusicpack owns all Sonic parsing,
validation and profile-compatibility semantics (`musicpack/sonic.h`); the
`musicpack` CLI reports it in `info`/`verify` and attaches a completed
document in `build-draft`. The `musicpack-sonic` analyzer (in `sonic/`, ONNX
Runtime, single-threaded) computes embeddings at authoring time with a
SHA-256-pinned model; MusicPack Author exposes it as a model-independent
"Sonic Analysis" panel and downloads the ~18 MB pinned ONNX artifact on first
use (never a Python/ONNX-conversion step for end users). The standalone
`.app` bundles ONNX Runtime; the model lives in the app data directory. See
`author/README.md` for the acquisition, offline and packaging details.
Server-side recommendations are deliberately **not** implemented yet.

## musicpack-server — self-hosted library server

`musicpack-server` (in `server/`) indexes a real `.mpack` collection into a
SQLite collector library and serves it over a read-only HTTP API v1. It
consumes `libmusicpack` for all package parsing, validation and path
security; it never parses manifests itself.

```sh
musicpack-server scan    --library ~/Music [--database library.db] [--verify]
musicpack-server serve   --library ~/Music [--listen 127.0.0.1] [--port 8080]
musicpack-server verify  --library ~/Music
musicpack-server token create --name "Web" | token list | token revoke <id>
```

- **Scanner** — deterministic and idempotent; finds `.mpack` bundles (recursion
  stops at a package root), validates through libmusicpack, and ingests each
  package in its own transaction. A package's manifest sha256 gives cheap
  change detection; a content fingerprint keeps a *moved* package the same
  album. Removed packages become `unavailable`; malformed packages are
  recorded `invalid` without touching the index. Scanning is **fail-closed**:
  an incomplete traversal (enumeration, metadata or database-write failure)
  fails the scan and the unavailable sweep does not run, so prior library
  state is preserved. A lightweight scan records package verification state;
  `--verify` runs full SHA-256 verification.
- **Verified-only visibility** — a newly discovered package is not servable
  merely because its manifest parses. It becomes visible/servable only after
  full SHA-256 verification succeeds (`--verify` or the live verify endpoint);
  an unverified, checksum-failed or quarantined package is never served.
- **Package ownership** — releases can share logical identity (editions,
  mirror duplicates), but servable content is owned by exactly one package per
  release: one package owns the content graph, and a package claiming the same
  release identity cannot overwrite another package's tracks/assets. Streaming
  and API content enumeration resolve through the owning package only.
- **Conflict quarantine** — a package claiming an already-owned release
  identity with different content is quarantined as `conflict` rather than
  silently merged. Verification can never clear a conflict; quarantine ends
  only through explicit ownership re-arbitration (the owner becomes
  unavailable/invalid, or the conflicting content comes to match the owner).
- **Collector library** — SQLite (vendored amalgamation) preserving the
  release-group → release/edition → media → track hierarchy, with artists,
  assets, package status, and schema migrations.
- **Authentication** — `/api/v1/*` is protected by opaque bearer tokens
  (256-bit secrets; only their SHA-256 is stored), except `GET
  /api/v1/health`. Tokens are created/listed/revoked via the CLI. CORS is
  deny-by-default (`--allow-origin URL`, repeatable). The first-party web
  client exchanges a token once for an **HttpOnly session cookie**
  (`POST /api/v1/session`).
- **Live maintenance** — `POST /api/v1/library/scan` and `POST
  /api/v1/library/verify` run on single background workers (SQLite WAL), so
  playback and API reads continue and new state appears after each commit;
  `GET /api/v1/library/status` reports current/last scan + verify state.
- **HTTP API v1** — `GET /api/v1/health|albums|albums/{id}|releases/{id}|
  tracks/{id}|tracks/{id}/audio|artists|artists/{id}|assets/{id}|session`,
  with pagination, strict numeric ids, a JSON error envelope, and
  deterministic ordering. Editions are never collapsed: an album lists its
  distinct releases.
- **Direct streaming** — audio/assets are served as the **original stored
  bytes** (no decode/remux/rewrite) with full RFC 9110 single-range support
  (`206`/`416`, `Content-Range`, `Accept-Ranges`, `HEAD`), streamed from a
  file descriptor — never buffered. Only **verified** package content is
  servable, and every referenced object is type-checked (a regular file) and
  read through hardened opened-descriptor handling. Manifest hashes are
  validated during verification; a strong `ETag` (the manifest hash) enables
  `If-None-Match` → `304`. After verification, the package must remain in
  non-mutable, server-controlled storage for this trust guarantee to hold;
  post-verification mutation is detected on the next scan/verify. MIME and
  codec (`musepack-sv8`, …) are reported separately and derived server-side.
- **Package-object serving** — package-controlled web content is served
  defensively. Inline serving is restricted to verified raster image types
  (JPEG, PNG, GIF, WebP, BMP) and audio streams; HTML, SVG, JavaScript, XML,
  text and PDF-like assets are forced to `Content-Disposition: attachment`
  with a sanitized filename. Every package-object response carries
  `X-Content-Type-Options: nosniff` plus a sandboxing `Content-Security-Policy`
  on `200`/`206`/`304`/`416`.
- **Browser streaming** — `--static-dir DIR` serves static files with
  cross-origin isolation (COOP/COEP). Point it at the built web client
  (`web/app/dist`) to get the first-party record shelf at the server root; the
  client streams Musepack through a demand-driven reader (SharedArrayBuffer +
  Atomics + a network worker with a block cache), fetching only the compressed
  ranges the decoder needs, so playback starts before the file downloads and
  seeking never fetches the whole file.

### Security model for untrusted ingestion

MusicPack serves collections that may contain untrusted, externally placed
`.mpack` directories, and the server is hardened accordingly: verified-only
visibility, package-owned content, identity-conflict quarantine, regular-file
checks bound to the object actually read or hashed (POSIX opens with
`O_NONBLOCK|O_NOFOLLOW` and `fstat`s the opened descriptor), hard-link
rejection on POSIX, symlink/reparse-point containment, the attachment/`nosniff`/
sandbox-CSP serving policy above, fail-closed scanning and verification, and
resource budgets bounding object counts, aggregate referenced bytes, traversal
depth and manifest/Sonic parse sizes. A hostile-package regression suite
exercises special files, hard links, symlink escapes and oversized objects.

Documented operational restrictions remain: verified bytes are **not** copied
into immutable server-owned storage, so package directories must not be
concurrently mutable after verification; path containment is pathname-based
rather than descriptor-relative (`openat`-style); Windows cannot reliably
detect hard links (that rejection is POSIX-only); scans are not scoped to a
persisted library-root id (one database per root); and sanitizer testing is
local, not hosted. See `docs/server-untrusted-package-hardening.md` for the
exact limits and threat model.

Defaults are safe: loopback binding, no remote access implied. The API spec
is `specs/musicpack-api-v1.md`.

### Deployment

For a real end-to-end setup — MusicPack Author on macOS, `musicpack-server`
and the web client on a Linux server, browser clients — see
**`docs/deployment.md`** (build/install steps, filesystem layout, transfer
workflow, first-run procedure, systemd service, reverse proxy, upgrade,
backup and troubleshooting). The server also has its own concise README at
`server/README.md`, and a production systemd unit plus optional environment
template ship under `packaging/systemd/`.

---

## The Musepack codec (foundation)

Musepack is the audio foundation: a mature, open lossy codec whose SV7/SV8
streams the rest of the ecosystem plays and produces. The codec parts are
preserved and modernized conservatively — they are stable, well-tested, and
guarded by byte-identity regression tests.

### Building

CMake 3.16+ is required. Everything builds out of the box:

```sh
cmake -S . -B build
cmake --build build -j
```

Useful options:

| Option               | Default             | Description                                  |
|----------------------|---------------------|----------------------------------------------|
| `MPC_BUILD_SHARED`   | `ON` (non-Windows)  | Build `libmusepack` as a shared library      |
| `MPC_BUILD_TESTS`    | `OFF`               | Register the regression suites (ctest)       |
| `MPC_BUILD_MPCGAIN`  | `ON`                | Build `mpcgain` (needs libreplaygain)        |
| `MPC_BUILD_MPCCHAP`  | `ON`                | Build `mpcchap` (needs libcuefile)           |
| `MPC_BUILD_SERVER`   | `ON`                | Build `musicpack-server` (needs libmicrohttpd)|

`mpcgain` and `mpcchap` are skipped automatically if their optional
dependencies (`libreplaygain`, `libcuefile`) are not found.
`musicpack-server` is skipped when `libmicrohttpd` is unavailable (Windows is
not supported for the server executable; the server core still builds there).

Install (headers, libraries, pkg-config files, and tools):

```sh
cmake --install build
```

Installed consumers can use the packaged CMake targets:

```cmake
find_package(Musepack CONFIG REQUIRED)   # the codec library
target_link_libraries(app PRIVATE Musepack::Decoder)
```

### libmusepack decoder API

`libmusepack` is the stable decoder-facing library (SV7/SV8). It exposes an
opaque, single-threaded session API over a pluggable reader abstraction
(files, memory buffers, or custom callbacks):

```c
#include <musepack/musepack.h>

mpc_reader reader;
mpc_reader_init_stdio(&reader, "song.mpc");
musepack_decoder *d = musepack_decoder_open(&reader, 0);
float pcm[MUSEPACK_FRAME_MAX * 2];
uint64_t n;
while (musepack_decoder_read(d, pcm, MUSEPACK_FRAME_MAX, &n) == MUSEPACK_OK) {
    /* play n sample-frames of interleaved float PCM */
}
musepack_decoder_close(d);
mpc_reader_exit_stdio(&reader);
```

See `docs/api.md` for the full API, ownership rules, error codes, and the
reader design. The decoder also builds to WebAssembly (Emscripten) and powers
the two browser front-ends above:

```sh
emcmake cmake -S . -B build-wasm && cmake --build build-wasm
```

- The **MusicPack web client** (`web/`) decodes `.mpc` over HTTP Range with a
  demand-driven reader; see `web/README.md`.
- The low-level WASM demo (`demo/`) is a development/test artifact
  kept for the demand-reader plumbing.

## Tools

| Tool       | Purpose                                                        |
|------------|----------------------------------------------------------------|
| `mpcdec`   | Decode MPC to WAV; `-i` prints stream info, `-c` checks a file |
| `mpcenc`   | Encode WAV (or other formats via external decoders) to MPC     |
| `mpc2sv8`  | Convert SV7 files to SV8                                       |
| `mpccut`   | Cut a range of samples out of an SV8 file                      |
| `mpcgain`  | Compute ReplayGain (requires libreplaygain)                    |
| `mpcchap`  | Read/write SV8 chapters (requires libcuefile)                  |
| `wavcmp`   | Compare two WAV files                                          |
| `musicpack` | Author and verify `.mpack` albums (`info`, `verify`, `create`, `import`, `identify`, `update-metadata`, plus the authoring draft commands `inspect`/`validate-draft`/`encode-draft`/`build-draft`/`identify-draft`) |
| `musicpack-server` | Index a `.mpack` library and serve it over HTTP API v1 (requires libmicrohttpd) |

## Testing

See `tests/README.md`. With `-DMPC_BUILD_TESTS=ON`, `ctest` runs the full suite,
organized by category rather than a single count:

- **unit / API / codec** — crc32, bitstream and table internals; the stable
  `libmusepack` API surface; a fixture regression pinning decode output for
  bit-exactness; end-to-end toolchain integration; and decoder robustness on
  malformed input;
- **decoder SIMD A/B** — scalar-vs-SIMD synthesis differential and direct
  decoder-state differential (`synth_ab`, `synth_state_ab`), each fail-closed
  when the SIMD kernel is unavailable;
- **encoder / psy A/B** — analysis-filter and psychoacoustic scalar-vs-SIMD
  differentials (`enc_ab`, `psy_ab`), also fail-closed;
- **`.mpack` package** — manifest, paths, checksums, loudness meter, handoff,
  integration and fuzz-lite;
- **v1 conformance corpus** — 3 valid manifests, 42 invalid manifests, 8
  invalid asset cases, required to pass `info`/`verify` per case;
- **hostile-package regression** — special files (FIFO/directory/symlink
  escape), hard links and oversized objects must fail quickly;
- **Sonic** — container parsing/validation units and WAV robustness;
- **Author** — backend draft-command tests and the FLAC→MPC q6 encode stage
  (skipped when ffmpeg is unavailable);
- **server** — a core suite (range parser, migrations, tokens, package
  identity, ownership/conflict, verify, scanner behaviors) that runs on all
  platforms, plus an HTTP integration suite (auth matrix, CORS, live
  scan/verify/status, streaming, HTTP Range, ETag/304, concurrency, security
  headers — UNIX);
- **compatibility** — a check that pins the encoder to bit-identical output
  with the pristine reference encoder, and `enc_compat` for the extended
  q5/q6/q7 corpus (live against a same-toolchain reference build on CI, with a
  committed manifest as the local fallback);
- **Wasm / web** — an Emscripten `wasm_smoke` suite (demand-driven
  range-reader path: PCM identity, seek fetch-accounting, network-failure
  injection), a `web_wasm_gapless` Node suite (gapless two-track continuity,
  exact track-end frames, demand-reader seek accounting), and the web client's
  own Vitest/Playwright suites.

Hosted CI covers Linux GCC, Linux Clang, macOS ARM64, Windows MSVC, a Linux
SIMD-off build, Emscripten/Wasm, and the web client (Playwright). Sanitizer
coverage (ASan/UBSan) is a targeted local exercise of the package, server,
integration and fuzz suites — not a hosted job.

The web client adds its own suites (`web/`): Vitest units for the controller,
queue, ring buffer, loudness math, API client and session; a Playwright
browser suite (authenticate → shelf → album → edition switch → Musepack and
native playback → seek → gapless → queue → Media Session → 401 re-auth) run
in the CI `web-client` job; and a performance report script. See
`web/README.md`.

```sh
cmake -S . -B build -DMPC_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

## Layout

- `libmpcdec/` — SV7/SV8 decoder + `musepack_decoder` facade (the codec)
- `libmpcenc/` — SV8 encoder core
- `libmpcpsy/` — psychoacoustic model
- `libwavformat/` — WAV read/write helper
- `include/musepack/` — stable `libmusepack` public API
- `include/mpc/` — historical public headers (installed for compatibility)
- `libmusicpack/` — `.mpack` package library (`libmusicpack`)
- `musicpack/` — `musicpack` CLI (info/verify/create/import/update-metadata + the authoring draft commands `inspect`/`validate-draft`/`encode-draft`/`build-draft`/`identify-draft`)
- `author/` — **MusicPack Author**: the Tauri 2 + Svelte 5 desktop authoring GUI (`author/README.md`)
- `server/` — `musicpack-server`: scanner, SQLite collector library, HTTP API v1, direct streaming (vendored SQLite in `server/vendor/`)
- `web/` — the web client (Svelte 5 + Vite + TS): the digital record shelf
- `packaging/` — systemd service unit + optional environment template (see `docs/deployment.md`)
- `wasm/` — Emscripten build of the decoder + WASM wrapper + smoke test
- `demo/` — low-level browser playback proof-of-concept
- `specs/` — `.mpack` v1 spec + JSON Schema, and the server API spec (`musicpack-api-v1.md`)
- `common/` — shared sources (crc32, fast-math tables, tag handling)
- `tests/` — fixture generator, corpus generator, and regression harnesses
- `legacy/` — retired autotools and Visual Studio 2005 build files

## License

The codec layer: `libmpcdec` is BSD-licensed; `libmpcenc`, `libmpcpsy`, and the
codec tools are LGPL-licensed. The MusicPack layer (`libmusicpack`, the
`musicpack` CLI, and `musicpack-server`) is BSD 3-clause licensed. See
`LICENSE` for details.
