<!--
Copyright (c) 2026, The MusicPack Development Team
SPDX-License-Identifier: BSD-3-Clause
-->

<script lang="ts">
  // Album / edition screen — the primary UI v2 reference. Hero + info
  // tiles + seven deep-linkable sections (Overview / Tracks / Edition /
  // Audio / Analysis / Metadata / Package) and the contextual right rail.
  // Everything renders from the API v1 payloads; nothing is invented.
  import { library, player, playerModel, queue, router, offline, offlineStates, session, audioPreference } from '../bootstrap';
  import type { AudioPreference } from '../state/representation-selection';
  import DownloadControl from './DownloadControl.svelte';
  import Artwork from './Artwork.svelte';
  import EditionSelector from './EditionSelector.svelte';
  import TrackList from './TrackList.svelte';
  import TrackTable from './TrackTable.svelte';
  import SectionTabs from './SectionTabs.svelte';
  import StatusChip from './StatusChip.svelte';
  import InfoTile from './InfoTile.svelte';
  import InfoGrid from './InfoGrid.svelte';
  import HashLine from './HashLine.svelte';
  import ContextRail from './ContextRail.svelte';
  import ErrorView from './ErrorView.svelte';
  import { codecLabel, codecName, countryName, formatDate, formatBytes, fmtTime, mediumLabel, qualityLine, yearOf } from '../format';
  import {
    identityChip,
    offlineStateChip,
    packageStatusChip,
    verifyStatusChip,
  } from './status';
  import { normalizationGainDb } from '../playback/loudness';
  import { tracksOfRelease } from '../state/queue';
  import { onDestroy, onMount } from 'svelte';

  let { albumId, releaseParam }: { albumId: string; releaseParam?: string } = $props();

  let detail = $state<Awaited<ReturnType<typeof library.albumDetail>> | null>(null);
  let rel = $state<Awaited<ReturnType<typeof library.releaseDetail>> | null>(null);
  let error = $state<string | null>(null);
  let loading = $state(true);
  let selectedId = $state(-1);
  let viewerOpen = $state(false);
  // Transient "✓ Added" confirmation for the album-level queue action; the
  // timer is cleared on destroy so no state update fires post-unmount.
  let albumAdded = $state(false);
  let addedTimer: ReturnType<typeof setTimeout> | undefined;
  onDestroy(() => {
    if (addedTimer) clearTimeout(addedTimer);
  });

  const currentTrackId = $derived($playerModel.current?.track.id);
  const routeStore = router.route;
  const sectionParam = $derived($routeStore.query.get('section') ?? 'overview');

  const SECTIONS = [
    { id: 'overview', label: 'Overview' },
    { id: 'tracks', label: 'Tracks' },
    { id: 'edition', label: 'Edition' },
    { id: 'audio', label: 'Audio' },
    { id: 'analysis', label: 'Analysis' },
    { id: 'metadata', label: 'Metadata' },
    { id: 'package', label: 'Package' },
  ];
  const section = $derived(SECTIONS.some((s) => s.id === sectionParam) ? sectionParam : 'overview');

  onMount(() => {
    void load();
  });

  async function load(): Promise<void> {
    loading = true;
    error = null;
    detail = null;
    rel = null;
    try {
      detail = await library.albumDetail(albumId);
      const releases = detail.releases;
      if (releases.length === 0) throw new Error('This album has no playable releases in the collection.');
      const paramId = releaseParam ? Number(releaseParam) : NaN;
      selectedId =
        !Number.isNaN(paramId) && releases.some((r) => r.id === paramId)
          ? paramId
          : library.selectedRelease(detail.album.id, releases);
      library.selectRelease(detail.album.id, selectedId);
      await loadRelease(selectedId);
    } catch (e) {
      error = e instanceof Error ? e.message : 'Could not open this album.';
    } finally {
      loading = false;
    }
  }

  async function loadRelease(id: number): Promise<void> {
    rel = await library.releaseDetail(id);
    // D2 update check (online only): flag a committed download whose
    // hashes no longer match the server. Flagging is all the domain does;
    // replacement stays a user action on the Download control.
    if (offline.enabled && session.get().state === 'authenticated') {
      void offline.checkForUpdate(rel);
    }
  }

  function sectionHref(id: string): string {
    const base = `/albums/${albumId}?release=${selectedId}`;
    return id === 'overview' ? base : `${base}&section=${id}`;
  }

  function setSection(id: string): void {
    router.replace(sectionHref(id));
  }

  async function selectEdition(id: number): Promise<void> {
    selectedId = id;
    library.selectRelease(detail?.album.id ?? 0, id);
    const base = `/albums/${albumId}?release=${id}`;
    router.replace(section === 'overview' ? base : `${base}&section=${section}`);
    await loadRelease(id);
  }

  function playAlbum(): void {
    if (!rel || !detail) return;
    const artist = detail.album.artists[0]?.name ?? '';
    void player.playAlbum(rel, detail.album.title, artist, 0);
  }

  function playWithPreference(preference?: AudioPreference): void {
    if (!rel || !detail) return;
    if (preference) audioPreference.set(preference);
    const artist = detail.album.artists[0]?.name ?? '';
    void player.playAlbum(rel, detail.album.title, artist, 0);
  }

  function shuffleAlbum(): void {
    if (!rel || !detail) return;
    const artist = detail.album.artists[0]?.name ?? '';
    player.setShuffle(true);
    // Host owns randomness (core purity law): pick the shuffled starting
    // track here; the core shuffles only the continuation order.
    const count = tracksOfRelease(rel).length;
    void player.playAlbum(rel, detail.album.title, artist,
      count > 0 ? Math.floor(Math.random() * count) : 0);
  }

  function addToQueue(): void {
    if (!rel || !detail) return;
    const artist = detail.album.artists[0]?.name ?? '';
    queue.addAlbum(rel, detail.album.title, artist);
    albumAdded = true;
    if (addedTimer) clearTimeout(addedTimer);
    addedTimer = setTimeout(() => {
      albumAdded = false;
      addedTimer = undefined;
    }, 2500);
  }

  function playFrom(index: number): void {
    if (!rel || !detail) return;
    const artist = detail.album.artists[0]?.name ?? '';
    void player.playAlbum(rel, detail.album.title, artist, index);
  }

  const heroArt = $derived(rel?.artwork.find((a) => a.role === 'front') ?? rel?.artwork[0]);
  const title = $derived(detail?.album.title ?? '');
  const artist = $derived(detail?.album.artists.map((a) => a.name).join(', ') ?? '');
  const leadArtist = $derived(detail?.album.artists[0]);

  // ---- derived release facts (API values only) -------------------------
  const allTracks = $derived(rel ? rel.media.flatMap((m) => m.tracks) : []);
  const trackCount = $derived(allTracks.length);
  const discCount = $derived(rel?.media.length ?? 0);
  const totalDuration = $derived(allTracks.reduce((n, t) => n + (t.duration ?? 0), 0));
  const primary = $derived(rel?.media[0]?.tracks[0]);
  const primarySize = $derived(
    formatBytes((rel?.media ?? []).reduce((sum, disc) => sum + disc.tracks.reduce((s, t) => s + t.audio.size, 0), 0)),
  );
  const totalSize = $derived(
    formatBytes((rel?.media ?? []).reduce(
      (sum, disc) =>
        sum +
        disc.tracks.reduce(
          (s, t) => s + t.audio.size + (t.representations ?? []).reduce((r, rep) => r + rep.size, 0),
          0,
        ),
      0,
    )),
  );
  const waveCount = $derived(allTracks.filter((t) => t.waveform).length);
  const repCount = $derived(allTracks.reduce((n, t) => n + (t.representations?.length ?? 0), 0));

  interface RepGroup {
    key: string;
    title: string;
    sub: string;
    codec: string;
  }
  const repGroups = $derived.by(() => {
    const seen = new Map<string, RepGroup>();
    for (const track of allTracks) {
      for (const rep of track.representations ?? []) {
        const key = (rep.label ?? rep.codec.codec).toLowerCase();
        seen.set(key, {
          key,
          title: rep.label ?? codecName(rep.codec.codec),
          sub: qualityLine({
            codec: rep.codec.codec,
            sampleRate: rep.codec.sampleRate,
            channels: rep.codec.channels,
          }),
          codec: rep.codec.codec.toLowerCase(),
        });
      }
    }
    return [...seen.values()];
  });

  const isMpc = $derived(
    Boolean(primary && primary.codec.codec.toLowerCase().startsWith('musepack')),
  );
  const primaryTileSub = $derived(
    isMpc && primary?.codec.streamVersion
      ? `SV${primary.codec.streamVersion}`
      : primary
        ? qualityLine({ codec: primary.codec.codec, sampleRate: primary.codec.sampleRate, channels: primary.codec.channels })
        : '',
  );
  const altGroup = $derived(repGroups[0]);

  const identity = $derived(rel ? identityChip(rel.identitySource, rel.identityConfidence) : null);
  const offlineChip = $derived(rel ? offlineStateChip($offlineStates.get(rel.id)?.state) : null);

  const albumGain = $derived(
    rel?.loudness
      ? normalizationGainDb('album', undefined, {
          albumLufs: rel.loudness.albumLufs,
          albumTruePeakDb: rel.loudness.albumTruePeakDb,
        })
      : null,
  );

  // Waveform peek for the rail: the playing track when it belongs to this
  // release, otherwise the first track that carries an envelope.
  const waveTrack = $derived.by(() => {
    if (!rel) return null;
    const current = $playerModel.current?.track;
    if (current && allTracks.some((t) => t.id === current.id)) return current;
    return allTracks.find((t) => t.waveform) ?? null;
  });

  const mediums = $derived(
    rel
      ? rel.media
          .map((m) => mediumLabel(m.format))
          .filter((v, i, a) => a.indexOf(v) === i)
          .join(', ')
      : '',
  );

  const trackIsrcs = $derived(
    allTracks.filter((t) => t.isrc).map((t) => ({ title: t.title, isrc: t.isrc ?? '' })),
  );
</script>

{#if loading}
  <div class="spinner" role="status" aria-label="Loading album"></div>
{:else if error}
  <ErrorView message={error} detail="It may have been removed from the library, or the server is unreachable." />
{:else if detail && rel}
  <div class="album-layout">
    <div class="album-main">
      <article>
        <header class="album-hero">
          <div class="cover">
            <button style="width:100%;padding:0;display:block" onclick={() => (viewerOpen = true)}
              aria-label={`View artwork for ${title}`}>
              <Artwork src={heroArt?.url} alt={`${title} — front cover`} label={title} />
            </button>
          </div>
          <div class="album-heading">
            <p class="eyebrow">{detail.album.releaseType ?? 'album'}</p>
            <h1>{title}</h1>
            {#if leadArtist}
              <p class="artist"><a href={`/artists/${leadArtist.id}`}>{artist}</a></p>
            {:else}
              <p class="artist">{artist}</p>
            {/if}
            {#if detail.album.genres?.length}
              <p class="genre-pills">
                {#each detail.album.genres as genre}
                  <span class="genre-pill">{genre}</span>
                {/each}
              </p>
            {/if}
            <p class="edition-line">
              {#if rel.releaseDate}{yearOf(rel.releaseDate)}{/if}
              {#if rel.label} · {rel.label}{/if}
              {#if rel.catalogueNumber} · {rel.catalogueNumber}{/if}
            </p>
            {#if rel.country || mediums || rel.edition}
              <p class="edition-line">
                {#if rel.country}{countryName(rel.country)}{/if}
                {#if mediums} · {mediums}{/if}
                {#if rel.edition} · {rel.edition}{/if}
              </p>
            {/if}
            {#if identity || rel.sourceType}
              <p class="badge-row">
                {#if identity}<span class="badge">{#if identity.mark}<span aria-hidden="true">{identity.mark}</span>{/if}{identity.label}</span>{/if}
                {#if rel.sourceType}<span class="badge">{rel.sourceType}</span>{/if}
              </p>
            {/if}
            {#if rel.releaseDate || totalDuration > 0}
              <p class="edition-line dates">
                {#if rel.releaseDate}{formatDate(rel.releaseDate)}{/if}
                {#if totalDuration > 0} · {fmtTime(totalDuration)}{/if}
              </p>
            {/if}
            <div class="hero-actions">
              <button class="btn" onclick={playAlbum}>▶ Play album</button>
              <button class="btn ghost" onclick={shuffleAlbum}>⤨ Shuffle</button>
              {#if rel}
                <DownloadControl release={rel} states={offline.states} downloads={offline.downloads} />
              {/if}
              <button
                class="btn ghost album-add"
                aria-label={albumAdded ? 'Album added to queue' : 'Add album to queue'}
                onclick={addToQueue}
              >{albumAdded ? '✓ Added' : '+ Add to Queue'}</button>
              <button class="btn ghost" onclick={() => (viewerOpen = true)}>View artwork</button>
            </div>
          </div>
        </header>

        <div class="info-tiles" role="group" aria-label="Audio summary">
          {#if primary}
            <InfoTile
              title={codecName(primary.codec.codec)}
              sub={primaryTileSub}
              href={sectionHref('audio')}
            />
            {#if altGroup}
              <InfoTile title={altGroup.title} sub={altGroup.sub} href={sectionHref('audio')} />
            {/if}
          {/if}
          <InfoTile
            title={`${trackCount} Tracks`}
            sub={discCount > 1 ? `${discCount} Discs` : 'Single disc'}
            href={sectionHref('tracks')}
          />
        </div>

        <SectionTabs sections={SECTIONS} active={section} onSelect={setSection} />

        <div class="album-section" role="tabpanel" id={`panel-${section}`} aria-labelledby={`tab-${section}`}>
          {#if section === 'overview'}
            <h2 class="smallcaps section-heading">
              {discCount} {discCount === 1 ? 'medium' : 'media'} · {trackCount} tracks
            </h2>
            <TrackList release={rel} currentTrackId={currentTrackId} onPlay={playFrom} />
            {#if primary}
              <p class="muted section-note">
                Plays as {qualityLine({ codec: primary.codec.codec, sampleRate: primary.codec.sampleRate, channels: primary.codec.channels }) || codecLabel(primary.codec.codec)}
                {#if rel.loudness} · album {rel.loudness.albumLufs.toFixed(1)} LUFS{/if}
                — <a href={sectionHref('audio')}>audio details</a>
              </p>
            {/if}
          {:else if section === 'tracks'}
            <TrackTable release={rel} currentTrackId={currentTrackId} onPlay={playFrom} />
          {:else if section === 'edition'}
            <EditionSelector releases={detail.releases} selectedId={selectedId} onSelect={selectEdition} />
            <InfoGrid
              rows={[
                ['Edition', rel.edition],
                ['Release date', formatDate(rel.releaseDate)],
                ['Original release', formatDate(rel.album.originalReleaseDate)],
                ['Country', countryName(rel.country)],
                ['Label', rel.label],
                ['Catalogue number', rel.catalogueNumber],
                ['Barcode', rel.barcode],
                ['Medium', rel.media.map((m) => `Disc ${m.disc}${m.format ? ` (${mediumLabel(m.format)})` : ''}`).join(', ')],
                ['MusicBrainz release group', detail.album.mbid],
                ['MusicBrainz release', rel.mbid],
                ['Editions in collection', detail.releases.length],
              ]}
            />
          {:else if section === 'audio'}
            {#if primary}
              <div class="section-block">
                <h3 class="smallcaps section-heading">How this album plays</h3>
                <p class="muted">
                  Default format {qualityLine({ codec: primary.codec.codec, sampleRate: primary.codec.sampleRate, channels: primary.codec.channels }) || codecLabel(primary.codec.codec)} —
                  the player picks per your <a href="/settings">playback quality preference</a>.
                </p>
              </div>
              <InfoGrid
                rows={[
                  ['Primary codec', codecName(primary.codec.codec)],
                  ['Stream version', primary.codec.streamVersion ? `SV${primary.codec.streamVersion}` : undefined],
                  ['Sample rate', primary.codec.sampleRate ? `${primary.codec.sampleRate} Hz` : undefined],
                  ['Channels', primary.codec.channels],
                  ['Primary audio', primarySize],
                  ['Alternate formats', repGroups.map((g) => g.title).join(', ') || undefined],
                  ['Total audio', totalSize],
                ]}
              />
              {#if repGroups.length > 0}
                <h3 class="smallcaps section-heading">Per-track representations</h3>
                <ul class="rep-list">
                  {#each allTracks as track (track.id)}
                    {#if (track.representations?.length ?? 0) > 0}
                      <li>
                        <span class="rep-track">{track.number}. {track.title}</span>
                        <span class="rep-facts">
                          {#each track.representations ?? [] as rep (rep.id)}
                            <span class="rep-fact">
                              {rep.label ?? codecLabel(rep.codec.codec)} · {qualityLine({ codec: rep.codec.codec, sampleRate: rep.codec.sampleRate, channels: rep.codec.channels })} · {formatBytes(rep.size)}
                            </span>
                          {/each}
                        </span>
                      </li>
                    {/if}
                  {/each}
                </ul>
              {:else}
                <p class="muted">This edition carries a single audio representation per track.</p>
              {/if}
            {/if}
          {:else if section === 'analysis'}
            {#if rel.loudness}
              <div class="section-block">
                <h3 class="smallcaps section-heading">Album loudness</h3>
                <InfoGrid
                  rows={[
                    ['BS.1770 loudness', `${rel.loudness.albumLufs.toFixed(1)} LUFS (true peak ${rel.loudness.albumTruePeakDb.toFixed(1)} dBTP)`],
                    ['Algorithm', rel.loudness.algorithm],
                    ...(albumGain !== null
                      ? ([['Album normalization gain', `${albumGain > 0 ? '+' : ''}${albumGain.toFixed(1)} dB`]] as Array<[string, string]>)
                      : []),
                  ]}
                />
                <p class="muted">
                  Measured as one continuous program at build time. The gain preview assumes album-mode
                  normalization toward the player's −16 LUFS target.
                </p>
              </div>
            {:else}
              <p class="muted">This edition carries no album loudness measurement.</p>
            {/if}
            <h3 class="smallcaps section-heading">Track loudness &amp; waveforms</h3>
            <TrackTable release={rel} currentTrackId={currentTrackId} onPlay={playFrom} />
          {:else if section === 'metadata'}
            <h3 class="smallcaps section-heading">Credits</h3>
            <ul class="id-list">
              {#each detail.album.artists as a (a.id + a.name)}
                <li><span class="id-what">{a.name}</span>{#if a.role}<span class="muted"> — {a.role}</span>{/if}</li>
              {/each}
            </ul>
            <h3 class="smallcaps section-heading">Identifiers</h3>
            <ul class="id-list">
              {#if detail.album.mbid}
                <li><span class="smallcaps id-key">Release group</span><HashLine value={detail.album.mbid} label="MusicBrainz release group ID" /></li>
              {/if}
              {#if rel.mbid}
                <li><span class="smallcaps id-key">Release</span><HashLine value={rel.mbid} label="MusicBrainz release ID" /></li>
              {/if}
              {#each trackIsrcs as entry (entry.title + entry.isrc)}
                <li><span class="smallcaps id-key">ISRC · {entry.title}</span><span class="mono">{entry.isrc}</span></li>
              {/each}
            </ul>
            <h3 class="smallcaps section-heading">Source</h3>
            <InfoGrid
              rows={[
                ['Source type', rel.sourceType],
                ['Source store', rel.sourceStore],
                ['Source id', rel.sourceId],
                ['Identity source', rel.identitySource],
                ['Identity confidence', rel.identityConfidence],
              ]}
            />
            {#if rel.notes}
              <h3 class="smallcaps section-heading">Notes</h3>
              <p class="muted">{rel.notes}</p>
            {/if}
          {:else if section === 'package'}
            <div class="status-row" style="margin-bottom:var(--space-4)">
              <StatusChip chip={packageStatusChip(rel.packageStatus)} />
              <StatusChip chip={verifyStatusChip(rel.verifyStatus)} />
              {#if offlineChip}<StatusChip chip={offlineChip} />{/if}
            </div>
            <InfoGrid
              rows={[
                ['Package status', rel.packageStatus],
                ['Verification', rel.verifyStatus],
                ['Structure', `${discCount} ${discCount === 1 ? 'disc' : 'discs'} · ${trackCount} tracks`],
                ['Audio files', `${trackCount} primary${repCount > 0 ? ` · ${repCount} alternate${repCount === 1 ? '' : 's'}` : ''}`],
                ['Waveform envelopes', waveCount > 0 ? `${waveCount} of ${trackCount} tracks` : undefined],
                ['Total audio', totalSize],
                ['Artwork', `${rel.artwork.length} image${rel.artwork.length === 1 ? '' : 's'}`],
                ['Authored with', [rel.provenanceTool, rel.provenanceToolVersion].filter(Boolean).join(' ') || undefined],
              ]}
            />
            <h3 class="smallcaps section-heading">Assets</h3>
            <ul class="asset-list">
              {#each rel.artwork as art (art.id)}
                <li>
                  <span class="id-what">Artwork{art.role ? ` · ${art.role}` : ''}</span>
                  <span class="muted">{art.mimeType}</span>
                  {#if art.sha256}<HashLine value={art.sha256} label="Artwork SHA-256" />{/if}
                </li>
              {/each}
              {#each rel.assets as asset (asset.id)}
                <li>
                  <span class="id-what">{asset.kind}{asset.role ? ` · ${asset.role}` : ''}</span>
                  <span class="muted">{asset.mimeType}</span>
                  {#if asset.sha256}<HashLine value={asset.sha256} label="Asset SHA-256" />{/if}
                </li>
              {/each}
            </ul>
            <p class="muted section-note">
              The server verifies every referenced file's SHA-256 before this album is served; the offline
              download re-verifies hashes on this device during install.
            </p>
          {/if}
        </div>
      </article>
    </div>

    <ContextRail
      {detail}
      {rel}
      waveTrack={waveTrack}
      offlineState={$offlineStates.get(rel.id)?.state}
      onSelectEdition={selectEdition}
      onPlay={playWithPreference}
      onSection={setSection}
    />
  </div>

  {#if viewerOpen}
    <div
      class="viewer"
      role="dialog"
      aria-modal="true"
      aria-label="Artwork viewer"
      tabindex="-1"
      style="position:fixed;inset:0;background:rgba(8,9,11,0.94);z-index:40;display:flex;align-items:center;justify-content:center;flex-direction:column;gap:var(--space-4);padding:var(--space-5)"
      onclick={(e) => {
        if (e.target === e.currentTarget) viewerOpen = false;
      }}
      onkeydown={(e) => {
        if (e.key === 'Escape') viewerOpen = false;
      }}
    >
      <button
        style="position:absolute;top:var(--space-4);right:var(--space-5);color:var(--text);font-size:var(--fs-xl)"
        aria-label="Close artwork viewer"
        onclick={() => (viewerOpen = false)}>✕</button>
      {#each rel.artwork as art, i (art.id)}
        <div style="max-width:min(720px, 90vw);text-align:center">
          <img src={art.url} alt={`${title} — ${art.role ?? 'artwork'} ${i + 1}`} style="max-width:100%;max-height:70vh;object-fit:contain;border-radius:4px">
          <p class="smallcaps" style="color:var(--text)">{art.role ?? 'artwork'}</p>
        </div>
      {/each}
    </div>
  {/if}
{:else}
  <ErrorView message="Album not found" detail="It may have been removed from the collection." />
{/if}
