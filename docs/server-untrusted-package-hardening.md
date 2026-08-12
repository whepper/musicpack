# Server untrusted-package hardening

This document describes the server ingestion trust model after the hostile-package
remediation milestone. MusicPack supports two modes:

- **Trusted local libraries**: packages are authored or placed by the operator and
  scanned with `musicpack-server verify`; fully verified packages are servable.
- **Fully untrusted package directories**: packages are treated as attacker
  controlled. Ingestion fails closed: nothing becomes visible or servable until a
  full SHA-256 verification succeeds.

The claimed status is **`safe with documented operational restrictions`**, not
`safe for untrusted packages`. The operational restrictions are listed in
[Remaining limitations](#remaining-limitations); they must hold for the claims
below to apply.

## Threat model

For an untrusted package, the attacker controls:

- manifest JSON and all metadata strings;
- filenames, directory layout, links, and all referenced file contents;
- artwork, lyrics, booklets, extras, and analysis documents;
- object counts and sizes;
- special filesystem objects (FIFOs, sockets, devices, reparse points);
- timing/races when the package directory is locally writable during scan/serve.

The trusted assets are the server host, configuration, database, and filesystem
outside the package/library root, plus the authenticated API session.

## Package ownership model

Logical release identity (release group + release edition, hashed into stable
`group_key`/`release_key`) is kept for metadata deduplication. Servable content,
however, is owned by exactly one package per release: `releases.owner_package_id`.

- A package is the owner when no release exists yet, when it already owns it, or
  when the current owner is unavailable/invalid.
- A package claiming the same release identity with an identical content
  fingerprint is a **mirror duplicate**: it attaches to the release but never
  serves or replaces content.
- A package claiming the same release identity with different content is
  quarantined as **`conflict`**: it does not mutate the owner's content or
  metadata and is never served.
- Streaming and API content enumeration resolve through `owner_package_id`, never
  through an arbitrary visible package on the same release.

### Durable conflict quarantine

**Verification can never clear a `conflict` status.** Specifically:

- `musicpack-server verify` / `mp_verify_library()` excludes packages already in
  the `conflict` state from its query entirely.
- The per-package verification-state write is additionally guarded with
  `WHERE status != 'conflict'`, so even a package that reaches the write path
  cannot have its `status` overwritten.
- A lightweight or full verification scan re-arbitrates ownership the same way
  ingestion does: a package that still collides with an active owner is
  re-quarantined, never promoted.
- A `conflict` package that moves (same content fingerprint, new path) stays
  `conflict`.

A quarantined package can only leave the `conflict` state through an explicit
ownership re-arbitration: the active owner becomes unavailable/invalid, or the
conflicting content changes to match the owner's fingerprint (making it a mirror
duplicate). Ordinary verification does not do this. The unavailable sweep also
skips `conflict` packages, so removing and re-adding a conflicting package's
directory does not clear its quarantine; if it reappears (identical or changed
content) it remains `conflict` and cannot mutate the owner.

## Verified-content model

Only fully verified packages are servable. The visibility gate requires both:

- `packages.status IN ('valid','warning')`
- `packages.verify_status IN ('valid','warning')`

A lightweight scan leaves `verify_status = 'unverified'`, so newly discovered
external packages are not visible or servable until `musicpack-server verify`
(scan with `--verify`) succeeds. The unchanged-manifest fast path re-checks
referenced-object existence on every scan; a disappeared object downgrades the
package to `unverified` so it stops serving.

### Verification persistence is fail-closed

If the database cannot persist a verification result, verification **fails**:

- every per-package `pkg_set_verify()` write is checked;
- a failed write aborts the whole verification run (transaction rolled back) and
  `mp_verify_library()` returns `MUSICPACK_ERR_IO`;
- the job layer records the failure, and the CLI returns a non-zero exit;
- no package is reported verified (or made servable) unless its state was
  durably written.

### Verified-byte limitation

A strong ETag is emitted only for servable (verified) content. **Verified bytes
are not copied into immutable server-owned storage**: serving reopens the package
pathname and does not re-hash at serve time, so a post-verification in-place
mutation is served until the next rescan/verify detects it (a lightweight rescan
re-checks object existence; a full `verify` re-hashes). Therefore, untrusted
packages must reside in non-mutable, server-controlled storage after
verification. In-place mutation after verification is detected only by the next
rescan/verify; there is no claim that served bytes always equal the historically
verified bytes.

## Filesystem containment

Manifest paths are validated lexically (absolute paths, `..`, `.`, backslashes,
colons, controls, empty segments rejected) and the existing-ancestor symlink
canonicalization check remains. Additional enforcement:

- **Opened-object regular-file requirement**: every referenced object must be a
  regular file, and the type check is bound to the object that is actually read
  or hashed. POSIX paths open first with `O_RDONLY | O_NONBLOCK | O_NOFOLLOW`,
  then `fstat` on that descriptor requires `S_ISREG`; the same descriptor is
  used for hashing (via `fdopen`). Windows uses attribute checks before open.
  This removes the `stat()`-then-`fopen()` race for a regular file being swapped
  for a FIFO between check and open in the hashing/reading paths.
- **Scanner discovery**: POSIX skips symlinked directories via `lstat`; Windows
  rejects all directory reparse points via `GetFileAttributesA` so junctions and
  mount points cannot escape the configured library root or cause cycles.
- **Traversal bounds**: discovery is depth-capped (64) and a failed or incomplete
  root traversal aborts the scan without sweeping the library.
- **Hard-link policy**: on POSIX, referenced files with `st_nlink > 1` are
  rejected (a hard link can alias an outside inode). Windows does not reliably
  expose link counts, so the rejection is POSIX-only; Windows containment relies
  on the reparse-point and attribute checks. See
  [Hard-link policy](#hard-link-policy).
- Residual limitation: intermediate pathname components are not opened
  descriptor-relative (`openat`-style), so a concurrently mutable package
  directory can still race directory components. The operational restriction is
  that untrusted package trees must not be writable by the attacker during
  scan/serve.

## Asset-serving policy

Package-controlled assets cannot become active same-origin web content:

- Inline serving is allowed only for raster images whose leading bytes match
  their declared type (JPEG, PNG, GIF, WebP, BMP) and audio streams.
- SVG, HTML, JavaScript, XML, text, PDF, and unknown content are forced to
  `Content-Disposition: attachment`.
- Content-Disposition filenames are sanitized (no quotes, backslashes, controls,
  or CRLF) and the header buffer is sized so the quoted name always terminates.
- The frontend meta-CSP is not relied upon for separately navigated asset
  responses.

### Exact HTTP header coverage

Every package-object response carries these security headers **consistently**:

| Status | `X-Content-Type-Options: nosniff` | sandbox `Content-Security-Policy` | `Content-Disposition` (when not inline) |
|--------|-----------------------------------|------------------------------------|------------------------------------------|
| 200    | yes | yes | yes (non-inline types) |
| 206    | yes | yes | yes (non-inline types) |
| 304    | yes | yes | no body (not applicable) |
| 416    | yes | yes | no body (not applicable) |

`HEAD` responses use the same code path as their `GET` equivalent and therefore
carry the same headers. The previously undocumented 304/416 responses now send
`nosniff` + sandbox CSP so the documented guarantee matches reality.

## Package state machine

| Status         | Meaning                                 | Servable |
|----------------|-----------------------------------------|----------|
| `valid`        | verified OK (or present in lightweight) | only if `verify_status` is `valid`/`warning` |
| `warning`      | verified with warnings, or object missing (fast path) | only if `verify_status` is `valid`/`warning` |
| `unverified`   | lightweight scan only                   | no |
| `checksum-failed` | full verification failed             | no |
| `conflict`     | identity conflict with active owner     | no |
| `invalid`      | manifest parse/validation failed        | no |
| `unavailable`  | not seen by the last successful scan    | no |

## Scanner failure behavior

An authoritative scan succeeds **only if** the complete intended traversal and
database publication succeed. Concretely:

- Any `opendir()` failure, depth overflow, overlong path, or directory
  enumeration error (`readdir`/`closedir`) marks the traversal incomplete.
- Any `lstat`/`GetFileAttributesA` metadata failure for a discovered entry marks
  the traversal incomplete (previously it was silently skipped).
- Any package-ingestion failure (manifest read, parse, or database write) is
  propagated, not logged-and-ignored.
- Any failed database mutation (package upsert, `record_invalid`, move handling,
  fast-path refresh) aborts the scan with an error.
- The final unavailable sweep is a database write: if it fails, the scan returns
  an error rather than reporting success.
- A failed/incomplete traversal or failed sweep means the scan returns
  `MUSICPACK_ERR_IO`; the unavailable sweep is **not** run after an incomplete
  traversal, so prior library state is preserved.
- The job layer and CLI surface the failure (job `failed` flag / non-zero exit).

## Resource budgets

### Scanner (per scan)

| Budget | Limit | Notes |
|--------|-------|-------|
| Filesystem objects visited | 100,000 | directories + packages, across the whole traversal |
| Packages ingested | 10,000 | `.mpack` directories processed |
| Total path bytes | 64 MiB | sum of discovered path lengths |
| Directory depth | 64 | aborts the scan (fail closed) |

### Package verification (per package)

| Budget | Limit | Notes |
|--------|-------|-------|
| Single referenced file | 8 GiB | enforced from `fstat` size before hashing |
| Aggregate referenced bytes | 64 GiB | enforced before hashing when exceeded |
| Referenced assets | 4,096 | existing manifest cap |

Verification tracks a per-pass inode set so the same underlying object is never
hashed twice in one pass (defense in depth: hard links are rejected, and the
manifest validator rejects duplicate asset paths).

### Parser (per manifest / sonic document)

- 32 discs, 512 tracks per disc
- 64 artists per credit, 64 genres
- 32 artwork, 32 booklet, 512 lyrics, 256 extras, 32 analysis
- 4,096 total referenced assets
- manifest.json and sonic documents capped at 16 MiB
- 1,048,576 sonic dimensions, 4,096 sonic tracks

Boundary tests cover: a 9 GiB sparse referenced file (rejected by the per-file
limit before hashing) and the scanner depth/object limits (deep-tree scan fails
closed).

## Identity conflict behavior

External MusicBrainz IDs are treated as claims, not authority to mutate existing
content. A release-group/release ID is used as a key only when it is a canonical
UUID; malformed values fall back to the hashed identity. The fallback identity
serialization is canonical and length-prefixed (tag + 4-byte length + bytes), so
no two distinct field sets can collide. Conflicting claims are quarantined as
described in [Durable conflict quarantine](#durable-conflict-quarantine).

## WAV parsing guarantees (Sonic)

The Sonic WAV reader walks the RIFF/WAVE chunk structure rather than assuming a
fixed 44-byte header:

- parses the `fmt ` and `data` chunks in any order;
- safely skips unknown chunks, honoring odd-size padding;
- validates chunk lengths before access and rejects arithmetic overflow;
- validates channels (1-2), sample format (PCM 8/16/24, IEEE float 32),
  sample rate, block alignment (must equal channels × bytes/sample), byte rate,
  and data size/frame count;
- rejects compressed/unsupported format tags;
- uses unsigned arithmetic for all byte assembly (no signed-shift UB).

Musepack integration rejects stream channel counts outside 1-2 before decoding.
ASan/UBSan fixtures cover truncated/oversized chunks, malformed `fmt`, missing
`data`, odd-sized padded chunks, and unsupported formats.

## Platform differences

- POSIX: `O_NONBLOCK` + `O_NOFOLLOW` + `fstat` opened-object checks, `lstat`-based
  discovery, hard-link (`nlink > 1`) rejection, `O_NOFOLLOW` on the final serve
  component.
- Windows: reparse points rejected from discovery; regular-file checks via file
  attributes; hard links are not detected (no reliable link count); the HTTP
  executable is not built on Windows (core library only). Windows containment is
  not descriptor-relative.

## Hard-link policy

Chosen policy: **reject** referenced regular files whose POSIX link count
exceeds one (`st_nlink > 1`) in every verification, hashing, and serving path.
This prevents a hard link from aliasing an inode outside the package tree. The
rejection is enforced on POSIX only; on Windows link counts are not reliably
available, so Windows relies on the reparse/attribute checks and the
operational restriction that the tree is server-controlled and not externally
hard-linked. A POSIX regression test (`mpack_hostile`) creates a real hard link
and asserts verification rejects the package.

## Remaining limitations

The following are genuine, documented operational restrictions. **`safe for
untrusted packages` is not claimed** unless all of them hold:

- **Mutable package trees**: verified bytes are not copied into immutable
  server-owned storage; in-place mutation after verification is detected only by
  the next rescan/verify. Untrusted packages must reside in non-mutable,
  server-controlled storage after verification.
- **Pathname-based containment**: intermediate directory components are not
  opened descriptor-relative (`openat`-style). The attacker must not be able to
  race directory components during scan/serve.
- **Windows hard links**: hard-link rejection is POSIX-only (Windows cannot
  reliably detect link counts); see [Hard-link policy](#hard-link-policy).
- **Library root identity**: scans are not scoped to a persisted root id; one
  database should be used with one library root.
- **No hosted sanitizer job**: sanitizer evidence is local.

## Tests

- `mpack_hostile`: FIFO/directory/symlink-escape objects fail quickly; hard-linked
  assets rejected; oversized (9 GiB sparse) referenced files rejected by the
  per-file budget.
- `server_unit::test_ownership_conflict`: conflicting identity is quarantined and
  the owner's content/metadata are untouched.
- `server_unit::test_conflict_survives_verify`: quarantine survives
  `mp_verify_library`, rescans, a full verification scan, owner verification, and
  a remove/re-add cycle.
- `server_unit::test_scan_fail_closed`: a directory tree deeper than the scan
  limit fails the scan (fail closed, no sweep).
- `server_unit::test_scan_db_fail_closed`: a scan whose database writes fail
  returns an error, never a successful incomplete publication.
- `server_unit::test_verify_fail_closed`: verification against a read-only
  database returns `MUSICPACK_ERR_IO` (persistence failures fail closed).
- `server_integration`: unverified packages are not servable; verification makes
  them visible; checksum-failed and symlink-escape packages are hidden; 200/206/
  304/416 package-object responses carry `nosniff` + sandbox CSP; non-inline
  assets are forced to attachment with a sanitized filename.
- Sonic `test_wav_robustness`: RIFF chunk-walk rejection of truncated/oversized
  chunks, malformed `fmt`, missing/oversized `data`, bad block alignment,
  unsupported formats, and odd-padded chunks.
- v1 conformance corpus: 3 valid, 42 invalid manifests, 8 invalid asset cases.

## Validation evidence

- Local full suite (excluding the optimization-dependent local `compat`
  manifest fallback): all CTest suites pass (24/24 + the expected SIMD gate
  skip).
- Local ASan/UBSan package/server/fuzz suite: 13/13 pass.
- Hosted CI run
  [31643457000](https://github.com/whepper/musicpack/actions/runs/31643457000):
  Linux GCC, Linux Clang, macOS ARM64, Windows MSVC, Linux SIMD-off, Wasm,
  web-client (Playwright), and research all pass.

Commits:

- `51d47df` server: harden untrusted package ingestion
- `8e2a68b` web: verify e2e library before serving (fail-closed visibility)
- `545ea4b` web: add valid third edition for verified-only e2e
- `35cd020` web: fix e2e third-edition script import
- `feafcea` ci: drop flaky google-chrome apt source on Linux runners
- `97d044f` server: close untrusted-ingestion remediation gaps
- `b6bef99` package: disable inode-dedup on Windows (unreliable st_ino)
- `86a38b1` tests: deep-tree scan test must stay under Windows MAX_PATH
