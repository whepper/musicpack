# Representation Selection — Phase 4

Phase 3 gave packages, servers and the API multiple audio representations
per track (`track.representations[]`, `GET …/representations/{rid}/audio`)
with selection deliberately pinned at default-only. Phase 4 turns that
boundary into a deterministic selection policy. The scope decision is the
same one-shot rule as every codec change here: **selection lives at the
web/domain boundary (`itemForTrack()`); Player Core stays
representation-blind.**

## Product decisions (resolved)

1. **Lossless mode ships now** — "prefer a playable lossless representation
   when one exists"; a preference, not a guarantee. Closed set:
   `flac`, `wav`, `aiff`.
2. **Manifest order is the only tie-break.** It is canonical, stable, and
   needs no probing; size never decides.
3. **An alternate may rescue an unplayable primary** (musepack unavailable →
   playable FLAC → use FLAC). A playable primary always wins.
4. **The preference persists** in a dedicated web-app localStorage key
   (`musicpack.audio-preference.v1`). The player-core snapshot schema is
   untouched; representation preferences never enter player-core state.
5. **Representation-aware identity**: non-default items are
   `t{trackId}r{representationId}` so one track can legitimately appear as
   distinct PlaybackItems. Default keeps plain `t{trackId}`.
6. **No UI in this phase.** Policy + persistence plumbing only.

## Model

One mechanism, one discriminated union:

```ts
type AudioPreference =
  | { mode: 'default' }
  | { mode: 'representation'; id: number }
  | { mode: 'codec'; codec: string }
  | { mode: 'lossless' };
```

`resolveAudio(track, pref, canPlay)` (in
`web/app/src/lib/state/representation-selection.ts`) is pure, total,
DOM-free and Node-testable. Playability is an injected predicate; the web
host feeds it from `lib/playback/capability.ts` — the single place that
knows browser codec support, shared with backend resolution so selection and
playback can never disagree.

## Algorithm

Candidates are the track's representations in manifest order.

1. undefined/`default` → step 5.
2. `representation`: first candidate with that id, if playable; else step 5.
3. `codec`: first playable candidate with that codec (case-insensitive);
   else step 5.
4. `lossless`: first playable candidate in the lossless set; else step 5.
5. Primary with playable-or-absent codec → primary; else first playable
   candidate (rescue — unreachable for pre-representation tracks); else
   primary anyway: the item is still built and today's unsupported-format
   error surfaces at engine open. `itemForTrack()` never throws or skips.

Selection happens only at item construction. Changing the preference never
restarts or rebuilds playing/built items.

## Host-side robustness fix

Mixed queues exposed a latent NativeBackend issue: preparing the next queue
item loads its bytes into a standby `<audio>` element, and when the next
item's codec isn't browser-decodable (e.g. musepack following a FLAC
alternate) the standby's error fired the backend's fatal error path —
killing healthy playback of the current track. Standby load failures now stay
non-fatal: `metadata()` rejects, `prepareNext()` returns null, and the
core's existing boundary recovery fresh-loads the target with the correct
engine. Ownership failures (current/faded-in lanes) remain fatal.

## Compatibility contract

- No preference ⇒ byte-identical pre-Phase-4 behavior (pinned by the Phase 3
  guard test, unmodified).
- Tracks without representations behave identically under every preference.
- Snapshot restore replays persisted sources regardless of later preference
  changes; old snapshots restore unchanged.

## Future compatibility

Offline playback can reuse the resolver by injecting local-availability as
`canPlay`; `QueueItem.representationId` records the resolution for a future
downloader. Treat URL/content-sha — not row id — as content identity when
offline layers arrive (ids are stable only while `(track,path)` is
unchanged). Deliberately out of scope here: downloads/offline storage,
transcoding, adaptive streaming, per-track preference maps, UI.
