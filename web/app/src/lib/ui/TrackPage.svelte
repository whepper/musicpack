<!--
Copyright (c) 2026, The MusicPack Development Team
SPDX-License-Identifier: BSD-3-Clause
-->

<script lang="ts">
  // /tracks/:id — the track-detail surface. Editorial, then technical:
  // hero (artwork + context + playback), then a single editorial inspector
  // that exposes the depth of the .mpack without becoming a developer
  // screen. Every value shown is real API or real package data.
  import { api, library, player, playerModel, queue, audioPreference, offline, offlineStates, router } from '../bootstrap';
  import type { AudioPreference } from '../state/representation-selection';
  import type { MediaDisc, ReleaseDetail, Track, TrackDetail } from '../api/types';
  import Artwork from './Artwork.svelte';
  import DownloadControl from './DownloadControl.svelte';
  import ErrorView from './ErrorView.svelte';
  import StatusChip from './StatusChip.svelte';
  import HashLine from './HashLine.svelte';
  import InfoTile from './InfoTile.svelte';
  import InfoGrid from './InfoGrid.svelte';
  import WaveformSpark from './WaveformSpark.svelte';
  import WaveformSeek from './WaveformSeek.svelte';
  import {
    identityChip,
    offlineStateChip,
    packageStatusChip,
    verifyStatusChip,
  } from './status';
  import {
    codecLabel,
    codecName,
    countryName,
    formatBytes,
    fmtTime,
    qualityLine,
    yearOf,
  } from '../format';
  import { normalizationGainDb } from '../playback/loudness';
  import { tracksOfRelease } from '../state/queue';
  import { onMount } from 'svelte';

  let { trackId }: { trackId: string } = $props();

  let detail = $state<TrackDetail | null>(null);
  let rel = $state<ReleaseDetail | null>(null);
  let error = $state<string | null>(null);
  let loading = $state(true);

  const currentTrackId = $derived($playerModel.current?.track.id);

  onMount(() => {
    void load();
  });

  async function load(): Promise<void> {
    loading = true;
    error = null;
    detail = null;
    rel = null;
    try {
      const d = await api.trackDetail(trackId);
      detail = d;
      // Pull the owning release for the editorial inspector (status, hashes,
      // provenance, package counts, edition facts). Refreshed copy bypasses
      // the library cache so a stale release JSON never lingers on this page.
      rel = await library.refreshRelease(d.context.releaseId);
      if (offline.enabled) void offline.checkForUpdate(rel);
    } catch (e) {
      error = e instanceof Error ? e.message : 'Could not open this track.';
    } finally {
      loading = false;
    }
  }

  // ---- derived facts (real data only) ----------------------------------
  const t = $derived(detail?.track ?? null);
  const ctx = $derived(detail?.context);

  const leadArtist = $derived(t?.artists[0]);
  const trackDisc = $derived.by(() => {
    if (!rel || !t || !ctx) return null;
    return rel.media.find((m) => m.disc === ctx.disc) ?? null;
  });
  const trackIndexInRelease = $derived.by(() => {
    if (!rel || !t) return -1;
    let i = 0;
    for (const disc of [...rel.media].sort((a, b) => a.disc - b.disc)) {
      for (const track of disc.tracks) {
        if (track.id === t.id) return i;
        i += 1;
      }
    }
    return -1;
  });
  const allTracks = $derived(rel ? rel.media.flatMap((m) => m.tracks) : []);
  const trackCount = $derived(allTracks.length);
  const waveCount = $derived(allTracks.filter((tr) => tr.waveform).length);
  const repCount = $derived(allTracks.reduce((n, tr) => n + (tr.representations?.length ?? 0), 0));

  const isMpc = $derived(Boolean(t && t.codec.codec.toLowerCase().startsWith('musepack')));
  const primaryFormat = $derived(t ? qualityLine({ codec: t.codec.codec, sampleRate: t.codec.sampleRate, channels: t.codec.channels }) : '');
  const altFormats = $derived(t?.representations ?? []);

  const albumUrl = $derived(ctx ? `/albums/${ctx.albumId}?release=${ctx.releaseId}` : '/');

  const trackGainAlbum = $derived.by(() => {
    if (!rel?.loudness) return null;
    return normalizationGainDb('album', undefined, {
      albumLufs: rel.loudness.albumLufs,
      albumTruePeakDb: rel.loudness.albumTruePeakDb,
    });
  });
  const trackGainTrack = $derived(
    t?.loudness ? normalizationGainDb('track', t.loudness) : null,
  );

  // Track identifier rows — only keys the API actually carries.
  const identifiers = $derived.by(() => {
    const out: Array<[string, string]> = [];
    if (t?.isrc) out.push(['ISRC', t.isrc]);
    return out;
  });

  const identity = $derived(rel ? identityChip(rel.identitySource, rel.identityConfidence) : null);
  const offlineChip = $derived(rel ? offlineStateChip($offlineStates.get(rel.id)?.state) : null);

  // Position in the track-relative timeline (used for the seek control's
  // initial position when the user opens the page while it is playing).
  const wfTrack = $derived<Track | null>(t ?? null);
  const trackStart = $derived($playerModel.current?.track.id === t?.id ? $playerModel.currentTrackStartSeconds : 0);
  const trackDur = $derived($playerModel.current?.track.id === t?.id ? $playerModel.currentTrackDurationSeconds : (t?.duration ?? 0));
  const withinPos = $derived(Math.max(0, Math.min(trackDur, $playerModel.positionSeconds - trackStart)));
  const rangeFill = $derived(trackDur > 0 ? Math.max(0, Math.min(100, (withinPos / trackDur) * 100)) : 0);

  // ---- actions ---------------------------------------------------------
  function play(): void {
    if (!rel || !detail) return;
    const startIndex = trackIndexInRelease >= 0 ? trackIndexInRelease : 0;
    const artist = leadArtist?.name ?? '';
    void player.playAlbum(rel, detail.track.title, artist, startIndex);
  }
  function playRepresentation(preference?: AudioPreference): void {
    if (!rel || !detail) return;
    if (preference) audioPreference.set(preference);
    const startIndex = trackIndexInRelease >= 0 ? trackIndexInRelease : 0;
    const artist = leadArtist?.name ?? '';
    void player.playAlbum(rel, detail.track.title, artist, startIndex);
  }
  function shuffle(): void {
    if (!rel || !detail) return;
    player.setShuffle(true);
    const count = tracksOfRelease(rel).length;
    const startIndex = count > 0 ? Math.floor(Math.random() * count) : 0;
    void player.playAlbum(rel, detail.track.title, leadArtist?.name ?? '', startIndex);
  }
  function addToQueue(): void {
    if (!rel || !detail) return;
    // addAlbum already applies the existing representation-selection policy
    // and the per-track cover-art fallback, so we route through it.
    queue.addAlbum(rel, rel.album.title, leadArtist?.name ?? '');
  }

  // Build a representation-row entry per track variant. Display only what
  // the server actually claims (codec, sampleRate, channels, size, label).
  function representationRows(): Array<{ key: string; label: string; sub: string; size?: number; sha?: string }> {
    if (!t) return [];
    const rows: Array<{ key: string; label: string; sub: string; size?: number; sha?: string }> = [];
    rows.push({
      key: 'primary',
      label: codecName(t.codec.codec),
      sub: isMpc && t.codec.streamVersion
        ? `SV${t.codec.streamVersion}`
        : qualityLine({ codec: t.codec.codec, sampleRate: t.codec.sampleRate, channels: t.codec.channels }),
      size: t.audio.size,
      sha: t.audio.sha256,
    });
    for (const rep of altFormats) {
      rows.push({
        key: String(rep.id),
        label: rep.label ?? codecName(rep.codec.codec),
        sub: qualityLine({ codec: rep.codec.codec, sampleRate: rep.codec.sampleRate, channels: rep.codec.channels }),
        size: rep.size,
        sha: rep.sha256,
      });
    }
    return rows;
  }
  const repRows = $derived(representationRows());

  // The track page lets users jump to the package section for the full
  // inspector; deep links reuse the canonical album route.
  const goPackage = $derived(ctx ? `${albumUrl}&section=package` : null);
  const goAudio = $derived(ctx ? `${albumUrl}&section=audio` : null);
  const goAnalysis = $derived(ctx ? `${albumUrl}&section=analysis` : null);
  const goEdition = $derived(ctx ? `${albumUrl}&section=edition` : null);
  const goMetadata = $derived(ctx ? `${albumUrl}&section=metadata` : null);
  function navSection(href: string | null): void {
    if (href) router.go(href);
  }

  // Hero art: prefer the front-cover of the owning release; the
  // `/api/v1/tracks/{id}` payload deliberately doesn't carry artwork.
  const heroArt = $derived(
    rel?.artwork.find((a) => a.role === 'front') ?? rel?.artwork[0],
  );

  // Disc / position rows.
  const positionLine = $derived.by(() => {
    if (!ctx || !rel) return null;
    const media: MediaDisc | undefined = trackDisc ?? undefined;
    const pos = trackIndexInRelease + 1;
    return {
      disc: ctx.disc,
      of: rel.media.length,
      pos,
      of2: trackCount,
      title: media?.title,
    };
  });
</script>

{#if loading}
  <div class="spinner" role="status" aria-label="Loading track"></div>
{:else if error}
  <ErrorView message={error} detail="It may have been removed from the collection, or the server is unreachable." />
{:else if detail && t && ctx && rel}
  <div class="track-layout">
    <div class="track-main">
      <article>
        <header class="album-hero">
          <div class="cover">
            <Artwork src={heroArt?.url} alt={`${t.title} — front cover`} label={t.title} />
          </div>
          <div class="album-heading">
            <p class="eyebrow">
              <a href={albumUrl}>{rel.album.title}</a>
              {#if rel.edition} · {rel.edition}{/if}
            </p>
            <h1>{t.title}</h1>
            <p class="artist">
              {#if leadArtist}<a href={`/artists/${leadArtist.id}`}>{leadArtist.name}</a>{:else}Unknown artist{/if}
            </p>
            <p class="edition-line">
              {#if positionLine}
                Disc {positionLine.disc} of {positionLine.of}{#if positionLine.title} — {positionLine.title}{/if}
                · Track {positionLine.pos} of {positionLine.of2}
              {/if}
            </p>
            <p class="edition-line">
              {#if t.duration}{fmtTime(t.duration)} · {/if}
              {primaryFormat || codecLabel(t.codec.codec)}
            </p>
            {#if identity || rel.sourceType}
              <p class="badge-row">
                {#if identity}<span class="badge">{#if identity.mark}<span aria-hidden="true">{identity.mark}</span>{/if}{identity.label}</span>{/if}
                {#if rel.sourceType}<span class="badge">{rel.sourceType}</span>{/if}
              </p>
            {/if}
            <div class="hero-actions">
              <button class="btn" onclick={play}>▶ Play track</button>
              <button class="btn ghost" onclick={shuffle}>⤨ Shuffle album</button>
              {#if repRows.length > 1}
                <button
                  class="btn ghost"
                  aria-label="Play this track with the alternate representation selected"
                  onclick={() => {
                    const alt = repRows.find((r) => r.key !== 'primary');
                    if (!alt) return;
                    playRepresentation({ mode: 'codec', codec: alt.label.toLowerCase() });
                  }}
                >Play alternate</button>
              {/if}
              <button
                class="btn ghost album-add"
                aria-label="Add track to queue"
                onclick={addToQueue}
              >+ Add to Queue</button>
              {#if rel}
                <DownloadControl release={rel} states={offline.states} downloads={offline.downloads} />
              {/if}
            </div>
            <p class="status-row">
              <StatusChip chip={packageStatusChip(rel.packageStatus)} />
              <StatusChip chip={verifyStatusChip(rel.verifyStatus)} />
              {#if offlineChip}<StatusChip chip={offlineChip} />{/if}
            </p>
          </div>
        </header>

        <section class="track-seek">
          <span class="time smallcaps">{fmtTime(withinPos)}</span>
          <div class="track-seek-control">
            {#if wfTrack && wfTrack.waveform}
              <WaveformSeek
                track={wfTrack}
                startSeconds={trackStart}
                durationSeconds={trackDur}
                positionSeconds={$playerModel.positionSeconds}
                onSeek={(s) => void player.seek(s)}
              />
            {:else if t.duration}
              <input
                type="range"
                min="0"
                max={t.duration}
                step="0.5"
                value={withinPos}
                aria-label="Seek position"
                style={`--range-fill:${rangeFill}%`}
                oninput={(e) => {
                  const v = Number((e.currentTarget as HTMLInputElement).value);
                  if (currentTrackId === t.id) void player.seek(trackStart + v);
                }}
                onchange={(e) => {
                  const v = Number((e.currentTarget as HTMLInputElement).value);
                  if (currentTrackId === t.id) void player.seek(trackStart + v);
                }}
              >
            {/if}
          </div>
          <span class="time smallcaps">{t.duration ? fmtTime(t.duration) : '—'}</span>
        </section>

        <div class="info-tiles" role="group" aria-label="Track facts">
          <InfoTile
            title={codecName(t.codec.codec)}
            sub={isMpc && t.codec.streamVersion ? `SV${t.codec.streamVersion}` : (primaryFormat || '—')}
            href={goAudio ?? undefined}
          />
          <InfoTile
            title={rel.loudness ? `${rel.loudness.albumLufs.toFixed(1)} LUFS` : '—'}
            sub={rel.loudness ? 'Album loudness' : 'No album loudness'}
            href={goAnalysis ?? undefined}
          />
          <InfoTile
            title={t.loudness ? `${t.loudness.lufs.toFixed(1)} LUFS` : '—'}
            sub={t.loudness ? `${t.loudness.truePeakDb.toFixed(1)} dBTP` : 'No track loudness'}
            href={goAnalysis ?? undefined}
          />
        </div>

        <div class="album-section">
          <h2 class="smallcaps section-heading">Audio</h2>
          <InfoGrid
            rows={[
              ['Codec', codecName(t.codec.codec)],
              ['Stream version', t.codec.streamVersion ? `SV${t.codec.streamVersion}` : undefined],
              ['Sample rate', t.codec.sampleRate ? `${t.codec.sampleRate} Hz` : undefined],
              ['Channels', t.codec.channels],
              ['Duration', t.duration ? fmtTime(t.duration) : undefined],
              ['Primary file', t.audio.size ? formatBytes(t.audio.size) : undefined],
            ]}
          />
          {#if t.audio.sha256}
            <p class="section-note">
              Audio SHA-256: <HashLine value={t.audio.sha256} label="audio SHA-256" />
            </p>
          {/if}

          <h3 class="smallcaps section-heading">Representations</h3>
          <ul class="rep-list">
            {#each repRows as row (row.key)}
              <li>
                <span class="rep-track">{row.label}</span>
                <span class="rep-facts">
                  {row.sub}
                  {#if row.size}· {formatBytes(row.size)}{/if}
                  {#if row.sha}· <HashLine value={row.sha} label={`${row.label} SHA-256`} />{/if}
                </span>
              </li>
            {/each}
          </ul>
        </div>

        <div class="album-section">
          <h2 class="smallcaps section-heading">Analysis</h2>
          {#if t.loudness || rel.loudness}
            <InfoGrid
              rows={[
                ['Track integrated', t.loudness ? `${t.loudness.lufs.toFixed(2)} LUFS` : undefined],
                ['Track true peak', t.loudness ? `${t.loudness.truePeakDb.toFixed(2)} dBTP` : undefined],
                ...(trackGainTrack !== null
                  ? ([['Track normalization gain', `${trackGainTrack > 0 ? '+' : ''}${trackGainTrack.toFixed(1)} dB`]] as Array<[string, string]>)
                  : []),
                ['Album integrated', rel.loudness ? `${rel.loudness.albumLufs.toFixed(2)} LUFS` : undefined],
                ['Album true peak', rel.loudness ? `${rel.loudness.albumTruePeakDb.toFixed(2)} dBTP` : undefined],
                ...(trackGainAlbum !== null
                  ? ([['Album normalization gain', `${trackGainAlbum > 0 ? '+' : ''}${trackGainAlbum.toFixed(1)} dB`]] as Array<[string, string]>)
                  : []),
                ['Algorithm', rel.loudness?.algorithm],
              ]}
            />
            <p class="muted section-note">
              Loudness measured as one program at build time. The gain preview uses the
              player's published policy (−16 LUFS target, −1 dBTP ceiling).
            </p>
          {:else}
            <p class="muted">No loudness measurement is recorded for this track or album.</p>
          {/if}

          {#if t.waveform}
            <h3 class="smallcaps section-heading">Waveform</h3>
            <div class="track-waveform">
              <WaveformSpark track={t} height={48} />
              <p class="muted">
                {t.waveform.intervalMs} ms buckets · {t.waveform.encoding} · {t.waveform.floorDb} dB floor · {t.waveform.points} points
              </p>
            </div>
          {/if}
        </div>

        <div class="album-section">
          <h2 class="smallcaps section-heading">Metadata</h2>
          <InfoGrid
            rows={[
              ['Album', rel.album.title],
              ['Edition', rel.edition],
              ['Release date', rel.releaseDate ? `${rel.releaseDate.slice(0, 4)}` : undefined],
              ['Country', countryName(rel.country)],
              ['Label', rel.label],
              ['Catalogue number', rel.catalogueNumber],
              ['Release group', rel.album.mbid],
              ['Release', rel.mbid],
            ]}
          />
          {#if identifiers.length > 0}
            <InfoGrid rows={identifiers} />
          {/if}
          {#if t.artists.length > 1}
            <h3 class="smallcaps section-heading">Credits</h3>
            <ul class="id-list">
              {#each t.artists as a (a.id + a.name)}
                <li><span class="id-what">{a.name}</span>{#if a.role}<span class="muted"> — {a.role}</span>{/if}</li>
              {/each}
            </ul>
          {/if}
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
        </div>

        <div class="album-section">
          <h2 class="smallcaps section-heading">Package</h2>
          <p class="status-row">
            <StatusChip chip={packageStatusChip(rel.packageStatus)} />
            <StatusChip chip={verifyStatusChip(rel.verifyStatus)} />
            {#if offlineChip}<StatusChip chip={offlineChip} />{/if}
          </p>
          <InfoGrid
            rows={[
              ['Structure', `${rel.media.length} ${rel.media.length === 1 ? 'disc' : 'discs'} · ${trackCount} tracks`],
              ['Audio files', `${trackCount} primary${repCount > 0 ? ` · ${repCount} alternate${repCount === 1 ? '' : 's'}` : ''}`],
              ['Waveforms', waveCount > 0 ? `${waveCount} of ${trackCount} tracks` : undefined],
              ['Authored with', [rel.provenanceTool, rel.provenanceToolVersion].filter(Boolean).join(' ') || undefined],
            ]}
          />
          <p class="muted section-note">
            The server verifies every referenced file's SHA-256 before this album is served; the
            offline download re-verifies hashes on this device during install. See the
            <button class="inline-link" onclick={() => navSection(goPackage)}>full Package section</button>
            for the complete asset inventory.
          </p>
        </div>
      </article>
    </div>

    <aside class="album-rail" aria-label="Track inspector">
      <section class="rail-panel">
        <header><h3 class="smallcaps">Album</h3>
          <button class="rail-link" onclick={() => router.go(albumUrl)}>›</button>
        </header>
        <dl class="rail-kv">
          <div><dt>Title</dt><dd>{rel.album.title}</dd></div>
          <div><dt>Edition</dt><dd>{rel.edition ?? '—'}</dd></div>
          <div><dt>Tracks</dt><dd>{trackCount} across {rel.media.length} {rel.media.length === 1 ? 'disc' : 'discs'}</dd></div>
          <div><dt>Year</dt><dd>{yearOf(rel.releaseDate ?? rel.album.originalReleaseDate)}</dd></div>
        </dl>
      </section>

      <section class="rail-panel">
        <header><h3 class="smallcaps">Audio</h3>
          <button class="rail-link" onclick={() => navSection(goAudio)}>›</button>
        </header>
        <div class="rep-card">
          <div class="rep-body">
            <p class="rep-title">{repRows[0]?.label ?? '—'}
              {#if repRows[0]?.size}<span class="rep-size"> · {formatBytes(repRows[0].size)}</span>{/if}
            </p>
            {#if repRows[0]?.sub}<p class="rep-sub">{repRows[0].sub}</p>{/if}
          </div>
          <button class="btn-small" onclick={() => playRepresentation()}>Play</button>
        </div>
        {#each repRows.slice(1) as row (row.key)}
          <div class="rep-card">
            <div class="rep-body">
              <p class="rep-title">{row.label}{#if row.size}<span class="rep-size"> · {formatBytes(row.size)}</span>{/if}</p>
              {#if row.sub}<p class="rep-sub">{row.sub}</p>{/if}
            </div>
            <button
              class="btn-small"
              aria-label={`Play track in ${row.label}`}
              onclick={() => playRepresentation({ mode: 'codec', codec: row.label.toLowerCase() })}
            >Play</button>
          </div>
        {/each}
      </section>

      <section class="rail-panel">
        <header><h3 class="smallcaps">Analysis</h3>
          <button class="rail-link" onclick={() => navSection(goAnalysis)}>›</button>
        </header>
        {#if t.loudness}
          <dl class="rail-kv">
            <div><dt>Integrated</dt><dd>{t.loudness.lufs.toFixed(1)} LUFS</dd></div>
            <div><dt>True peak</dt><dd>{t.loudness.truePeakDb.toFixed(1)} dBTP</dd></div>
          </dl>
        {/if}
        {#if rel.loudness}
          <dl class="rail-kv" style="margin-top:var(--space-2)">
            <div><dt>Album</dt><dd>{rel.loudness.albumLufs.toFixed(1)} LUFS</dd></div>
            <div><dt>Album peak</dt><dd>{rel.loudness.albumTruePeakDb.toFixed(1)} dBTP</dd></div>
          </dl>
        {/if}
        {#if t.waveform}
          <div class="analysis-mini">
            <p class="smallcaps rail-label" style="margin-top:var(--space-3)">Waveform</p>
            <WaveformSpark track={t} height={44} />
          </div>
        {/if}
      </section>

      <section class="rail-panel">
        <header><h3 class="smallcaps">Package</h3>
          <button class="rail-link" onclick={() => navSection(goPackage)}>›</button>
        </header>
        <div class="status-row" style="margin-bottom:var(--space-3)">
          <StatusChip chip={verifyStatusChip(rel.verifyStatus)} />
          <StatusChip chip={packageStatusChip(rel.packageStatus)} />
          {#if offlineChip}<StatusChip chip={offlineChip} />{/if}
        </div>
        <dl class="rail-kv">
          <div><dt>Authored with</dt><dd>{[rel.provenanceTool, rel.provenanceToolVersion].filter(Boolean).join(' ') || '—'}</dd></div>
          <div><dt>Waveforms</dt><dd>{waveCount > 0 ? `${waveCount} / ${trackCount}` : '—'}</dd></div>
          <div><dt>Alt. files</dt><dd>{repCount > 0 ? repCount : '—'}</dd></div>
        </dl>
        <div class="rail-link-row">
          <button class="inline-link" onclick={() => navSection(goEdition)}>Edition facts</button>
          <span aria-hidden="true">·</span>
          <button class="inline-link" onclick={() => navSection(goMetadata)}>Identifiers</button>
        </div>
      </section>
    </aside>
  </div>
{:else}
  <ErrorView message="Track not found" detail="It may have been removed from the collection." />
{/if}
