# MusicPack deployment

This is the primary end-to-end guide for running a real MusicPack setup:
**MusicPack Author on macOS**, a **Linux server** running `musicpack-server`
plus the first-party web client, and **browser/phone/tablet** clients.

```text
macOS workstation
  MusicPack Author.app
        |
        |  verified .mpack albums (copied/synced to the Linux server)
        v
Linux server
  /srv/musicpack/library      .mpack album packages (read-only after verify)
  /srv/musicpack/data         SQLite library.db (mutable server state)
  /srv/musicpack/web/dist     static web client (built once)
        |
        v
browser / phone / tablet clients
```

This milestone establishes a practical baseline: current components installed
and run from a clean checkout, without a package manager, containers, or
cloud infrastructure. It changes no `.mpack` semantics, no Musepack codec
behavior, and no server architecture.

For advanced/developer details, see `author/README.md`, `web/README.md`, and
`server/README.md`. For the server's threat model, see
`docs/server-untrusted-package-hardening.md`. The HTTP API contract is
`specs/musicpack-api-v1.md`.

---

## 1. Architecture

- **macOS workstation** — MusicPack Author is a standalone `.app` that authors
  `.mpack` albums (tagged FLAC/WAV or an existing Musepack album → curated,
  validated `.mpack`). It bundles the `musicpack` backend and the `mpcenc`
  encoder; FLAC/WAV decoding is native (vendored dr_flac + a small WAV reader),
  so there is no external decoder.
- **Linux server** — `musicpack-server` scans and verifies an `.mpack`
  collection into a SQLite collector library and serves it over a read-only
  HTTP API v1. It also serves the static web client from `--static-dir`.
- **Browsers** — the web client authenticates with a token once (exchanged
  for an HttpOnly session cookie), then browses and plays the collection.
  Musepack audio is decoded in the browser by the WASM decoder via the
  demand-driven range reader; FLAC uses the browser's native stack.

The server runs single-process, foreground (perfect for systemd
`Type=simple`). It binds loopback by default; a reverse proxy provides HTTPS
for remote/LAN clients.

---

## 2. macOS: build and run MusicPack Author

### Prerequisites

- macOS with **Xcode Command Line Tools** (`xcode-select --install`)
- **CMake ≥ 3.16**
- **Node.js ≥ 20** and npm
- **Rust toolchain** (Tauri 2 backend)
- **curl** (system `/usr/bin/curl`, used for MusicBrainz lookups)

No external multimedia tool is required. FLAC/WAV decoding for encoding and
loudness is native (see "No external decoder requirement" below), so there is
nothing extra to install.

### Build the standalone `.app`

From a clone of the repository root:

```sh
./scripts/build-author-macos.sh
```

This is the exact supported build command. The script (1) builds fully static
`musicpack` and `mpcenc` binaries plus the `musicpack-sonic` analyzer in
`build-author/`, (2) verifies they reference only macOS system libraries,
(3) stages them as Tauri sidecars, (4) runs the Svelte frontend build and
`npm run tauri build`, and (5) runs `scripts/smoke-author-macos.sh` against
the finished bundle. A missing piece, external dependency, or mixed
architecture fails the build (see `scripts/audit-author-macos.sh`).

The generated application bundle is:

```text
author/src-tauri/target/release/bundle/macos/MusicPack Author.app
```

### Install

Copy the `.app` anywhere, including `/Applications`:

```sh
cp -R "author/src-tauri/target/release/bundle/macos/MusicPack Author.app" /Applications/
```

Launch it and author as described in `author/README.md`. The `.app` is
self-contained: it does not need CMake, Node, Rust, the source repository,
`MUSICPACK_CLI`, FFmpeg, Homebrew or MacPorts. It bundles:

- `musicpack` (static backend: inspect/validate-draft/encode-draft/
  build-draft/identify-draft/verify)
- `mpcenc` (Musepack encoder; **q6 is the default encoding quality**)
- `musicpack-sonic` (optional analysis) and a relocatable ONNX Runtime dylib

**Source files are never modified.** Package finalization is transactional and
fully verified before it is reported successful.

### No external decoder requirement

FLAC/WAV sources are decoded **natively** by the bundled backend through
libmusicpack (vendored dr_flac for FLAC, a small native RIFF reader for WAV).
There is no FFmpeg, no `MUSICPACK_FFMPEG`, no Homebrew/MacPorts lookup, and no
PATH probing anywhere in the packaged app. The distribution audit rejects any
bundled `ffmpeg*` file and any external dylib. Authoring a FLAC or WAV album
works with no additional software installed.

### Optional Sonic analysis

MusicPack Author can compute content-based audio embeddings ("Sonic
Analysis") per track. It is never run automatically. On first use the app
downloads a pinned, SHA-256-verified ~18 MB ONNX model into its app data
directory (never into the package). Offline with a cached model, analysis
works normally; offline without one, the package can still be built.

### Where app data lives

Model cache and analyzer working files are stored under the platform-native
app data directory:

```text
~/Library/Application Support/MusicPack Author/sonic/models/...
```

See `author/README.md` for the full workflow, backend resolution, and
limitations. Note: the standalone `.app` is built for the host architecture
only (arm64 or x86_64, not universal), is ad-hoc signed for local runs, and
has **no notarization** — do not distribute it as-is.

---

## 3. Linux: build and install the server

Ubuntu/Debian is the primary documented platform (it is what CI runs on).
Package names vary by distro; adjust accordingly.

### Dependencies

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config libmicrohttpd-dev
```

- `libmicrohttpd-dev` is required for the `musicpack-server` executable. If it
  is absent the build succeeds but only the platform-independent server core
  is produced (with a CMake warning) — **no `musicpack-server` binary**.
- `mpcgain`/`mpcchap` are skipped automatically when their optional
  dependencies are missing; they are unrelated to serving.

### Configure, build, test, install

From a fresh clone:

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DMPC_BUILD_SERVER=ON

cmake --build build -j

# optional: run the server core + HTTP integration suites
cmake -S . -B build-test -DCMAKE_BUILD_TYPE=Release -DMPC_BUILD_TESTS=ON
cmake --build build-test -j
ctest --test-dir build-test -R 'server_unit|server_integration'

sudo cmake --install build
```

`MPC_BUILD_SERVER` defaults to ON; the flag is shown for clarity. A standard
Release build is recommended for production.

### What gets installed and where

`sudo cmake --install build` installs headers, libraries, pkg-config files,
CMake package configs, and all tools into the CMake install prefix
(`/usr/local` by default):

```text
/usr/local/bin/musicpack-server     the HTTP server (this milestone's target)
/usr/local/bin/musicpack            .mpack authoring/verification CLI
/usr/local/bin/mpcenc               Musepack encoder
/usr/local/bin/mpcdec, mpc2sv8, mpccut, wavcmp, musicpack-sonic
/usr/local/lib/...                  libmusepack, libmusicpack, pkg-config
/usr/local/include/...              public headers
```

Set `-DCMAKE_INSTALL_PREFIX=/usr/local` explicitly if you want the default
stated, or choose another prefix. Confirm with:

```sh
musicpack-server version
```

---

## 4. Build the web client

The client is a static Svelte 5 + Vite + TypeScript build served verbatim by
`musicpack-server --static-dir`. There is **no Node runtime required once the
static build exists**.

### Prerequisites

- **Node.js ≥ 22** and npm (22 is what CI tests against; the Node-side wasm
  gapless harness needs 22.6+). Vite 6 requires a modern Node.
- **Emscripten** to build the WASM decoder module:

```sh
EMSDK_VERSION=6.0.6
git clone --depth 1 https://github.com/emscripten-core/emsdk.git /opt/emsdk
/opt/emsdk/emsdk install "$EMSDK_VERSION"
/opt/emsdk/emsdk activate "$EMSDK_VERSION"
source /opt/emsdk/emsdk_env.sh        # per shell, or add to your profile
```

### Build the WASM decoder

```sh
emcmake cmake -S . -B build-wasm
cmake --build build-wasm --target musepack_wasm -j
```

This produces `build-wasm/wasm/musepack.{js,wasm}`.

### Build the static client

```sh
cd web
npm ci
npm run build
```

`npm run build` first runs `web/scripts/sync-wasm.sh`, which copies the
built WASM module and the demand-reader scripts into `web/app/public`, then
builds.
The result is:

```text
web/app/dist/
```

Serve that directory with `musicpack-server --static-dir` (next section).

---

## 5. Recommended filesystem layout

```text
/srv/musicpack/
├── library/                  .mpack album directories (package data)
│   ├── Album A.mpack/
│   └── Album B.mpack/
├── data/                     mutable server state
│   └── library.db            SQLite collector library (+ -wal/-shm)
├── incoming/                 staging area for transfers (separate)
└── web/
    └── dist/                 built web client (static, read-only)
```

The three layers are kept separate deliberately:

| Path | Purpose | Ownership |
|------|---------|-----------|
| `library/` | immutable package storage | root; read-only for the service user after verification |
| `data/` | SQLite database + WAL files | the service user (writable) |
| `web/dist/` | static client files | root; read-only for the service user |
| `incoming/` | transfer staging | the operator/transfer user |

Never place the writable database inside a `.mpack` package directory, and
never inside a package. Database state is not part of an album.

---

## 6. Transferring packages from macOS to the server

The simplest supported flow is tool-agnostic (`scp`, `rsync`, SFTP apps):

```sh
# land the transfer in the staging area first
scp -r "Album A.mpack" user@server:/srv/musicpack/incoming/

# then move it into the library with an atomic rename (same filesystem)
sudo mv /srv/musicpack/incoming/"Album A.mpack" /srv/musicpack/library/
```

The final `mv` runs as the operator (root), which works whether `library/` is
owned by root or by the service user — and keeps the library non-writable by
the service during normal serving.

Rules:

- **Stage before install.** Partial transfers must never expose an incomplete
  `.mpack`. Because `incoming/` is never scanned, a half-transferred package
  cannot be picked up by the scanner.
- **Atomic rename when possible.** `mv` within one filesystem (here `/srv`)
  is atomic; put both `incoming/` and `library/` on the same filesystem to
  keep the final step atomic. If they must span filesystems, use a
  `rsync --remove-source-files` from staging and accept the non-atomic
  window, or copy then verify.
- **Never modify a finalized package.** After Author finalizes and verifies an
  `.mpack`, the server treats it as immutable. Do not add, remove, or rewrite
  files inside a package after verification — the server re-checks on the
  next scan/verify and may mark the package unverified.

A convenient one-liner for ongoing syncs (rsync preserves metadata and
contents):

```sh
rsync -a --info=progress2 "Artist - Album.mpack" \
  user@server:/srv/musicpack/incoming/
```

---

## 7. Security and storage model

The server is hardened for libraries that may contain untrusted, externally
placed `.mpack` directories. The operational model you must maintain:

- **Verification gates visibility.** A newly discovered package is not
  servable until full SHA-256 verification succeeds. A lightweight `scan`
  leaves packages `unverified`; only `verify` (or `scan --verify`) makes them
  servable. A checksum-failed or quarantined package is never served.
- **Conflicts are quarantined.** A package claiming an already-owned release
  identity with different content is quarantined as `conflict` and can only
  leave that state through explicit ownership re-arbitration — never by
  verification.
- **Packages must become effectively non-mutable after verification.** The
  server serves the verified bytes by pathname and does not re-hash at serve
  time. In-place mutation after verification is only detected on the next
  scan/verify. Put packages in **server-controlled storage** and make them
  read-only for the service user once verified.
- **Do not modify package directories while they are being served.** Use the
  `incoming/` staging pattern instead of in-place edits.
- **Containment is pathname-based**, not descriptor-rooted (`openat`-style).
  Untrusted package trees must not be writable by an attacker during
  scan/serve.
- **Hard-link behavior differs by platform.** POSIX rejects referenced files
  with a link count > 1; Windows cannot reliably detect hard links and relies
  on reparse-point/attribute checks plus the operational restriction above.

The full trust model, fail-closed scanning, resource budgets, and the exact
limits are in `docs/server-untrusted-package-hardening.md`. The claimed status
is **"safe with documented operational restrictions"**, not
"safe for untrusted packages" unconditionally.

---

## 8. First-run procedure

Create the layout and service user (see also the systemd section below):

```sh
sudo mkdir -p /srv/musicpack/library /srv/musicpack/data \
             /srv/musicpack/incoming /srv/musicpack/web
sudo useradd --system --home /srv/musicpack --shell /usr/sbin/nologin musicpack
sudo chown -R musicpack:musicpack /srv/musicpack/data
sudo chown -R musicpack:musicpack /srv/musicpack/library   # owner keeps r/w for now
sudo chown -R musicpack:musicpack /srv/musicpack/incoming
```

### Scan

```sh
musicpack-server scan \
  --library /srv/musicpack/library \
  --database /srv/musicpack/data/library.db
```

A scan indexes the library (idempotent, deterministic) and records package
state. It does **not** hash package contents, so freshly scanned packages are
`unverified` and not servable.

### Verify

```sh
musicpack-server verify \
  --library /srv/musicpack/library \
  --database /srv/musicpack/data/library.db
```

`verify` is a scan with full SHA-256 integrity verification and is the gate
for servability: only fully verified packages become visible/streamable.
**Run verify after every package addition** (or pass `--verify` to `scan`,
which is the same thing). The intended relationship: `scan` finds and indexes,
`verify` makes the collection servable.

### Create the first web token

```sh
musicpack-server token create --name Web \
  --database /srv/musicpack/data/library.db
```

The command prints a one-time opaque token (`mpk_...`) and its id. The
browser exchanges this **once** for an HttpOnly session cookie
(`POST /api/v1/session`); the token is never stored in the browser. Treat the
printed secret like a password: it is shown only at creation, and only its
SHA-256 is stored server-side. List/revoke with `musicpack-server token list`
/ `musicpack-server token revoke <id> --database ...`.

---

## 9. Production server invocation

```sh
musicpack-server serve \
  --library /srv/musicpack/library \
  --database /srv/musicpack/data/library.db \
  --static-dir /srv/musicpack/web/dist \
  --listen 127.0.0.1 \
  --port 8080
```

All flags match the current CLI (`musicpack-server help`). Behavior:

- A **startup scan** runs unless `--no-scan` is given; it does not need a
  pre-existing database. With `--no-scan`, an existing database is required
  and prior library state is used as-is.
- **Loopback is the recommended default.** Bind `127.0.0.1` and put a reverse
  proxy in front for HTTPS and external access. This keeps the server
  unreachable from the network when misconfigured, follows the "no remote
  access implied" default, and lets the proxy handle TLS, headers, and body
  limits. Bind `0.0.0.0` only if you have a firewall and accept plain HTTP.
- The database opens in WAL mode; `-wal`/`-shm` files live beside `library.db`
  (keep the `data/` directory writable by the service user).
- Logs go to **stderr** (captured by the journal under systemd). Set
  `MUSICPACK_LOG=debug|warn|error` to change the level.
- Live maintenance is available over the API without restarting:
  `POST /api/v1/library/scan` and `POST /api/v1/library/verify` run on a
  single background worker each.

`--allow-origin` is **not needed** when the web client is served by the same
server (same origin). It is only for CORS-granting a client served elsewhere
(e.g. the Vite dev server).

---

## 10. Browser/client access

- Open the server URL (e.g. `https://server.example.com/`).
- Sign in once with the generated token. It is exchanged for the HttpOnly
  `musicpack_session` cookie (30 days, sliding, `SameSite=Strict`); the token
  is **not stored in local storage**.
- The web client is the first-party record shelf: artwork-first album shelf,
  album/edition pages, search, and playback. `.mpc` streams decode in-browser
  via the WASM demand-driven reader (SharedArrayBuffer); FLAC uses the native
  browser stack. BS.1770 album normalization and gapless playback are client
  behavior.
- **HTTPS is recommended for any remote/LAN deployment.** Over HTTPS the
  server automatically emits `Secure` session cookies (it trusts
  `X-Forwarded-Proto: https` from the reverse proxy; `--secure-cookies`
  forces it).
- There is no native mobile app; phones and tablets use the same web client.

---

## 11. Reverse proxy (optional, for HTTPS)

The web client and API must be proxied to `127.0.0.1:8080`. Requirements:

- preserve **Range** requests so audio streaming stays byte-exact (206);
- forward `X-Forwarded-Proto: https` so the server emits `Secure` cookies;
- **do not strip the COOP/COEP headers** the server emits on static files
  (`Cross-Origin-Opener-Policy: same-origin` +
  `Cross-Origin-Embedder-Policy: require-corp`) — the WASM demand reader needs
  SharedArrayBuffer, which requires cross-origin isolation;
- a sane upload/body limit (the only POST bodies are the ~4 KB session
  exchange; a 1 MB limit is plenty);
- terminate HTTPS.

No external authentication is introduced here; the MusicPack token/session
layer is the authentication.

### Caddy

```caddyfile
server.example.com {
    request_body {
        max_size 1MB
    }
    reverse_proxy 127.0.0.1:8080
}
```

Caddy sets `X-Forwarded-Proto`, passes `Range` and the upstream COOP/COEP
headers through, and handles TLS automatically (plus OCSP, HTTP/2/3). If the
page must also be reachable on the server's LAN IP over plain HTTP, add
another site block without TLS.

### nginx

```nginx
server {
    listen 443 ssl;
    server_name server.example.com;

    ssl_certificate     /etc/letsencrypt/live/server.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/server.example.com/privkey.pem;

    client_max_body_size 1m;

    location / {
        proxy_pass http://127.0.0.1:8080;
        proxy_http_version 1.1;

        # Range / streaming: pass Range through and stream instead of buffering
        proxy_set_header Range $http_range;
        proxy_buffering off;

        # needed so the server emits Secure session cookies
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_set_header Host $host;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;

        # COOP/COEP come from the upstream server and pass through untouched.
        # Do NOT add an `add_header` block here: nginx `add_header` (with
        # inheritance) can suppress the upstream COOP/COEP headers. If you must
        # add headers, re-declare them explicitly with `always`:
        # add_header Cross-Origin-Opener-Policy same-origin always;
        # add_header Cross-Origin-Embedder-Policy require-corp always;
    }
}

# optional: HTTP -> HTTPS
server {
    listen 80;
    server_name server.example.com;
    return 301 https://$host$request_uri;
}
```

---

## 12. systemd service

A production-grade unit ships at `packaging/systemd/musicpack-server.service`
(plus an optional environment template `musicpack-server.env.example`). It
runs as the dedicated unprivileged `musicpack` user, binds loopback, uses
explicit paths, restarts on failure, and hardens the process.

### Install

```sh
sudo useradd --system --home /srv/musicpack --shell /usr/sbin/nologin musicpack
sudo mkdir -p /srv/musicpack/library /srv/musicpack/data \
             /srv/musicpack/incoming /srv/musicpack/web
sudo chown -R musicpack:musicpack /srv/musicpack/data /srv/musicpack/incoming

sudo cp packaging/systemd/musicpack-server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now musicpack-server
```

The unit's `ExecStart` uses explicit paths:
`--library /srv/musicpack/library --database /srv/musicpack/data/library.db
--static-dir /srv/musicpack/web/dist --listen 127.0.0.1 --port 8080`.

### Ownership and permissions

- `library/` — owned by root or `musicpack`, but the service user only needs
  **read**; after verification make it non-mutable
  (`chown -R musicpack:musicpack library` for the initial r/w scan, then
  drop write via `chmod -R a-w` on the package directories, or rely on
  `ProtectSystem=strict` which already makes everything read-only for the
  service).
- `data/` — owned by `musicpack`, writable (SQLite WAL needs it).
- `web/dist/` — read-only for the service user.
- `incoming/` — writable by the operator/transfer account, **not** scanned.

**Why the library should be non-mutable after verification:** the server
serves verified bytes without re-hashing at serve time. Mutation is detected
only on the next scan/verify. Making packages read-only after verification is
what converts the server's documented trust restriction into an enforceable
deployment invariant.

### Hardening

The unit includes `NoNewPrivileges=true`, `PrivateTmp=true`,
`ProtectSystem=strict`, `ProtectHome=true`, explicit `ReadOnlyPaths`
(library, web) and `ReadWritePaths` (data for the SQLite WAL), 
`RestrictAddressFamilies=AF_INET AF_UNIX`, and `LockPersonality=true`.
These do not block package reads, SQLite writes, or loopback networking.

---

## 13. Upgrade procedure

```text
1. build/install the new MusicPack binaries   (sections 3–4)
2. build the new web client                  (section 4)
3. stop the service:   sudo systemctl stop musicpack-server
4. back up the database                      (section 14)
5. replace binaries/static files
6. start the service:  sudo systemctl start musicpack-server
7. scan/verify if required
```

- **Database migrations are automatic.** `musicpack-server` applies schema
  migrations transactionally when it opens the database, then runs the
  startup scan. No manual migration step exists.
- **Rollback limitation:** migrations are forward-only. If you must roll back
  to an older server after it has migrated the database, restore the
  pre-upgrade backup (step 4) — an older binary does not downgrade a newer
  schema.
- Package data (`library/`) is untouched by upgrades; a re-`verify` after
  upgrade is cheap insurance but not usually required.

---

## 14. Backup and restore

**Primary collection data** — the `.mpack` directories under `library/`.
Backing these up preserves the collection itself; they are the source of
truth and never rewritten.

**Derived/index state** — `library.db` under `data/`. It is a rebuildable
index: deleting it and running `scan` + `verify` rebuilds the whole library
from `library/`. **But tokens (and any derived session state) live in the
database**, so a rebuilt database loses all API tokens — you would need to
re-create and re-distribute a token. Back up the database to keep tokens.

**Safe SQLite backup.** The database runs in WAL mode; do **not** copy
`library.db` alone while the server is running (you may capture an
inconsistent snapshot or miss the WAL). Either:

```sh
# while the server is running (SQLite online backup):
sqlite3 /srv/musicpack/data/library.db ".backup /srv/musicpack/backup/library.db"

# or with the service stopped (consistent snapshot; copy WAL siblings too):
sudo systemctl stop musicpack-server
sudo rsync -a /srv/musicpack/data/ /srv/musicpack/backup/
sudo systemctl start musicpack-server
```

Restore: place the backup database (and `-wal`/`-shm` if present) back into
`data/`, ensure ownership is `musicpack:musicpack`, and start the service.
The server re-verifies packages that changed while the database was
out-of-date.

---

## 15. Troubleshooting

| Symptom | Cause / fix |
|---------|-------------|
| CMake warns `libmicrohttpd not found` | Install `libmicrohttpd-dev` (Ubuntu/Debian) or the equivalent distro package, then reconfigure. The `musicpack-server` executable is not built without it. |
| Author encode stage reports a decode/source failure | FLAC/WAV decoding is native; a failure means the source is unsupported or malformed (e.g. a non-PCM WAV variant, >8 channels, or a corrupted file). Author only supports FLAC and PCM WAV authoring sources. |
| Package stays `unverified`, album not visible in the web client | A lightweight `scan` does not verify. Run `musicpack-server verify` (or `scan --verify`). Only fully verified packages are servable. |
| Package shows as `conflict` | It claims a release identity already owned by another package with different content. Verification cannot clear it; re-arbitrate ownership (make the conflicting content match, or remove/invalidate the owner) or remove the duplicate. |
| Package `invalid` / checksum mismatch | The manifest failed to parse or referenced bytes do not match their SHA-256. Fix or re-author the package on the workstation and re-transfer. |
| Permission denied reading `.mpack` | The service user cannot read `library/`. Check ownership/`chmod`; ensure parent directories are traversable (`o+x`) by `musicpack`. |
| SQLite database not writable | `data/` must be owned/writable by the service user (WAL needs `-wal`/`-shm` there). `chown musicpack:musicpack data` and check the systemd `ReadWritePaths`. |
| Port already in use | Another process holds the port. Change `--port`, or find it with `ss -ltnp`. |
| Web client loads but Musepack playback fails | The WASM module is missing or stale. Rebuild (`build-wasm`) and re-run `npm run build` so `sync-wasm.sh` copies `musepack.{js,wasm}` into `web/app/public`; restart the server with the new `--static-dir`. |
| SharedArrayBuffer / cross-origin isolation failure | The page is not cross-origin isolated. Check the served response headers carry `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp` (the server emits them; a reverse proxy may be stripping them). Do not serve the client from a different origin. |
| Stale web build | `web/app/dist` and `web/app/public` are gitignored; `npm run build` rebuilds them. Browsers cache with `Cache-Control: no-cache` from the server, but force-refresh or rebuild after upgrades. |
| Server reachable only locally | Expected: it binds `127.0.0.1` by default. Expose it through a reverse proxy (section 11), not by binding `0.0.0.0` without a firewall. |
| `serve --no-scan` fails "database does not exist" | Run `scan`/`verify` once first, or drop `--no-scan`. |

---

## 16. Optional configuration strategy

The server reads environment variables (`MUSICPACK_LIBRARY`, `MUSICPACK_DATABASE`,
`MUSICPACK_LISTEN`, `MUSICPACK_PORT`, `MUSICPACK_LOG`) and command-line flags
take precedence over them. If you prefer to keep configuration in one file
rather than in `ExecStart`, an optional `EnvironmentFile` works with the
current executable — no new config parser was added:

```text
# /etc/musicpack/musicpack-server.env
MUSICPACK_LIBRARY=/srv/musicpack/library
MUSICPACK_DATABASE=/srv/musicpack/data/library.db
MUSICPACK_LISTEN=127.0.0.1
MUSICPACK_PORT=8080
MUSICPACK_LOG=info
```

If you use it, remove the corresponding `--library/--database/--listen/--port`
flags from the unit's `ExecStart` (flags win over the environment), and keep
`--static-dir` explicit since there is no `MUSICPACK_STATIC_DIR` variable.
A template ships at `packaging/systemd/musicpack-server.env.example`.
