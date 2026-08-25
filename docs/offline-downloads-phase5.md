# Offline `.mpack` Downloads — Phase: offline v1

<!--
Copyright (c) 2026, The MusicPack Development Team
SPDX-License-Identifier: BSD-3-Clause
-->

Status: **shipped (v1)**. Architecture approved; decisions D1–D3 binding.
Companion docs: `web/README.md` (user-facing rules),
`specs/musicpack-api-v1.md` (content-hash fields).

## Scope

Complete `.mpack` releases download to browser-local storage and play
without network connectivity, inside the existing web architecture:

```
server per-object HTTP (Range, ETag=sha256)
    → installer: fetch → stage → incremental SHA-256 → ATOMIC COMMIT
    → OPFS files + IndexedDB catalog
    → availability  ─┐
                     ├─→ resolveAudio() (UNCHANGED) → itemForTrack()
    canPlay (cap.)  ─┘            → PlaybackItem → Player Core (UNTOUCHED)
```

## Product decisions (binding)

| # | Decision | Rule |
|---|---|---|
| D1 | Local-first | installed ⇒ play local, online AND offline |
| D2 | User-initiated updates | hash diffs flag `stale`; replacement only on explicit action |
| D3 | Lyrics deferred | booklet/lyrics/extras not downloaded until consumed |

**Installed-Package Usability Invariant.** A package is *installed* iff,
in one atomically committed catalog record, every playback-critical asset
is present, size-matched and SHA-256-valid, and every other policy asset
is valid or explicitly damaged. Only committed records feed availability;
a partially-installed package can never appear playable.

## Server change (the only one)

Additive `sha256` on representation refs, waveform objects, and release
assets in release-detail JSON — the same manifest hashes the byte
endpoints already expose as strong ETags. No new endpoints; no `.mpack`
format change.

## Module map (`web/app/src/lib/offline/`)

| File | Responsibility |
|---|---|
| `sha256.ts` | incremental FIPS 180-4 hashing during streaming |
| `types.ts` | record shapes + invariant documentation |
| `plan.ts` | v1 download policy (pure): primaries + all representations + waveforms + artwork |
| `stores.ts` | FileStore/CatalogStore seams (+ memory fakes) |
| `browser-stores.ts` | OPFS file store + IndexedDB catalog |
| `installer.ts` | fetch→stage→verify→commit pipeline, typed outcomes |
| `availability.ts` | committed-package view: `allows`, `localKeyFor` |
| `manager.ts` | lifecycle, UI states, update checks, audit wiring |
| `audit.ts` | boot reconciliation vs eviction/truncation, quota probe |
| `snapshot-storage.ts` | StoragePort wrapper remapping restored items to local sources |
| `register-sw.ts` | shell service-worker registration |

## Playback path

- Musepack: `public/localreader.js` mirrors the HTTP demand reader's
  read/seek/tell contract over an OPFS sync access handle; the wasm decode
  loop is byte-source agnostic. Equivalence pinned by the wasm-gapless
  harness (`local path == memory PCM`, exact seek targets).
- Native codecs: blob object URL over the OPFS `File` (disk-backed), CSP
  already allows `media-src 'self' blob:`; URLs revoked on dispose.
- Player Core: one additive union member (`PlaybackSource.kind =
  'local-file'`); zero logic changes; purity gate untouched.

## Service worker boundary

`public/sw.js` caches the static shell only: SPA entry, hashed assets
(discovered from index.html at install — they never pass through the
worker otherwise), wasm, worklet bundle, and every classic worker script
(`decoder.worker.js`, `musepack.js`, `reader_mailbox.js`, `rangereader.js`,
`networker.js`, `localreader.js`). This last point is load-bearing: each
new decoder `Worker()` re-fetches its scripts, which fails offline under
the server's `Cache-Control: no-cache`. `/api/**` is never intercepted.

## Failure semantics

| Event | Behavior |
|---|---|
| corrupt primary/waveform/artwork | whole install rolls back, `failed(integrity)` |
| corrupt alternate representation | commit proceeds without it; excluded from availability |
| quota exceeded mid-stage | rollback + sweep, `failed(quota)` |
| interrupted install / crash | staging subtree swept on next boot |
| server content changed mid-download | caught by per-asset hash verification |
| browser evicted files later | boot audit marks package stale/damaged; reinstall heals |

## Test matrix

- Vitest units: sha256 vectors/chunking, planner policy, installer
  (atomicity/integrity/quota/abort/supersede), availability + D1 source
  rule, manager lifecycle incl. D2 flag-only updates, boot audit.
- Node harness: local-reader byte/sample equivalence + seeks
  (`web_wasm_gapless`).
- Playwright: full journey — install (real fetch/OPFS/IndexedDB) →
  sever network mid-session → local next-track + seek → offline reload →
  resumed playback + progression → online restore with remote fallback;
  remove-download lifecycle.

## Deliberately out of scope (v1)

Signing/publisher trust, transcoding/codecs, background sync, mobile
hosts, Player Core redesign, `.mpack` format changes, PWA install UX,
lyrics/booklet/extras consumption, per-track preference maps.
