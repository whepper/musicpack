# MusicPack UI v2 — Design System

Status: **complete and approved** (phases P0–P7, 2026-08). This document is the
normative reference for the MusicPack visual language as implemented in
`web/app` (the consumer record-shelf client) and `author/app` (MusicPack
Author). Both apps ship one token block and one component grammar; where they
deliberately diverge, the divergence is documented here (§12).

The governing product principle from the v2 brief, unchanged:

> A beautiful digital record collection where every album is a rich, portable
> object. Not a web player with a technical database attached to it.

Author restates the same principle for its mode: *a curated release-editor
where every package fact is inspectable — not a debug console.*

## 1. Scope and sources

- Consumer client: `web/app` (Svelte 5 + Vite SPA, no SSR, no UI framework).
  Global stylesheet `web/app/src/lib/ui/theme.css`.
- Author: `author/app` (Svelte 5 + Tauri 2). Global stylesheet
  `author/app/src/lib/ui/theme.css` carries an identical `:root` token block.
- The capability→UI map (§10) is limited to what API v1 and the `.mpack`
  model actually expose; the known gaps are recorded in §10 and §16.
- Verification gates and contracts: `web/tests/e2e/` (Playwright),
  `web/tests/unit/`, `author/tests/`, `npm run check` in both apps.

## 2. Design tokens

One shared token set, value-identical in both apps (each app additionally
carries only its own layout locals: the consumer adds `--sidebar-w` and
`--player-height` for the shell and player bar):

```css
/* neutral near-black; slightly warm cream text; gold accent; green status */
--bg:            #0f1013;  /* page canvas                      */
--bg-rail:       #0a0b0d;  /* sidebar / deeper chrome          */
--surface:       #16171b;  /* cards, rail panels, player, drawer */
--surface-2:     #1e2025;  /* hover/active elevation, tiles    */

--text:          #f1eee7;  /* warm cream                       */
--text-soft:     #b0aca3;  /* secondary                        */
--text-faint:    #8a8781;  /* micro-copy only                  */

--accent:        #d9a856;  /* gold — selection, active, emphasis */
--accent-strong: #e6c37a;  /* hover / focus                     */
--accent-ink:    #1a150b;  /* text/icons placed on gold         */
--ok:            #63b97c;  /* verified / valid / installed only */
--danger:        #e07a6a;
--focus:         #e6c37a;

--hairline:        rgba(241, 238, 231, 0.10);
--hairline-strong: rgba(241, 238, 231, 0.22);
```

Type stacks are system-only (no webfont downloads): `--serif` (Iowan Old
Style → Palatino → Georgia), `--sans` (system-ui), `--mono` (SF Mono/Menlo).
Font sizes `--fs-xxs…--fs-2xl` plus `--fs-display: clamp(2.2rem, 4.5vw,
3.4rem)`; spacing `--space-1…--space-7` (4→48 px); radii `--radius` 4 px /
`--radius-card` 10 px; `color-scheme: dark`.

**Depth law.** Depth comes from tone steps (`--bg` → `--surface` →
`--surface-2`) and hairlines. Exactly one shadow is allowed in the system,
`--shadow-artwork`, and only beneath artwork. No gradients, no
glassmorphism, no dashboard chrome.

## 3. Typography hierarchy

| Voice | Treatment | Used by |
|---|---|---|
| Display | serif, `--fs-display`, letter-spacing −0.015em | Album / track / now-playing titles |
| Page title | serif, `--fs-2xl` | Shelf, Artists, Search, Settings |
| Section heading | serif `--fs-lg`, or small-caps `--fs-xxs` + letterspacing 0.1em + hairline underline rail | Album/track sections, rail panel headers |
| Eyebrow | sans, `--fs-xxs`, letterspacing 0.14em, uppercase, `--text-faint` | "The collection", "Now playing", context lines |
| Body | sans, `--fs-md`/`--fs-sm`, line-height 1.5 | prose, form labels |
| Micro | sans, `--fs-xs`/`--fs-xxs`, `--text-faint` | filenames, meta, captions |
| Tabular numerals | `font-variant-numeric: tabular-nums` | times, durations, LUFS/dBTP, table columns |
| Mono | `--mono` | hashes/IDs (consumer); also working filenames/paths (Author) |

Small-caps is a *voice*, not a component: `.smallcaps` / `.eyebrow` are the
shared classes; uppercase + letterspacing is never faked with real small-caps
glyphs.

## 4. Colour and status semantics

- **Gold (`--accent`) = selection, active state, emphasis, and the single
  primary action.** Active nav row, tab underline, playing-format marker,
  filled primary `.btn`, progress fills (seek/volume), inline links.
- **Green (`--ok`) = a genuine verified/valid/installed assertion only.**
  It is never decoration and never "brand green". The mapping is explicit
  and fail-closed (§11): unknown server strings render neutral.
- **`--danger` = errors and damaged states**, always with text (never
  colour-only).
- **Text tiers**: `--text` primary → `--text-soft` secondary →
  `--text-faint` micro-copy only, never for body text.
- Hover/focus on interactive elements moves to `--accent-strong`; focus
  uses the global `:focus-visible` gold outline.

## 5. Component grammar (shared)

| Primitive (consumer file) | Grammar |
|---|---|
| `.btn` / `.btn.ghost` | pill, 10×22 padding; primary = gold fill + `--accent-ink` text; ghost = transparent, text-coloured, hairline border |
| `StatusChip.svelte` + `status.ts` | small-caps outline pill; tones `ok` (green text + 45% green border), `warn` (gold), `bad` (danger), muted (base). Optional leading ✓ glyph. **No filled backgrounds** |
| `InfoTile.svelte` / `.info-tiles` | quiet stat card, capped width, `auto-fit`; codec facts only (§10) |
| `InfoGrid.svelte` | key/value grid: small-caps `dt`, `fs-sm` `dd`; undefined values are omitted, never faked |
| `HashLine.svelte` | truncated mono sha256 (`first8…last6`), click-to-copy with `role="status"` confirmation, full value in `title` |
| `WaveformSeek.svelte` | canvas waveform seek with a visually-hidden `input[type=range]` as the keyboard/ARIA surface; linear range fallback when no envelope |
| `WaveformSpark.svelte` | static envelope renderer; row sparklines lazy-fetch on visibility + shared LRU |
| `SectionTabs.svelte` | `role=tablist`, uppercase letterspaced labels, active = `--text` + 2px gold underline, arrow-key navigation |
| `EditionSelector.svelte` | chips with artwork thumbnails |
| `TopBar.svelte` | breadcrumb + search field |
| `AppSidebar.svelte` | `--bg-rail`; small-caps group labels; active row = surface pill + gold; collapses to icon rail ≤1024px, off-canvas ≤680px |
| Search field | one shared pill grammar (`--surface`, faint magnifier, gold `:focus-within`): top bar, shelf/artist search and the search page |
| Tables | hairline row separators, small-caps headers, right-aligned tabular numerics, active row tinted with gold index glyph |
| `ErrorView.svelte` | empty-state frame in the reading voice (`role=alert`) |
| Cards | `--surface`, `--radius-card`, 1px hairline; boundaries only where interaction needs one |

The fixed bottom **player bar** (desktop, ≥681px) and **mobile player**
(≤680px) share the seek grammar; queue affordances (`QueueDrawer`/`QueueList`)
are identical between surfaces.

## 6. Consumer shell: the three-zone layout

```text
┌──────────┬──────────────────────────────┬───────────┐
│ sidebar  │ top bar (breadcrumb + search)│           │
│ (rail)   ├──────────────────────────────┤  context  │
│          │        main column           │   rail    │
│          │                              │ (album/   │
│ settings │                              │  track)   │
│ /account │                              │           │
├──────────┴──────────────────────────────┴───────────┤
│                 player bar (≥681px)                 │
└─────────────────────────────────────────────────────┘
```

Sidebar items are honest only — Library, Artists, Search, Now Playing,
Settings (+ offline state and sign-out at the bottom). No Playlists,
Favorites or speculative destinations exist, so none are shown. Routes:
`/` shelf · `/albums/:id` (+`?release=N&section=S`) · `/tracks/:id` ·
`/search?q=` · `/artists`, `/artists/:id` · `/queue` · `/settings`.

## 7. Album / Edition: seven-section IA

The album page (`/albums/:id`) is the consumer's primary reference screen;
this layout is the canonical visual reference other screens match.

Hero (artwork + serif title + artist link + `year · label · cat#` +
`country · medium · edition` + badges + actions + info tiles) → **tabs**,
each a `?section=` deep link:

1. **Overview** — simple disc-grouped track list + one-line audio summary.
2. **Tracks** — technical table: `# / Title / Disc / Duration / Peak / LUFS /
   Waveform / detail / add` (TrackTable), responsive column budget §13.
3. **Edition** — artwork-thumb edition chips (never flattened), full release
   facts, identity confidence, "N editions".
4. **Audio** — `RepresentationTable` cards (primary + alternates) with
   Play/Set-preference actions that go through the real selection policy.
5. **Analysis** — album loudness block (algorithm / LUFS / dBTP + derived
   normalization-gain preview via player-core constants), per-track loudness
   table, waveform previews.
6. **Metadata** — credits/roles, MusicBrainz IDs, ISRC, source.
7. **Package** — StatusChips, integrity summary, asset inventory, structure
   counts, provenance, offline state, HashLines.

The right **contextual rail** (`ContextRail` + Edition/Audio/
Representations/Analysis/Package panels) is a glanceable summary of the same
sections; every panel deep-links into its section (`?section=…`). Query-only
section switches never remount the page or scroll-jump.

## 8. Track detail and the technical inspector

`/tracks/:id` (`TrackPage`) is editorial first, technical second, in one
scroll: hero (artwork from the owning release, album/edition eyebrow link,
serif title, disc/track position, duration · compact codec) → playback
actions (Play track / Shuffle album / Play alternate / Add to Queue /
Download) → seek control (waveform or range fallback) → info tiles → Audio
(codec, streamVersion, sample rate, channels, size, audio SHA-256) →
Analysis (track + album LUFS/dBTP, normalization-gain preview, waveform meta)
→ Metadata (album, edition, ISRC and MBIDs *when present*) → Package
(structure counts, verification chips, link to the full `?section=package`).

A contextual inspector rail (Album / Audio / Analysis / Package summaries,
each deep-linking to its album section) mirrors the album-page rail. Every
value originates from `GET /tracks/:id` (track + `context`) plus the owning
release detail refreshed through the library; nothing is synthesized.

## 9. Player / Now Playing / Queue relationship

Three surfaces, one player-core, one set of facts:

- **Player bar** (desktop) — artwork, `artist · album · edition`,
  playing-format line (e.g. `MPC` or `FLAC · 48 kHz · stereo`), ghost
  transport around the gold circular play/pause, waveform seek, thin gold
  progress, then repeat/shuffle/crossfade/normalization/volume/queue with
  spelled-out labels.
- **Now Playing** (`/queue`) — the same information at reading scale:
  artwork hero, display-serif title, within-track time + waveform, format +
  normalization line, transport, queue below.
- **Queue drawer** — the live queue (`QueueList`) shared verbatim with the
  Now Playing page; `aria-current` marks the playing item; move/remove
  controls keep their labels.

The compact bar, expanded page and drawer never disagree because they render
the same `playerModel` state; representation selection and playback policy
live in the state layer, not the surfaces.

## 10. `.mpack` data-display rules

**Rule: real API/package data only.** A value appears only when the API or
probed metadata carries it; absent values are omitted or shown as an honest
"unknown", never inferred, averaged or invented.

Shown whenever (and only when) present — codec label, `streamVersion`,
sample rate, channels, file size, SHA-256, BS.1770 LUFS, true peak dBTP,
duration, waveform interval/encoding/points/floor, edition/label/catalogue/
country, MBIDs, ISRC, source/provenance fields, package/verify status,
representation labels.

**Never shown** (not part of `.mpack` v1 metadata, so not rendered —
the rule from `qualityLine()` extended system-wide):

- bitrate ("128 kbps" etc.) for any codec
- bit depth for Musepack (and any lossy codec); lossless depth only if a
  manifest label literally states it
- Loudness Range (LRA)
- replaygain tags as canonical loudness
- "Mastered by" credits, play counts, favorites, genre scores
- collection aggregates (size, codec share) — see gaps below

**Capability map** (all shipped unless noted):

| Capability | Destination | Source |
|---|---|---|
| Album / release-group identity | Album hero, Metadata, rail | `albums/:id` |
| Release / edition identity | Edition chips, Edition section, rail | `releases[]` |
| Artwork & assets | Hero, artwork viewer, Package | `artwork[]`, `assets[]` |
| Track metadata | Overview, Tracks, Track detail | track object |
| Disc structure | TrackTable + disc disclosures | `media[]` |
| MusicBrainz / ISRC | Metadata, Track detail (present-only) | `mbid`, `isrc` |
| Audio representations | Audio section, rail, Track detail | `codec`, `representations[]` |
| Codec facts | Audio grid, player format line | `codec.*` |
| BS.1770 loudness | Analysis section, rail, normalization line | `loudness` |
| Waveform | Sparklines, seek control, Analysis, Track detail | `waveform` |
| Package integrity | StatusChips, Package section | `packageStatus`, `verifyStatus` |
| SHA-256 | HashLines (audio/rep/waveform/artwork) | `sha256` refs |
| Provenance & source | Metadata + Package sections | release fields |
| Sonic analysis | **not displayed** — authoring-only, no API yet | — |
| Manifest document | **not served** — API-exposed facts only | — |
| Collection aggregates | album count only; size/codec-share **not faked** | browse `total` |

Known honest gaps (recorded, not built): a sonic-analysis API, a manifest
document endpoint, and an aggregate collection-stats API. No consumer surface
shows sonic analysis at all; there is no rail slot until an API exposes the
data.

## 11. Verification / status semantics

Chip labels map **explicitly** to the server's value vocabulary
(`server/src/scanner.c` / API); unmapped values fall through to a neutral
chip carrying the raw string — never a fabricated green:

- `packageStatus`: `valid`→*Valid* (ok) · `warning`→*Warning* (warn) ·
  `checksum-failed`→*Checksum failed* (bad) · `conflict`→*Conflict* (bad)
- `verifyStatus`: `valid`→*✓ Verified* (ok) · `warning`→*Verification
  warning* · `unverified`→*Unverified* (muted) · `checksum-failed` (bad)
- identity confidence: `exact`→*✓ Exact match* · `confirmed`→*Confirmed* ·
  `probable`→*Probable match* (muted) · `none`→*Unidentified* (muted)
- client-side offline states: `installed`→*Offline ready* ·
  `stale`→*Update available* · `damaged`→*Needs repair* · `failed`→
  *Download failed*
- Author completeness chips: green ✓ only for states the package genuinely
  satisfies; *Loudness · at build* is muted by definition (measured at
  build); identity badges follow the same exact/confirmed → green,
  probable/none → neutral mapping.

## 12. Consumer vs Author — deliberate differences

Same tokens, controls and semantics; different information architecture,
because the jobs differ (browsing a finished collection vs producing one).

| Aspect | Consumer | Author | Why |
|---|---|---|---|
| Navigation | sidebar + breadcrumb + contextual rail | slim top bar + sticky **8-stage stepper** | creation is sequential; browsing is not |
| Stage/tab labels | small-caps section tabs | title-case, larger targets carrying ✓/!/· marks | the nav doubles as a status surface |
| Footer | player bar (transport) | status bar: package-completeness chips + Validate / Create MusicPack | primary work state must persist across stages |
| Technical data | one tab away (`?section=…`, rail summary) | first-class inline (validation lists, candidate rows, filenames, hashes) | here, technical data *is* the work |
| Forms | none (read-only editorial) | label-over-input `.field` wizard, native dark controls | editing surface |
| Mono type | hashes/IDs | also working filenames, source roots, output paths | operational identifiers |
| Artwork | hero + viewer | 112px identity thumbnail | authoring focus is the package |
| Skip link | present | none (single-purpose window) | |
| Stages | — | Identity · Release · Tracks · Artwork · Encode · Sonic · Waveform · Validate (`authoring-state.ts STAGES`) | |

The stepper's stage set is the UI's workflow IA; the "Current workflow" list
in `author/README.md` describes the user-visible journey (Add album → … →
Create MusicPack), which includes the Welcome and Create steps that live
outside the stepper. The two orderings intentionally differ; the UI stepper
above is authoritative for stage order.

## 13. Responsive design principles

- Breakpoints: **≤1024px** contextual rail stacks below main content (and the
  sidebar becomes an icon rail from 1024 down to 680); **≤680px** sidebar goes
  off-canvas (backdrop + disclosure), desktop player bar yields to the mobile
  player, breadcrumbs hide, the search pill flexes; **≤520px** shelf grid
  becomes two columns.
- No horizontal page overflow at 1440/1024/768/681/680/520/390/360/320 (kept
  as an invariant; measured in review).
- Wide technical tables scroll *inside* their own container
  (`.table-scroll` + `contain: paint`), never as page overflow. On phones the
  Tracks table trims its budget: waveform column hidden; Disc column hidden
  **only when single-disc**; compact `Time` header; peak unit dropped — every
  hidden fact remains available in the Analysis section, the Package section
  and the track page. Nothing is invented to fill the reclaimed space.
- Fixed drawers/footers wrap their contents (Author status bar actions take
  their own row when the window is narrow) rather than clipping.
- Motion is quiet (1–2px hovers, underline transitions, drawer slide) and
  collapses globally under `prefers-reduced-motion`.

## 14. Accessibility principles

- **Never colour-only**: every status carries text (and often a glyph: ✓, ⚠);
  `aria-current`, `aria-pressed`, `aria-expanded` on nav/tabs/chips.
- Keyboard contracts are first-class: section tabs (`role=tablist` + arrow
  keys), the artwork viewer and queue drawer (`role=dialog`, Escape,
  click-outside), WaveformSeek's hidden range for pointer+keyboard parity,
  spelled-out control labels ("Play album", "Add {title} to queue",
  repeat/shuffle/crossfade/normalization).
- Global `:focus-visible` gold outline; focus never removed without a
  replacement.
- Native semantics over ARIA (lists, tables, fieldsets, labels; search input
  keeps `aria-label="Search the collection"`).
- Contrast policy: body ≥ 7:1, secondary ≥ 4.5:1, `--text-faint` restricted
  to non-essential micro-copy; gold/green ≥ 4.5:1 (verified at P0,
  re-swept at P5).
- CSP keeps `unsafe-inline` only for styles (layout literals) — no
  `unsafe-eval` beyond `wasm-unsafe-eval`.

## 15. Reuse inventory

| Component (consumer) | Role | Reused by |
|---|---|---|
| StatusChip + status.ts | verification vocabulary chips | Album, Track, shelf badges, Settings |
| InfoTile / InfoGrid | stat tiles, key/value facts | Album hero, Track hero, rail panels |
| HashLine | truncated sha + copy | Package, Track detail, representations |
| WaveformSpark / WaveformSeek | envelope render / seek | TrackTable, Analysis, player surfaces, Track page |
| SectionTabs | tablist + `?section=` binding | Album, Track rail contexts |
| Artwork | covers + generated v2 placeholder palette | everywhere artwork appears |
| DownloadControl | offline lifecycle | Album, Track hero |
| Search field pill | `.search-field` grammar | TopBar, SearchBox, Search page |
| `.track-table` / `.table-scroll` | technical table + scroll container | Album Tracks + Analysis sections |

Author re-uses the **vocabulary** (identical tokens, chip tone system,
`.btn`, hairline tables, small-caps labels, mono identifier treatment) as a
port of the grammar in its own stylesheet; the apps share no runtime code,
so parallel CSS classes are the deliberate mechanism (see §12 of the
roadmap decision). Where Author needed tool-specific UI (stepper, candidate
rows, drop zone, validation lists) it grew new components rather than
reshaping consumer ones.

## 16. Known follow-ups

Recorded, none blocking, none faked in the UI:

1. **Tauri application icons** (`author/src-tauri/icons/`) remain the
   v1-derived set; regeneration via `tauri icon` from the v2 mark is a small
   rider. (Consumer PWA icons were repainted to the v2 palette in P5; the
   service-worker VERSION bump is the documented invalidation path.)
2. **Sub-minimum Author widths**: below the declared `minWidth: 760`, the
   wrapped status bar can exceed the footer clearance; supported sizes are
   clean. Accepted degradation.
3. **Suite flake**: `offline.spec.ts` "download once via the UI…" was the
   historical CI-only flake, now root-caused and fixed: its offline seek ran
   on a ~1 s fixture clip, so the slider clamped a 10 s seek to the track
   boundary and queued the auto-advance chain into the album end — a race
   against the reload that CI timing lost. The seek now runs on the 48 s
   track (real OPFS random access), so the persisted session is
   deterministically playable. One `playback.spec.ts` timing assertion still
   flakes under heavy load — test-synchronization, not a production defect
   (see `web/README.md` §E2E/CI reliability notes).
4. **Faint micro-copy** (codec tags, artwork placeholder text) sits at the
   contrast floor by policy; raised only if a real legibility report arrives.
5. **API gaps awaiting product decisions**: sonic-analysis exposure to the
   web API, a manifest-document endpoint, aggregate collection stats
   (§10). The UI renders slots, never estimates.

## 17. Verification contract

The system is kept honest by, and only ships with:

- `npm run check` (svelte-check + tsc) clean in **both** apps;
- unit tests (`web/tests/unit`, `author/tests/unit`) — logic, not snapshots;
- component tests (`author/tests/component`) via role/label queries only;
- the Playwright suite (`web/tests/e2e`, 9 specs) — the selector/ARIA
  contract in §5 and the plan's §7 stays frozen; layout changes that break
  it are rejected;
- production builds + manual desktop/mobile inspection per phase.
