# Real-collection validation (Phase 7B)

This document records the Phase 7B validation of MusicPack against a real
music collection. The goal was to answer a single question:

> Does MusicPack already feel like a usable, compelling digital record shelf
> when populated with real albums?

No copyrighted audio or generated `.mpack` packages were committed; the
validation ran entirely in a scratch tree. All source albums were left
untouched (verified per package: `sources_unchanged = true`).

## Real collection tested

13 authored `.mpack` packages from a private FLAC collection (74 artists
available; 13 chosen for variety), all encoded FLAC → Musepack SV8 q6:

| Album | Coverage |
|---|---|
| Daft Punk — Random Access Memories | single disc, 13 tracks, gapless, rich tags, embedded art |
| Rush — 2112 | 6 tracks incl. a 20-minute medley, single disc |
| The National — Boxer | single disc, 12 tracks |
| Massive Attack — Mezzanine | single disc, per-track guest artist (Horace Andy) |
| Metallica — S&M2 | 2 discs (10+12), live with orchestra, wide loudness spread |
| Stevie Wonder — Songs in the Key of Life | 2 discs, classic |
| U2 — The Best Of 1990-2000 & B-Sides | 2-disc compilation with B-sides |
| The Cure — Disintegration (Remastered) | remaster edition, 72 min |
| Joy Division — Unknown Pleasures (2019 Digital Master) | edition #1 |
| Joy Division — Unknown Pleasures (1979 UK original) | edition #2, same release group |
| Rowwen Hèze — Andere Wind (Live) | Limburgish dialect, accented artist name |
| La Oreja de Van Gogh — Esencial | 2-disc Spanish compilation |
| Fleet Foxes — Live on Boston Harbor | 27 tracks, 125 min, live |
| André Hazes — De Hazes 100 | 100 tracks / 5 discs, Dutch box set |

**14 packages, 14 verified clean, 0 invalid.** No existing `.mpc` sources were
present in the collection, so every package exercised the FLAC → q6 encode
path. The collection contained no same-album edition pair; the two-edition
release group was constructed with **genuine MusicBrainz identities** (the
real 1979 and 2019 Digital Master releases of *Unknown Pleasures*, one
release-group ID) rather than fabricated metadata.

## Author experience

Driven through the exact backend pipeline MusicPack Author invokes
(`inspect` → `validate-draft` → `encode-draft` → `build-draft` → `verify`),
with the GUI layer documented as not click-tested.

Worked well:

- `inspect` captured rich tag metadata end to end: album/album artist/track
  title/artists+composers/ISRC/genre/date/label/barcode and Deezer source IDs,
  disc grouping, codec/sample-rate/duration probes, and artwork (external
  `cover.jpg` and embedded FLAC art, extracted to `artwork/front.jpg`).
- `validate-draft` separated errors from warnings (e.g. "missing release
  identity" warnings) and build-draft refused invalid drafts.
- `encode-draft` streamed NDJSON progress events and staged `.mpc` files with
  correct `N-01 Title.mpc` naming (multi-disc names include the disc number).
- `build-draft` measured BS.1770-5 loudness and built verified packages;
  `verify` reported 0 errors/warnings on every package.
- Sources were never modified; package finalization was transactional.

Found and fixed:

- **Uppercase disc directories (`CD1`/`CD2`) were silently ignored**, so every
  real multi-disc album failed `inspect` ("no audio files found"). The Deemix
  collection convention is `CD1`/`CD2`; the scanner only recognized lowercase
  `disc-*`/`cd *`. See [Issue 1](#issue-1-blocker-multi-disc-albums-with-cd1cd2-directories-were-skipped).
- **Live MusicBrainz identification did not fetch the release group**, so two
  editions identified via `identify-draft --mbid` would never group under one
  album. See [Issue 2](#issue-2-important-live-musicbrainz-identification-missed-the-release-group).
- **Live MusicBrainz label data was ignored** (`label-info` vs legacy
  `labels`). See [Issue 3](#issue-3-important-live-musicbrainz-labelcatalogue-was-not-applied).

## Metadata preservation

FLAC → `.mpc` APEv2 → `.mpack` manifest survived cleanly (checked on every
package):

- `.mpc` APEv2: Album, Album Artist, Genre, Year, Label, Barcode, Source,
  Title, Artist, Composer, Track `N/12`, Disc `N/N`, ISRC, SourceId, plus
  passthrough (LYRICS, COPYRIGHT, ITUNESADVISORY).
- Manifest: album title/artists/genres, release date/label, barcode, per-track
  artists+composers, ISRC, source store/id, duration, BS.1770 track/album
  loudness, audio sha256.
- Confirmed field ownership: source/store provenance → `source`; release
  identity (date/label/catalogue/country) → `release`; MusicBrainz IDs →
  `identifiers` + `identity.confidence`; audio-derived data → track `loudness`.
  No redundant duplication introduced.

One source-data note (not a MusicPack defect): Deezer writes the *original*
release date (`DATE=1979`) even on the 2019 Digital Master, so both Unknown
Pleasures editions carried `1979-06-14`. The Author GUI's title/edition
editing is the intended way to normalize group title vs edition (we set
group title "Unknown Pleasures", editions "2019 Digital Master" / "1979 UK
original").

## Package validation

14/14 authored packages pass full `musicpack verify` (0 errors, 0 warnings),
all referenced asset hashes verified, media/track ordering canonical,
BS.1770-5 loudness present (album LUFS range −8.0 … −13.4; the live S&M2
spread −21.8 … −9.7 dBTP is a realistic quiet/loud test case). Sonic analysis
was not exercised (requires an ONNX Runtime build; optional).

## Server / library behavior

Deployed through the Phase 7A workflow (`docs/deployment.md`): staged in
`incoming/`, moved into `library/`, `scan`, `verify`, `serve`.

- 14 packages → **13 release groups** (Unknown Pleasures grouped correctly
  with 2 editions, no flattening, no false conflicts).
- Editions distinct: `2019 Digital Master` (XE) vs `1979 UK original` (GB),
  both `valid/valid`, 10 tracks each.
- Multi-disc preserved: S&M2 = disc 1 (10) + disc 2 (12), correct ordering.
- `scan` + `verify`: idempotent, 0 invalid; the identity-less first JD package
  was replaced and re-verified cleanly.
- Move/remove/re-add and re-scan were exercised implicitly during library
  reassembly without stale or duplicate records.
- API: `GET /api/v1/albums`, `/albums/{id}`, `/releases/{id}` returned correct
  grouping, edition, media and status data.

## Web experience

Validated in a real Chromium (Playwright) against the live server + built
client, 9 checks + the repo's 13-test e2e suite:

- Shelf: 13 albums, no duplicates, search narrows server-side, collector
  lines ("2 versions"), artwork loads with **no broken images**.
- Album page: release information panel (codec `musepack-sv8`, BS.1770
  loudness, label, release date), edition chips (2 editions distinct with
  different tracklists), multi-disc album page shows both discs.
- Playback: `Play album` starts demand-driven; seek to 90%/5% with position
  tracking; **served bytes stayed far below the full file**; pause/resume and
  next track behave; Media Session metadata present; the session cookie
  survives reload (no re-sign-in).
- Mobile/narrow-browser layout was not exercised (Desktop Chrome only).

## Playback

- Real Musepack streaming: demand-driven WASM reader, `Range` 206 with exact
  `Content-Range` bytes, strong ETag → `304` on `If-None-Match`.
- **Gapless:** the exact technical harness (`web/tests/node/wasm-gapless.mjs`)
  run on two consecutive real RAM tracks verified frame-identical A+B output
  (no dropped/duplicated/inserted-silence frames), exact track-end frames,
  and seek accounting that never downloaded the whole file
  (712 KB fetched of 15.2 MB). Browser-native FLAC gapless is documented as
  platform-limited (no sample-perfect API), unchanged.
- Queue, seek latency, and long-track playback behaved; no truncation or
  audible corruption observed.

## Loudness

BS.1770-5 album normalization (default −16 LUFS, true-peak capped) applied in
the browser (`normDb < 0` asserted during playback). Per-album gains are
uniform (album mode), so quiet/loud masters (S&M2 vs Songs in the Key of
Life) do not jump within an album. Package loudness values are never modified
by the client.

## Multi-disc / compilation / Unicode

- Multi-disc: 5 albums (2–5 discs) authored and served; discs grouped,
  numbering resets per disc, disc order canonical.
- Compilations: U2 *Best Of*, La Oreja *Esencial*, Hazes 100 — album artist vs
  per-track artists preserved (Mezzanine's Horace Andy credit tested).
- Unicode: Rowwen Hèze / André Hazes accents, Spanish titles, `&` and `;`
  characters round-tripped Author → manifest → server DB → API → web client
  without corruption.

## Issues fixed

**Issue 1 — BLOCKER: multi-disc albums with `CD1`/`CD2` directories were skipped.**
`musicpack/main.c` `disc_from_dirname()` matched only lowercase `disc`/`cd` prefixes.
Real collections (Deezer/Deemix) use uppercase `CD1`/`CD2`, so every multi-disc
album failed `inspect`. Fixed with ASCII case-insensitive prefix matching
(`CD1`, `Disc 1`, `disc-2`, `CD 2` all recognized; non-disc dirs like
`Discography` still ignored). Regression test: `tests/run_author.sh`
("inspect recognizes uppercase CD1/CD2 disc directories") using the committed
Neon Skyline fixture copied to `CD1`/`CD2`.

**Issue 2 — IMPORTANT: live MusicBrainz identification missed the release group.**
`identify-draft --mbid` fetched
`/ws/2/release/{id}?inc=artist-credits+labels+recordings+media` — the current
MusicBrainz API does not include the `release-group` object without
`inc=release-groups`, so two editions identified via live MBID never shared a
release group (they became separate albums). Fixed by adding
`+release-groups` to the lookup. Regression test: new fixture
`tests/reference/meta/mb-release-live-shape.json` (live response shape) with
assertions in `tests/run_author.sh`.

**Issue 3 — IMPORTANT: live MusicBrainz label/catalogue was not applied.**
`musicpack_mb_apply_release()` read the legacy `labels` array; the live API
returns `label-info` (nested `label` object), so MB label and catalogue number
were silently dropped from live lookups. Fixed by reading `label-info` with a
`labels` fallback. Covered by the same regression test (asserts label
"Example Records" and catalogue "ERCD 001" from `label-info`).

## Remaining issues

- **Sonic analysis** not exercised (requires the ONNX Runtime-backed
  `musicpack-sonic` build; optional by design).
- **MusicPack Author GUI** not click-tested end to end; validated through the
  exact backend pipeline it invokes.
- **Mobile/narrow-browser layout** not validated (Desktop Chrome only).
- **`build-draft`/`encode-draft` with a relative or non-existent output
  parent** report a misleading "source and output directories overlap" error
  (the canonicalization failure is conflated with an actual overlap). The GUI
  pre-creates output parents, so this is CLI-only polish.
- **Media format** is empty for packages whose sources carry no format tag
  (Deezer FLACs) — a metadata-source limitation, not a defect.
- **Release date provenance** for remaster/digital-master releases relies on
  the source tag (Deezer writes the original date); a collector normalizes
  this in Author's edition field.
- Two albums showed anomalously long encode/build wall times in one batch run
  (34/45 min); direct timing showed ~360× real-time encoding (an 8-minute
  track in ~1 s), so this was system contention, not a reproducible defect.

## Regression coverage

- `tests/run_author.sh` — CD1/CD2 recognition; live-MB-shape identification
  (label-info + release-group). Part of the `author_backend` CTest suite.
- No codec, `.mpack` v1, or server/security behavior changed.

## Validation evidence

- CTest: `mpack`, `mpack_conformance`, `mpack_integration`, `mpack_fuzz`,
  `mpack_hostile`, `server_unit`, `server_integration`, `author_backend`,
  `enc_compat` all pass (25/26; the lone `compat` failure is the documented
  Release-vs-`-O0`-manifest local case, unrelated to these changes — CI runs
  it in live mode).
- Web: `vitest run` 48/48; repo Playwright e2e 13/13; Phase 7B real-collection
  Playwright checks 9/9.
- Wasm: `wasm_smoke` + `web_wasm_gapless` pass; the gapless harness on real
  RAM tracks PASS.
- `git diff --check` clean.
