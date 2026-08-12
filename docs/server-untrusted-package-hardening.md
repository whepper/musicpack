# Server untrusted-package hardening

This document describes the server ingestion trust model after the hostile-package
remediation milestone. MusicPack supports two modes:

- **Trusted local libraries**: packages are authored or placed by the operator and
  scanned with `musicpack-server verify`; fully verified packages are servable.
- **Fully untrusted package directories**: packages are treated as attacker
  controlled. Ingestion fails closed: nothing becomes visible or servable until a
  full SHA-256 verification succeeds.

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

## Verified-content model

Only fully verified packages are servable. The visibility gate requires both:

- `packages.status IN ('valid','warning')`
- `packages.verify_status IN ('valid','warning')`

A lightweight scan leaves `verify_status = 'unverified'`, so newly discovered
external packages are not visible or servable until `musicpack-server verify`
(scan with `--verify`) succeeds. The unchanged-manifest fast path re-checks
referenced-object existence on every scan; a disappeared object downgrades the
package to `unverified` so it stops serving.

A strong ETag is emitted only for servable (verified) content. In-place mutation
of a package file after verification is a documented residual limitation: bytes
are not re-hashed at serve time and packages are not copied into immutable
server-owned storage in this milestone. Rescan + verify re-establishes the
verified state.

## Filesystem containment

Manifest paths are validated lexically (absolute paths, `..`, `.`, backslashes,
colons, controls, empty segments rejected) and the existing-ancestor symlink
canonicalization check remains. Additional enforcement:

- **Regular-file requirement**: every referenced object must be a regular file.
  FIFOs, sockets, devices, directories, and unsupported reparse objects are
  rejected before hashing, probing, parsing, or serving. POSIX opens use
  `O_NONBLOCK` and an immediate `fstat`; Windows checks file attributes before
  opening.
- **Scanner discovery**: POSIX skips symlinked directories via `lstat`; Windows
  rejects all directory reparse points via `GetFileAttributesA` so junctions and
  mount points cannot escape the configured library root or cause cycles.
- **Traversal bounds**: discovery is depth-capped (64) and a failed or incomplete
  root traversal aborts the scan without sweeping the library.
- Residual limitation: containment validation and the later `open()` are separate
  operations (pathname-based, not descriptor-relative), so a concurrently mutable
  package directory remains an open research item. Hard links can also alias an
  outside inode. These are documented operational restrictions for the fully
  untrusted directory mode.

## Asset-serving policy

Package-controlled assets cannot become active same-origin web content:

- Inline serving is allowed only for byte-verified raster images (JPEG, PNG, GIF,
  WebP, BMP) and audio streams.
- SVG, HTML, JavaScript, XML, text, PDF, and unknown content are forced to
  `Content-Disposition: attachment`.
- Every package-object response sends `X-Content-Type-Options: nosniff` and a
  restrictive `Content-Security-Policy: sandbox; default-src 'none'; img-src
  'self'`.
- Content-Disposition filenames are sanitized (no quotes, backslashes, controls,
  or CRLF).
- The frontend meta-CSP is not relied upon for separately navigated asset
  responses.

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

- If the configured library root (or any subtree) cannot be opened or enumerated,
  the scan returns an error and the unavailable sweep is **not** run, preserving
  previous library state.
- Scans are not scoped to a persisted root identity in this milestone; a single
  database should be used with one library root.
- Explicit verification always re-hashes referenced objects; the unchanged
  manifest fast path never bypasses a `verify` scan.

## Resource budgets

The manifest parser enforces per-array limits before typed allocation:

- 32 discs, 512 tracks per disc
- 64 artists per credit, 64 genres
- 32 artwork, 32 booklet, 512 lyrics, 256 extras, 32 analysis
- 4,096 total referenced assets (existing)

Regular-file and size checks prevent FIFO/device blocking. Total hashing work is
bounded by the asset cap; per-file and aggregate byte budgets are not yet
enforced. Sonic documents are capped at 16 MiB, 1,048,576 dimensions, and 4,096
tracks.

## Identity conflict behavior

External MusicBrainz IDs are treated as claims, not authority to mutate existing
content. A release-group/release ID is used as a key only when it is a canonical
UUID; malformed values fall back to the hashed identity. The fallback identity
serialization is canonical and length-prefixed (tag + 4-byte length + bytes), so
no two distinct field sets can collide. Conflicting claims are quarantined.

## Platform differences

- POSIX: `O_NONBLOCK` + `fstat`, `lstat`-based discovery, `O_NOFOLLOW` on the
  final serve component.
- Windows: reparse points rejected from discovery; regular-file checks via file
  attributes; the HTTP executable is not built on Windows (core library only).
  Windows containment is not descriptor-relative.

## Remaining limitations

- No immutable server-owned verified-content store; in-place mutation after
  verification is detected only by the next rescan/verify.
- Containment is pathname-based; descriptor-relative traversal (`openat`-style)
  is future work.
- Hard links are not rejected; a hard link can alias an outside inode.
- Scans are not scoped to a canonical library-root id; one database, one root.
- No per-file or aggregate byte budgets yet.
- No hosted sanitizer job; sanitizer evidence is local.

## Tests

- `mpack_hostile`: FIFO/directory/symlink-escape objects fail quickly.
- `server_unit::test_ownership_conflict`: conflicting identity is quarantined
  and the owner's content/metadata are untouched.
- `server_integration`: unverified packages are not servable; verification makes
  them visible; checksum-failed and symlink-escape packages are hidden.
- v1 conformance corpus: 3 valid, 42 invalid manifests, 8 invalid asset cases.
