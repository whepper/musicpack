# musicpack-server

`musicpack-server` is the self-hosted MusicPack library server. It indexes a
collection of `.mpack` album packages into a SQLite collector library and
serves the collection over a read-only HTTP API v1 with **direct streaming**
(the original stored bytes, never transcoded).

It consumes `libmusicpack` for all package parsing, validation and path
security; it never parses manifests itself.

```text
library/ (verified .mpack packages) ── scan/verify ──> library.db (SQLite)
                                                           │
serve  ──> HTTP API v1 + static web client  <── browsers
```

## Build prerequisites

- CMake ≥ 3.16, a C11 compiler.
- **GNU libmicrohttpd** (development package, e.g. `libmicrohttpd-dev` on
  Ubuntu/Debian). Without it the CMake build succeeds but produces only the
  platform-independent server core — **no `musicpack-server` executable**.
- Windows builds the server core but not the HTTP executable (libmicrohttpd
  is unavailable there).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMPC_BUILD_SERVER=ON
cmake --build build -j --target musicpack_server_cmd
sudo cmake --install build          # installs musicpack-server to <prefix>/bin
```

## CLI overview

```sh
musicpack-server scan    --library DIR [--database PATH] [--verify]
musicpack-server verify  --library DIR [--database PATH]
musicpack-server serve   --library DIR [--database PATH]
                         [--listen IP] [--port N] [--no-scan]
                         [--static-dir DIR] [--allow-origin URL]...
musicpack-server token create --name NAME | token list | token revoke <id>
musicpack-server help | version
```

Defaults: library `./library`, database `./library.db`, bind `127.0.0.1`,
port `8080`. Command-line flags override the `MUSICPACK_LIBRARY` /
`MUSICPACK_DATABASE` / `MUSICPACK_LISTEN` / `MUSICPACK_PORT` environment
variables, which override defaults.

### scan / verify / serve

- **`scan`** indexes the library deterministically and idempotently. A
  lightweight scan leaves packages `unverified`.
- **`verify`** is a scan with full SHA-256 integrity verification — the gate
  for **servability**. Only fully verified packages are visible/streamable;
  `scan --verify` is equivalent.
- **`serve`** runs a startup scan (unless `--no-scan`) then serves the HTTP
  API. Loopback binding is the safe default; expose it via a reverse proxy.

Verified tracks with a waveform envelope expose its metadata in track JSON and
the immutable binary payload at `GET /api/v1/tracks/{trackId}/waveform`.
The server validates and indexes the package-provided envelope; it never
regenerates waveform data.

### Token management

`/api/v1/*` is protected by opaque bearer tokens (256-bit secrets; only their
SHA-256 is stored), except `GET /api/v1/health`:

```sh
musicpack-server token create --name Web --database /srv/musicpack/data/library.db
musicpack-server token list --database /srv/musicpack/data/library.db
musicpack-server token revoke 3 --database /srv/musicpack/data/library.db
```

The printed `mpk_...` secret is shown once. The first-party web client
exchanges it once for an **HttpOnly session cookie** (`POST /api/v1/session`).

### Static web serving

`--static-dir DIR` serves the built web client (e.g. `web/app/dist`) at the
server root with cross-origin isolation headers
(`Cross-Origin-Opener-Policy: same-origin`,
`Cross-Origin-Embedder-Policy: require-corp`) required by the
SharedArrayBuffer-based WASM demand reader, plus SPA fallback to `index.html`.
Paths are containment-checked; `/api/` is never routed here.

### Authentication overview

- Bearer tokens for CLI/native clients; session cookies for the browser.
- Cookie: `HttpOnly; SameSite=Strict; Path=/; Max-Age=2592000` (30 days,
  sliding). `Secure` is added over HTTPS (`X-Forwarded-Proto: https`) or with
  `--secure-cookies`.
- CORS is deny-by-default; grant origins explicitly with `--allow-origin`
  (repeatable). Same-origin clients (the default setup) need none.

## Library / database layout

Packages live on disk as `.mpack` directories; `library.db` is the mutable
index (WAL mode; `-wal`/`-shm` sit beside it). One database is expected per
library root. A recommended production layout, first-run commands, the
trusted transfer pattern, systemd service, backup and troubleshooting live in
**`docs/deployment.md`**.

## Reference documents

- `docs/deployment.md` — the end-to-end installation/run guide.
- `specs/musicpack-api-v1.md` — the HTTP API v1 contract.
- `docs/server-untrusted-package-hardening.md` — the untrusted-ingestion trust
  model, fail-closed scanning, quarantine rules and operational restrictions.
