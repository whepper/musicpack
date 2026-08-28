# MusicPack web client — the digital record shelf

The first-party MusicPack web application (Phase 6). It is a **digital record
shelf**, not a streaming clone: the primary entity is an intentionally
collected album/release, multiple editions are meaningful (never flattened),
and technical release information sits alongside the artwork.

```text
authenticate (token once) → HttpOnly session cookie
    ↓
album shelf (artwork-first grid, infinite scroll, search, recently added)
    ↓
album page → hero + seven ?section= panels + context rail → track detail
    ↓
/tracks/:id → playback actions, waveform, loudness, representations, hashes
    ↓
Play Album → BS.1770 album normalization → Musepack demand-driven WASM
             decoding → AudioWorklet ring → gapless to the next track
```

## Technology decision

| choice        | reason                                                                 |
|---------------|------------------------------------------------------------------------|
| **Svelte 5**  | Smallest shipped JS of the component frameworks (compiles away, no VDOM runtime); declarative components + built-in a11y linting keep a long-lived first-party client small and maintainable. |
| **Vite**      | First-class Svelte/TS support, fast dev server, static build with no SSR, emits the AudioWorklet as a bundled entry. |
| **TypeScript**| The codebase and the API contract are typed; strict mode + `noUncheckedIndexedAccess`. |
| **Vitest**    | Node unit tests for the framework-free core (controller, queue, ring, loudness, API client). |
| **Playwright**| Real Chromium browser integration against the actual server + wasm decoder. |

The client is plain static assets (`npm run build` → `web/app/dist`) served by
`musicpack-server --static-dir`. There is no Node runtime in production.

When a track has a package-provided waveform envelope, the Now Playing seek
control renders it on Canvas and supports pointer and keyboard seeking. Tracks
without one retain the linear range fallback; the browser never decodes audio
to synthesize an envelope.

## Layout

```text
web/
  player-core/         platform-independent player domain core (zero deps):
                       PlaybackItem types, Engine port + capabilities,
                       generic QueueModel + shuffle/repeat order policy,
                       Player orchestrator (transport, gapless bookkeeping,
                       persistence codecs), BS.1770 gain policy, event
                       surface — see player-core/README.md
  app/                 Vite root (the application)
    public/            classic worker + wasm + demand-reader scripts (synced)
    src/
      lib/
        api/           typed HTTP v1 client + error mapping
        auth/          session store (token → cookie, never stored)
        state/         library (shelf, editions), queue (core adapter),
                       player model
        playback/      web facade + engines: MusepackEngine, NativeBackend,
                       codec resolution, Media Session, loudness re-export
                       (transport/queue/persistence semantics live in
                       ../player-core)
        ui/            Svelte components + theme
        router.ts      history-API SPA router
  tests/
    unit/              Vitest units + the wasm+ring feed test
    node/              Node wasm gapless/seek harness (ctest web_wasm_gapless)
    e2e/               Playwright browser suite
    perf/              perf report script
  scripts/sync-wasm.sh copies the built wasm + demand reader into app/public
```

## Development

Requirements: a built `libmusepack.wasm` module (`build-wasm/wasm/musepack.*`)
and a running `musicpack-server` with a scanned library.

```sh
# 1. build the wasm decoder module (once)
emcmake cmake -S . -B build-wasm && cmake --build build-wasm --target musepack_wasm -j

# 2. build the server (once), scan a library, create a token
cmake -S . -B build && cmake --build build -j --target musicpack_server_cmd
build/server/musicpack-server scan --library ./library --database ./library.db
build/server/musicpack-server token create --name Web --database ./library.db

# 3. run the client against the server (Vite proxies /api, COOP/COEP on)
cd web && npm install && npm run dev        # http://localhost:5173

# production build + serve
cd web && npm run build
../build/server/musicpack-server serve --library ./library --database ./library.db \
    --static-dir web/app/dist
```

**Cross-origin isolation:** the demand-driven reader needs SharedArrayBuffer,
so the page must be cross-origin isolated. `musicpack-server --static-dir`
already sends `Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp`; the Vite dev server sends the
same headers. The server must be reachable at `http://127.0.0.1:8080` for the
dev proxy (or set `server.proxy` in `vite.config.ts`).

**Authentication:** sign in with a server token once. It is exchanged for an
HttpOnly `musicpack_session` cookie and is never stored in the browser.
Bearer tokens remain supported by the API for CLI/native clients.

## Tests

```sh
cd web
npm run check          # svelte-check + tsc
npm run test:unit      # Vitest units (needs build-wasm for the wasm+ring test)
npm run test:node      # Node wasm gapless/seek harness
npx playwright test    # browser e2e (builds a fixture library + starts the server)
node tests/perf/perf.mjs   # performance report
```

The `web_wasm_gapless` ctest (registered under the wasm build) runs the Node
harness in CI.

### E2E / CI reliability notes

Known test-infra behavior, recorded so future investigation starts in the
right place — neither item indicates a production defect:

- **Offline journey (`offline.spec.ts` "download once via the UI…")** is
  intentionally an end-to-end integration test: it exercises real
  network-severance, the service-worker shell, OPFS storage and session
  restore together. It exhibited intermittent **CI-only** failures during
  offline mid-album reload/resume while passing consistently in local
  CI-mode runs (including full-suite repetitions). **Root-caused** via the
  self-reporting state dump: the seek step ran on track 2 — a ~1 s fixture
  clip — and the slider clamps a 10 s seek to the track boundary, which
  queued the auto-advance chain into the album end. Reloading before the
  chain finished restored a playable mid-album session (local timing);
  reloading after it persisted `ended` and Play never resumed (CI timing).
  The seek now happens on the 48 s track 1, so the persisted session is
  deterministically playable; cross-track offline coverage moved to the
  post-reload Next step.
- **`playback.spec.ts` "removing the playing queue item…"** has a
  documented load race (asserting player state while the core may still be
  loading the neighbor). It is **pre-existing** and was observed flaking on
  CI commits *before* the Offline & Representation UX milestone; it passes
  on retry.

## Gapless and native playback notes

- **Musepack (exact):** two decoder workers — the current track's worker and a
  second worker already opened on the next track. At the exact sample boundary
  the player promotes the standby worker and keeps feeding the same ring,
  so adjacent tracks of a release play without inserted silence.
- **Native codecs (browser `<audio>`, e.g. FLAC):** the next track is
  preloaded into a second element and swapped in on `ended`. Browsers expose
  no sample-perfect gapless API for `<audio>`, so a small boundary gap may
  occur; this is a platform limitation, not a codec defect.

## Playback policy (repeat / shuffle)

The queue is one canonical track list; ordering never destroys it.

- **Repeat** off / all (wraps to the first track) / one (reloads the current
  track at EOS — sample-exact through the normal load path).
- **Shuffle** builds a presentation order (current track stays first); the
  Previous button retraces actual navigation history, and toggling shuffle
  off restores canonical order. The policy persists across page reloads
  (session snapshot v2).
- The repeat-all wrap preloads the wrap target into the standby decoder, so
  the boundary stays gapless on Musepack tracks.
- **Queue reorder**: ▲/▼ buttons per queue item move tracks within the
  canonical list; the cursor follows the moved item and Previous history
  stays valid.

## Audio representations (Phase 4)

A package may ship alternate audio representations per track (Phase 3
`representations[]`); the primary `audio` entry remains the frozen default.
Selection is a pure web-domain policy — `resolveAudio()` in
`lib/state/representation-selection.ts` — consumed only by `itemForTrack()`,
so Player Core, the engines and the queue model stay representation-blind:

- **No preference (default)** plays the primary audio exactly as before
  Phase 4; tracks without representations are unaffected by any preference.
- One active preference exists: `default`, an explicit representation id,
  an exact codec family (`codec: "flac"`), or `lossless` (closed set:
  flac/wav/aiff). It persists under its own localStorage key
  (`musicpack.audio-preference.v1`) — deliberately outside the player-core
  snapshot schema. The user-facing control lives on `/settings` (see
  "Audio preference & settings" below).
- Fallback is deterministic and total: a preference that matches nothing
  falls back to the primary; an unplayable primary may be rescued by the
  first playable alternate in manifest order; if nothing is playable the
  item is still built and today's unsupported-format error surfaces at open.
- Playability is injected (`browserCanPlay`, shared with backend resolution
  so both can never disagree); a future offline host can inject local-file
  availability instead.
- Selected items get identity `t{trackId}r{repId}` so the same track can
  appear twice with different sources unambiguously; default items keep the
  plain `t{trackId}` identity. Changing the preference never rebuilds or
  restarts already-built/playing items — it applies from the next
  construction onward.

## Offline downloads (offline `.mpack`, Phase: offline v1)

Complete releases can be downloaded for offline playback. The governing
rules:

- **Installed-Package Usability Invariant:** a package is *installed* iff
  one atomically committed catalog record holds every playback-critical
  asset (each track's primary audio, waveforms, referenced artwork)
  verified against the manifest SHA-256 — with any damaged non-critical
  asset (an alternate representation) explicitly excluded rather than
  silently absent. Staged/failed/orphaned files are invisible to playback;
  a partially-installed package can never appear playable.
- **D1 local-first:** an installed package's selected assets ALWAYS play
  from local storage, online or offline; remote sources serve only
  non-installed content.
- **D2 user-initiated updates:** online hash differences only FLAG a
  package `stale`; replacement happens strictly via explicit user action.
- **D3 lyrics deferred:** booklet/lyrics/extras are not downloaded until a
  consuming feature ships.
- **Download policy (v1):** every track's primary audio + ALL declared
  representations + per-track waveforms + release artwork. Waveforms ride
  along because the seek control AND Sweet-Fade planning consume them.
- **Selection stays Phase 4:** `resolveAudio()` remains the single policy;
  offline availability composes in at item construction (`SelectionContext.
  offline`) and through the injected `canPlay` candidate identity. Player
  Core gained only an additive `'local-file'` member on `PlaybackSource`.
- **Storage split:** OPFS (`musicpack-offline-v1/releases/…`, staged under
  `.staging/<installId>/…`) holds audio bytes; IndexedDB holds catalog
  records + small blobs; localStorage keys are unchanged. The static shell
  service worker (`public/sw.js`) caches ONLY the app shell + worker/wasm
  scripts — it never intercepts `/api/**` and holds no domain state.
- **Integrity:** bytes are hashed incrementally while streaming into
  staging and verified before commit; corrupt primaries fail the install,
  corrupt alternates commit nothing for that asset. Boot-time audit
  reconciles files vs catalog (browser eviction → `damaged` → excluded
  until reinstall). Signing/publisher trust remains out of scope.
- **Offline session:** a boot network failure WITH installed content
  enters `AuthState 'offline'` (degraded-authenticated) instead of the
  sign-in screen; `navigator.onLine` is deliberately not consulted.
  The top bar shows an Offline chip while degraded, and the app re-probes
  on connectivity return (an `online` listener plus a slow fallback timer,
  since a page booted offline may never receive the event). Reconnect
  never demotes an offline session to sign-in.
- **UI surface:** every release/edition page carries a download control
  (`DownloadControl.svelte`) covering the full lifecycle — Download with
  progress ring, Installed badge (with size hint), Update available
  (stale), Needs repair (audit-damaged), Failed reason + Retry, and
  Remove. The shelf adds an "Available offline" filter and a ⤓ badge on
  installed albums, and renders downloaded albums from stored snapshots
  while offline. `/settings` exposes the playback-quality preference and
  per-album storage management (`SettingsPage.svelte`).

## Audio preference & settings

The Phase-4 preference mechanism now has its user-facing half:
`/settings` → *Playback quality* offers Automatic / Prefer lossless /
FLAC-only / WAV-only / AIFF-only — a direct binding onto the one existing
`AudioPreference` union (`musicpack.audio-preference.v1`), so selection,
fallback and persistence semantics are exactly as documented under
"Audio representations" above. Changing the preference affects future item
construction only. *Downloads & storage* on the same page lists installed
albums with sizes, supports removal, and reports quota/persistence status
(including the browser-may-evict caveat when persistence was denied).

The player bar and mobile player label what is sounding — e.g.
`FLAC · 48 kHz · stereo` or `MPC` — derived from the selected
representation's probed metadata plus manifest labels. Bit depth and
encoder quality are not part of `.mpack` v1 metadata and are never shown.

PWA-lite: `manifest.json` + PNG icons complete the static-shell service
worker's precache list (sw.js VERSION bumps whenever that list changes);
the app remains installable-but-plain — there is no offline-sync machinery
beyond the package downloads described above.

## Crossfade (opt-in)

The ⤡ button in the player bar cycles Off → 4 s → 8 s → 12 s (persisted in
the session snapshot; default off, so playback is unchanged when disabled).
It applies only at natural track boundaries — never repeat-one, never a
single-track repeat-all loop, never manual skips or seeks.

- **Native lane** (FLAC etc.): the standby element overlaps the current one
  with equal-power ramps on per-slot gain nodes (AudioParam-scheduled).
  Element timing is approximate.
- **Musepack lane** (Phase B): overlap-add mixing inside the PCM worklet.
  The next track's decoder is pumped into a dedicated crossfade lane ring;
  both lanes stream under per-lane credit backpressure while a cosine/sine
  ramp pair mixes them in the render callback. When the window elapses the
  incoming ring becomes the output ring — with its playhead rebased to the
  outgoing count, so positions, seeking and end detection stay exact across
  fades. The trigger fires when the remaining time of the current track
  enters the planned overlap window; because decode is paced by the ring,
  short tracks may fall back to the natural gapless seam (the fade only
  engages while the standby is still open — by construction for 8–12 s
  fades on normal-length music).
- **Sweet Fades (content-aware planning)**: the app feeds the player a
  transition policy derived from each track's waveform envelope. Recordings
  that already separate themselves (trailing silence) keep true gapless
  playback; consecutive album tracks joining at full energy are never faded
  apart; a decayed outro gets an overlap that hugs its decay instead of a
  blind fixed length; and an abrupt loud ending straight into a loud attack
  is hard-cut rather than summed. The ⤡ setting acts as the maximum fade
  length. Without envelope data the planner degrades to the legacy fixed
  duration.
- **Known limitation:** tracks shorter than the fade window, or seeks that
  land within a second of a boundary, may fall back to the natural gapless
  seam instead of fading. The historical short-track progression stall was
  root-caused (a lost decode-pump credit) and fixed; boundaries now advance
  exactly once even across rapid sub-fade-length tracks.
- **Deterministic end-of-track queue jump (fixed):** with crossfade OFF, a
  queue whose future items carried no `durationHintSeconds` collapsed every
  later track's offset onto the current track's end; a single rendered tick
  past that instant then reported the LAST such index and playback jumped
  from song 1 straight to the last song. Two layers address it: player-core
  caps the tick catch-up at one proven step (d23e8ad), and restored
  snapshots are healed by `repairingStorage()` — missing/invalid hints are
  re-derived from the persisted item's own `track.duration`, never
  invented. Snapshots persisted before the player-core extraction (bare
  `{track,...}` items) are rejected at restore instead of being
  resurrected as unplayable entries. Two further boundary-ownership windows
  were closed in the same family: rendered ticks during a pending
  previous()/next() engine re-open now stand down (the old engine still
  sounds at higher offsets, so backward skips no longer bounce forward
  through the tracklist), and ticks inside the eos standby-promotion gap
  stand down (the cursor advance can no longer be stolen, so natural
  boundaries never skip the next track). Follow-ups: server-side ingest
  duration backfill for legacy packages (opt-in migration, header-only);
  audit of hint sample-rate scaling for non-device-rate sources.

## BS.1770 loudness normalization

Client playback policy (the `.mpack` values are never modified):

- Off / Album / Track, default **Album**.
- Target **−16 LUFS**; gain = target − measured, capped so the output true
  peak never exceeds **−1 dBTP**.
- Output gain = user volume × normalization gain (kept separate, combined in
  the linear domain in one `GainNode`).
