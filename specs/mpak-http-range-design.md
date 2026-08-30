# MPAK over HTTP Range — transport design spike

Status: **design document; transport seam and HTTP adapter
implemented** (not normative). The `musicpack_range_source`
abstraction, the stdio adapter, and `musicpack_package_open_range()`
are implemented in libmusicpack; HTTP adapters live outside it — the
optional embedding-layer component `mpakhttp/` (libcurl, gated like
musicpack-server's libmicrohttpd) for native hosts, and the
browser/WASM transport `demo/mpakrange.js` behind `wasm/mpak_wasm.c`
for `musicpack_package_open_range()` inside the Emscripten module. Both
implement this document's discovery and fetch semantics — the core
library remains network-free (§11/§14). Scope: reading `.mpak`
containers over HTTP using Range requests. This document inherits all
normative requirements from `specs/mpak-v1.md` and changes nothing about
the MPAK v1 wire format, the logical MusicPack model
(`specs/musicpack-v1.md`), or the Musepack integration.

Repository evidence this design is grounded in:

- **No C networking dependency exists** in `core/libmusicpack`,
  `core/musicpack`, or the codec libraries — by design. The only
  production HTTP client is Rust `ureq` in the author Tauri app
  (`author/src-tauri`), used for MusicBrainz/sonic-model fetches.
- **The server already serves Range correctly**: `server/src/api.c`
  implements 206/`Content-Range`/`Accept-Ranges`/`If-Range` with strong
  SHA-256 ETags over GNU libmicrohttpd (`server/src/range.c` parses
  ranges).
- **The WASM demo already implements the client side of this design** in
  JavaScript: `demo/networker.js` fetches 64 KiB block-aligned ranges,
  caches 16 blocks (~1 MiB), validates 206 + `Content-Range` + exact
  length, detects Range-ignoring servers via 200, and applies a 10 s
  timeout. `demo/rangereader.js` exposes it to the SV8 demuxer through
  synchronous read/seek/tell primitives.
- **libmusepack reads in ~64 KiB windows**: `DEMUX_BUFFER_SIZE =
  65536 - MAX_FRAME_SIZE` (`codec/libmpcdec/internal.h:51`);
  `mpc_demux_fill` requests up to a full demux buffer per fill when
  `canseek` is true, decodes sequentially, and seeks to absolute byte
  positions (SV8 `SO`→`ST`) followed by forward sequential decoding.

---

## 1. Scope and non-goals

**In scope**: the transport seam inside libmusicpack, the remote-package
open/read lifecycle, the HTTP Range protocol behavior a client adapter
must implement, caching/read-ahead, error and integrity mapping, API
changes, and the test strategy.

**Non-goals (unchanged from MPAK v1):**

- No wire-format changes; no new block types; no container-level audio
  seeking; no compression/dedup/encryption/signatures.
- No HTTP client code inside `core/libmusicpack` — the core library
  stays offline and platform-independent (existing invariant).
- No changes to libmusepack/codec; SV8 seeking remains entirely inside
  the embedded stream via the existing `mpc_reader` abstraction.
- No changes to local-directory or local-`.mpak` behavior or semantics.
- No speculative authentication/signing layer (Authorization headers may
  be supplied by the embedder through the transport, as the demo already
  does with its bearer token; the container design adds nothing).
- FLAC/WAV members over HTTP are out of scope for v1 of this design:
  `musicpack_audio_open` is path-based and dr_flac reads through a
  `FILE*`. Remote member access in this design targets codecs consumed
  via `mpc_reader` (i.e. Musepack). Remote FLAC is listed as a future
  possibility (dr_flac exposes IO-callback variants).

## 2. Existing architecture/seams

The container already exposes exactly the seams this design needs — no
new architecture is introduced:

1. **Package handle dispatch** — `musicpack_package_open(path)`
   dispatches directory vs `.mpak` file. Both backends serve the same
   `musicpack_package` handle; verification, manifest access, and
   `musicpack_package_track_open_reader` are backend-agnostic.
2. **Member-I/O vtable** (internal, `src/internal.h`): `size`, `sha256`,
   `read`, `list` — every member access already routes through
   offset+length semantics for the MPAK backend.
3. **Member reader** — `musicpack_mpak_member_reader` builds an
   `mpc_reader` over `{FILE*, base, size, pos}`. It is already
   "offset + length + read/seek/tell/get_size/canseek" — a byte-range
   source in all but name.
4. **Scanner** — `mpak_load`'s windowed scan performs only absolute
   offset reads (window refills, DATA preambles, MANF, TAIL, INDX
   payload): every I/O operation is `seek_absolute + fread`, i.e.
   mechanically replaceable by a range read at a known offset.
5. **Client-side prior art** — `demo/networker.js` (HTTP behavior) and
   `demo/rangereader.js` (reader integration) implement the access
   pattern end-to-end for the browser; the design below adopts their
   proven parameters and validation rules.

## 3. Recommended request/discovery algorithm

Normative-inherited constraints: the header is 16 bytes at offset 0;
INDX is written immediately after the header but readers MUST NOT depend
on position (spec §2); MANF is required for a semantic package; INDX and
TAIL are optional; MANF length ≤ 16 MiB; INDX ≤ ~17 MiB worst case.

**Recommended strategy: growing prefix, single strategy.**

```text
R0  GET {Range: bytes=0-262143}          # 256 KiB discovery prefix
    - 206 + Content-Range → record total size S; validate returned
      length == min(262144, S); validate prefix starts with "MPAK".
    - 200 + Content-Length → server ignored Range; the body IS the whole
      file (or its head). If Content-Length ≤ prefix cap, treat the
      response as a full download (see §9, Tier B); otherwise fail.
R1  Walk blocks from byte 16 inside the prefix exactly like the local
    scanner (validated framing only): header → INDX → MANF → …
    - INDX parsed and reconciled in memory; MANF captured if present.
R2  For any needed block whose payload is not in the prefix
    (MANF spill, TAIL, or full-scan fallback), fetch exactly the
    missing ranges as encountered.
```

Why not the alternatives:

- **TAIL-first discovery** (ZIP-EOCD-style): strictly worse for MPAK —
  writers place INDX immediately after the 16-byte header, so the tail
  costs one extra round trip in the common case and fails outright when
  TAIL is absent (it is optional). Rejected.
- **HEAD first for size**: an extra round trip; the size is available
  from the first 206's `Content-Range` total, or from `Content-Length`
  on a 200. Rejected as a separate request.
- **Fixed prefix without growth**: breaks on INDX-heavy packages
  (hundreds of members) and MANF spill. The growth step is required.
- **Full-file fallback as default**: wasteful for the common capable
  server; retained only as the explicit no-Range fallback (§9).

The discovery prefix cap (256 KiB) is a client choice, not a format
requirement; INDX+MANF for a 4096-member worst case can exceed it, which
is what the growth step (R2) covers.

## 4. HTTP Range protocol requirements

For a **fully capable** server the client requires, per request:

- `Range: bytes=<start>-<end-inclusive>` honored exactly;
- `206 Partial Content` with a correct single-part
  `Content-Range: bytes <start>-<end>/<total>`;
- response body length exactly `end - start + 1`;
- stable bytes for a given URL+range during a session (see §10).

**Validation rules (client adapter, normative for the implementation):**
adopt the demo's proven rules verbatim —

1. 206 without a parseable `Content-Range` → transport error
   (`ERR_RANGE` class).
2. 206 whose returned length ≠ requested length → truncated response,
   transport error.
3. 206 whose `Content-Range` total ≠ the size recorded at discovery →
   **remote object changed**; fail the session (do not mix two objects).
4. 200 to a ranged request → the server ignored Range. Behavior is
   policy: the adapter reports `unsupported` and the client either (a)
   aborts, or (b) switches to full download (§9 Tier B). A 200 body MUST
   NOT be interpreted as the requested range.
5. 404/403/416/5xx, timeouts, connection resets → transport errors with
   the HTTP status retained for diagnostics.
416 with a valid `Content-Range: bytes */<total>` response is the
   canonical way to learn the size of an empty-file edge; otherwise it is
   an error.
6. `Content-Encoding` MUST NOT be accepted on range responses (a
   transparently-decompressed body shifts offsets); treat as transport
   error. `Transfer-Encoding: chunked` is acceptable only on 200
   full-body responses where offsets are irrelevant.
7. Redirects (3xx) may be followed transparently by the HTTP stack; the
   final URL becomes the session URL and all subsequent validation
   applies to it.

## 5. Remote MPAK lifecycle

```text
open(url)
  1. build transport (embedder-provided), discover per §3
  2. size S recorded; header validated (magic/major; minor tolerant)
  3. blocks walked from byte 16 (framing-validated; INDX parsed,
     reconciled against the scan exactly as locally; MANF captured)
  4. manifest parsed from MANF bytes → musicpack_package handle
     (same type as local; io vtable = remote member source)
read member
  5. logical path → member {offset, length} (index or scan, identical
     to local) → mpc_reader over the range source + block cache
verify (optional, caller-invoked as today)
  6. member SHA-256 via ranged reads; TAIL digest via ranged read of
     [0, tail_offset) — identical rules to local (§8)
close
  7. transport closed; caches dropped
```

Scan fallback when INDX is absent/corrupt: the client walks DATA blocks
sequentially exactly like the local recovery scan (recovery-mode
preamble-skip semantics apply for unpack-style extraction; normal open
keeps hard errors). Over HTTP this costs one 64 KiB block fetch per
~64 KiB of container — linear, and the block cache makes it a one-time
cost during open.

**Unknown total size** (no `Content-Length`, no `Content-Range` total,
e.g. chunked responses): the container reader requires a determinable
size — TAIL validation, member-length sanity, and scan bounds all
depend on `file_size`. If the size cannot be determined from the
discovery response, the adapter performs one size probe
(`Range: bytes=0-0` → `Content-Range: bytes 0-0/<total>`); if that also
fails, open fails with a transport error. This mirrors the local
scanner's hard requirement on `mf->file_size`.

**Malformed server responses** (garbage headers, missing fields): map to
transport errors; never parse partially-validated responses as container
bytes.

## 6. Range-backed member reader design

The existing member reader `{FILE*, base, size, pos}` generalizes to
`{range_source, base, size, pos}` with zero interface change:

- `read` — copy from the block cache; on miss, fetch the covering
  blocks (§7) and retry; on transport failure return 0 and set a sticky
  error flag (the SV8 demuxer treats a 0 read at a needed boundary as
  end-of-data/error via `read_error`, which already fails decode safely).
- `seek` — move `pos` only (validated `≤ size`); no network activity.
  This is why seek is cheap and why SV8 seeks translate unchanged.
- `tell` / `get_size` — pure fields.
- `canseek` — **true**: the source is random-access by construction.
  A non-seekable variant is not part of this design.

Public/private boundary: callers keep using
`musicpack_package_track_open_reader` — no HTTP type, no offset, no
transport ever appears in the public API. The embedder-supplied transport
is an opaque pointer inside the package handle.

## 7. Musepack integration

Traced requirements from `codec/libmpcdec` (unchanged, read-only):

- `read` — `mpc_demux_fill` requests up to `DEMUX_BUFFER_SIZE`
  (~60 KiB) per fill while decoding sequentially; the SV8 header scan
  reads small amounts; the ST table is read in one bounded burst
  (≤ ~65536 entries).
- `seek` — `mpc_demux_seek` repositions to an absolute byte offset from
  the SO/ST tables (may be backward), then decodes forward; it never
  reads at a negative or out-of-size position (member size is reported
  by `get_size`).
- `tell`, `get_size` — trivially satisfied by the reader fields.
- `canseek` — must be true; the demuxer honors it for ST loading and
  full-buffer fills.

**SV8 seeking therefore operates unchanged over HTTP-backed reads.** A
seek is one cache miss (one or two 64 KiB block fetches at the target)
followed by forward sequential decode that is fully cache-resident.
No MPAK-specific seeking logic exists or is added; the container never
parses SV8 structures.

Known subtle issues to handle in the adapter:

- **Request amplification on tiny reads**: the SH/RG/EI/SO parse reads
  a few hundred bytes; block-aligned fetching turns that into 64 KiB.
  Acceptable (amortized by the cache) and strictly better than
  range-per-read.
- **Backward seeks**: cache misses on seek targets cost one round trip;
  the seek itself must not block on network I/O (position update only),
  matching `mpc_demux_seek`'s transactional behavior.
- **Read-past-end**: demuxer reads may request up to buffer-size bytes
  near EOF; the reader clamps to `size` exactly as the local member
  reader does (`mpc_demux_fill` tolerates short reads at EOF).

## 8. Caching/read-ahead strategy

**Adopt the demo's proven parameters; nothing more.**

- Block size **64 KiB**, block-aligned fetches: `base = pos & ~0xFFFF`,
  fetch `[base, base + 64 KiB)` clamped to size.
- Cache: **16 blocks (~1 MiB), insertion-order eviction** (demo uses
  `Map` insertion order; a C implementation uses the same LRU-by-
  insertion policy — good enough because decode access is sequential
  with occasional seek blips).
- Optional trivial read-ahead of the next block on a cache hit is
  permitted but not required for v1 (sequential decode already
  amortizes; the demux buffer itself is the read-ahead).
- Header/INDX/MANF blocks fetched during discovery stay in the cache
  (they are ordinary blocks), making reopen cheap for cached URLs.

Rejected as over-engineering for now: persistent disk caches, prefetch
worker threads, parallel range fetches, adaptive block sizes. The
measurement gate for adding any of these is the same style of evidence
that closed the SIMD work: profile first.

## 9. Error semantics

Mapping onto the existing `musicpack_status` model and reader
conventions:

| Condition | Mapping |
|---|---|
| Connection failure / timeout / reset | `MUSICPACK_ERR_IO`; sticky reader error |
| 404 | `MUSICPACK_ERR_MISSING` |
| 403/401 | `MUSICPACK_ERR_IO` (status surfaced via report/last-error) |
| 5xx | `MUSICPACK_ERR_IO` (retry policy is the transport's, not the container's) |
| 200 on a ranged request | adapter-level `unsupported` → open fails, or explicit full-download fallback (§9 Tier B) |
| 206 without/with malformed `Content-Range` | `MUSICPACK_ERR_IO` |
| 206 body length ≠ requested | `MUSICPACK_ERR_IO` |
| `Content-Range` total ≠ discovery size | **changed object**: `MUSICPACK_ERR_IO`, session aborted (restart required) |
| Short response / early EOF | `MUSICPACK_ERR_IO`; sticky reader error |
| Bad magic / wrong major / malformed framing | `MUSICPACK_ERR_INVALID` / `ERR_VERSION` — identical to local |
| Member SHA-256 mismatch (verify) | `MUSICPACK_ERR_CHECKSUM`, localized to the member — identical to local |
| TAIL digest mismatch / computation failure | verification error — identical to local |

Sticky-error rule: once a transport error occurs mid-read, the reader
fails all subsequent reads (the demuxer's `read_error` path already
fails decode and, for seekable readers, refuses further seeks); the
package handle remains closeable. No automatic transparent retry inside
the container layer — retry/reconnect is the transport adapter's job
(the demo uses a 10 s timeout and no retries; the server's strong ETags
make retry-after-partial safe to add later).

**Changed-remote protection** is layered: (1) every 206's
`Content-Range` total must equal the discovery size; (2) `If-Range` with
the discovery ETag is sent when the server offered one (the MusicPack
server does); (3) member SHA-256 vs MANF and the TAIL digest catch
anything the first two layers miss. A change detected mid-session fails
the session; the caller restarts open.

## 10. Integrity/consistency model

Layers, weakest to strongest — each strictly additive, all inherited
from MPAK v1 with no new mechanism:

1. **HTTP transport** — TLS if https; correctness of a single response
   (status, headers, length). Not trusted for content identity.
2. **Physical framing** — per-block CRC-16 (resync confidence only,
   never an integrity guarantee).
3. **Scan consistency** — framing walk + INDX↔scan reconciliation
   before the index is trusted.
4. **Member integrity** — SHA-256 per manifest-referenced member vs
   MANF (the semantic authority), via ranged reads.
5. **Whole-package fixity** — optional TAIL digest over [0, tail).

The design adds no integrity mechanism and removes none; remote reading
verifies exactly what local reading verifies.

## 11. API proposal

The smallest change that keeps a single package model (no parallel
"remote package" type):

```c
/* musicpack/mpak.h (or a new musicpack/range.h) — proposed, ~20 lines */

typedef struct musicpack_range_source {
    void *ctx;
    /* Total container size in bytes; required (see §5 unknown-size). */
    musicpack_status (*size)(void *ctx, uint64_t *out);
    /* Read exactly [offset, offset+len); len > 0; no short reads. */
    musicpack_status (*read)(void *ctx, uint64_t offset,
                             unsigned char *buf, size_t len);
    void (*destroy)(void *ctx);   /* may be NULL */
} musicpack_range_source;

/* Returns the same musicpack_package handle as local opens. The source
   is owned by the package after a successful open. */
MUSICPACK_API musicpack_package *
musicpack_package_open_range(const musicpack_range_source *src,
                             musicpack_status *status);

/* Local-file adapter over the same seam (hardened regular-file open;
   the reference implementation and offline test target). */
MUSICPACK_API musicpack_status
musicpack_range_source_stdio(const char *path,
                             musicpack_range_source *out);
```

- `musicpack_package_open(path)` is unchanged (dir vs file dispatch).
- `musicpack_package_track_open_reader`,
  `musicpack_package_read_member`, `musicpack_package_verify`,
  manifest access — all work unchanged on the returned handle.
- Directory-only APIs (`save_manifest`, `resolve_path`) keep returning
  `ERR_INVALID` for remote packages, exactly as for local `.mpak`.

**Where the HTTP adapter lives** (follow-up decision, not this design's
core): the transport vtable is the only seam libmusicpack provides. The
first real adapter should be the browser/WASM one (fetch — already
proven in `demo/`), and/or a small optional C component (e.g.
`core/libmpakhttp` with libcurl, built only when found — mirroring how
`musicpack_server_cmd` is conditionally built against MHD). The author
app can equally implement the vtable over `ureq` via FFI. The container
design is indifferent; nothing below the vtable is specified here.

## 12. Testing strategy

Two layers, both planned before implementation:

**Unit (in-process, no sockets — deterministic):**

- Fake `musicpack_range_source` scripted per test (log of range reads;
  programmable failures). Assert exact requested ranges/counts for:
  discovery prefix; INDX-in-prefix (no further fetch); INDX spill (one
  growth fetch); MANF retrieval; member read; repeated member reads
  (cache hits — assert request count does not grow); MPC reader
  read/seek/tell sequences over the fake source.
- Container-level edge cases through the same seam: missing INDX, hint
  lie, corrupt INDX, missing TAIL, corrupt TAIL — each asserting the
  local-semantics outcome plus the network request pattern.
- Changed-object: a fake source whose reported size changes mid-session.
- Local stdio adapter: byte-equality with direct file reads.

**Integration (real HTTP over loopback):**

- A minimal loopback HTTP server in the test (or MHD, already a server
  dependency) serving a fixture `.mpak` with scripted behaviors:
  correct 206s; 200-ignore-Range; missing/malformed `Content-Range`;
  short body; wrong `Content-Range` total (changed object); 404; 5xx
  then success (if retry is implemented); Content-Encoding present
  (must be rejected); unknown size (no Content-Length).
- The mock records every request's `Range` header; tests assert the
  request sequence from §3/§7.
- End-to-end: open → verify → `track_open_reader` → full decode of the
  MPC fixture through HTTP, byte-exact against the local decode.

Existing local suites (mpak, mpack, run_mpack.sh) remain the regression
base for unchanged behavior.

## 13. Security considerations

- TLS termination/verification belongs to the adapter; the container
  layer performs no downgrade and never disables certificate checks.
- Credentials: supplied by the embedder via the transport (demo passes a
  bearer token); never stored in the container, never logged by the
  container layer.
- Redirects: only follow within the embedder's policy; re-validate size
  on the final URL (rule 3, §4) so a redirect cannot silently substitute
  a different object.
- Response caps: the adapter enforces a maximum response length (a
  range request can never yield more than its requested length; a 200
  fallback download is capped by a configurable limit before spooling).
- Header caps: response header size limits are the HTTP stack's; the
  container layer additionally rejects unparseable `Content-Range`
  rather than guessing.
- No compression: `Content-Encoding` on range responses is rejected
  (offset integrity); this also closes decompression-bomb responses.
- Hostile container bytes over HTTP are exactly as hostile as local
  files: all existing MPAK validation (checked arithmetic, path rules,
  bounds) applies unchanged; nothing new trusts the network.
- The demo's cross-origin-isolation requirement (COOP/COEP for
  SharedArrayBuffer) is a browser deployment note, not a container
  concern.

## 14. Alternatives considered and rejected

- **libcurl inside libmusicpack**: violates the core library's
  offline/platform-independence invariant; drags a large dependency into
  every consumer. Rejected for the core; acceptable as an *optional*
  adapter component later (mirroring the MHD pattern).
- **HTTP-aware container format** (e.g., mandatory TAIL index, fixed
  INDX capacity padding, byte-reserved windows for backpatching):
  changes the frozen wire format for a transport the format already
  supports. Rejected.
- **TAIL-first discovery**: extra round trip in the common case; fails
  when TAIL is absent. Rejected (§3).
- **Range-per-read without caching**: unacceptable — the demuxer's
  sequential fills would still coalesce, but tiny metadata reads
  (SH/RG/EI/SO) and the ST burst would each cost a round trip; the
  block cache removes the class of problem for ~1 MiB of memory.
  Rejected.
- **Whole-file download as the primary path**: simplest and correct for
  Range-less servers, but wastes the format's central design goal
  (early index, O(log n) member access, 2–3 requests to first audio).
  Retained only as the explicit Tier-B fallback.
- **A parallel "remote package" API**: rejected — the existing
  `musicpack_package` handle already abstracts storage; a second model
  would fork verification/reader code for no capability.
- **Server-side extraction as the only answer** (status quo): the
  server already streams members with strong ETags, but it cannot serve
  static object storage, offline libraries, or P2P/CDN distribution of
  `.mpak`; direct client streaming is the capability this design adds.

## 15. Open questions

1. **C reference HTTP adapter**: libcurl (new optional dependency,
   `MPC_BUILD_MPAKHTTP`-style gate) vs leaving HTTP adapters entirely to
   embedders (browser first). The vtable makes this reversible; decide
   when the first native client exists.
2. **Full-download fallback policy** (Tier B): fail-fast vs
   download-to-temp-then-open (temp files introduce cleanup/atomicity
   concerns mirroring the CLI's staging discipline). Recommend: fail
   fast in the library; let embedders implement spooling if they want it.
3. **Retry policy**: none in v1 (transport's job). Revisit only with
   measurement.
4. Whether `musicpack_package_open` should someday auto-detect URL
   schemes — deliberately not proposed; explicit `open_range` keeps the
   transport injection honest.

## 16. Concrete implementation plan (follow-up task)

Implementation status: steps 1–5 and 7 are **implemented** — the
transport seam, scanner generalization through the container-I/O layer,
the remote/local member backend with the §8 block cache, the stdio
adapter, the libcurl adapter, the browser/WASM transport (step 7), and
the §12 test layers (unit/adversarial over a scripted fake source, real
loopback-HTTP integration tests for both adapters covering every §9
error row, changed-object detection, and MPC decode/seek over HTTP).
Still open: CLI URL support.

**Browser/WASM adapter (implemented).** `demo/mpakrange.js` is the
fetch()-based transport; `wasm/mpak_wasm.c` (Emscripten builds only)
exposes `mpak_wasm_open_range()/verify/track_*` over a
`musicpack_range_source` whose reads bridge to synchronous JS imports
(`wasm/mpak_range_library.js`). Browser networking is asynchronous while
`musicpack_range_source.read` is synchronous, so the adapter uses the
demo's proven acquire-then-serve model (the Phase 4 pattern from
`wasm/range_library.js`): the complete container is downloaded and
validated over the §3 discovery request plus 64 KiB §8-aligned block
requests, and `musicpack_package_open_range()` then runs synchronously
over the validated bytes. Demand-driven per-miss fetching (the Phase 5
`networker.js` model) requires SharedArrayBuffer/Atomics.wait in a
worker with cross-origin isolation (COOP/COEP) and a JS-side block cache
duplicating the C cache; it remains the documented future path for
progressive playback. Browser-specific limitations: `If-Range` and
`Accept-Encoding: identity` are not CORS-safelisted request headers
(cross-origin containers trigger a preflight the server must allow;
`ifRange: false` opts out, leaving the total-size check plus the MPAK
CRC/SHA-256 layers); `Content-Range` values beyond 2^53 are rejected as
malformed; fetch combines duplicate response headers, which fails
Content-Range parsing (fail-closed). The acquisition is bounded
(`maxBytes`, default 2 GiB — a lying discovery total is rejected before
any container-sized allocation), `Content-Type: multipart/byteranges`
responses are explicitly rejected, and the synchronous WASM imports
enforce one-active-source-per-Module (a second `install()` fails with a
typed error until the active source is destroyed). The Emscripten smoke
(`wasm/smoke_mpak.js`, run by `tests/run_mpak_smoke.sh`) uses the
committed deterministic container fixture
`tests/fixtures/mpak-range-test.mpak`.

1. **Seam**: add `musicpack_range_source` (+ stdio adapter) and
   `musicpack_package_open_range` (mpak.h/`internal.h`; ~100 lines).
2. **Scanner generalization**: replace `seek_absolute + fread` pairs in
   `mpak_load` with calls through an internal container-source wrapper
   (FILE* today, range source for remote); keep the windowed resync and
   all validation byte-for-byte. Local behavior must remain
   hash-identical (regression: existing suites + deterministic-pack
   hash).
3. **Remote backend**: member-I/O vtable implementation over
   `{source, cache}`; member reader over the same; open lifecycle per §5.
4. **Caching**: 64 KiB blocks / 16-entry insertion-order cache as §8.
5. **Unit tests** (fake source) then **integration tests** (loopback
   mock server) per §12.
6. **CLI**: optional `--url` support on `info`/`verify` for manual
   testing only (the CLI's no-network invariant is about *shipping*
   network I/O in the default flow; if that invariant is judged
   absolute, test via the C suites and skip CLI changes).
7. **Browser adapter**: port `demo/networker.js` behavior behind the
   vtable for the WASM build (mostly mechanical; the JS side already
   exists).
8. **Docs**: a short section in `specs/mpak-v1.md` ("remote reading")
   referencing this design, and a README note in `demo/`.

Estimated size: seam + backend + cache ≈ 400–600 lines of C plus tests;
no wire-format, codec, or public-semantic changes.
