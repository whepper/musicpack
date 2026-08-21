<!--
Copyright (c) 2026, The MusicPack Development Team
SPDX-License-Identifier: BSD-3-Clause
-->

<script lang="ts">
  import { onMount } from 'svelte';
  import { api, draft, draftStore } from '../bootstrap';
  import { validating, validation, encodeStaging } from '../authoring-state';
  import { runValidation } from '../validate';
  import type { IdentifyCandidate, IdentifyResult } from '../types';
  import { artistLine, formatDate } from '../format';
  import ConfidenceBadge from './ConfidenceBadge.svelte';
  import ReleaseForm from './ReleaseForm.svelte';
  import TrackList from './TrackList.svelte';
  import EncodePanel from './EncodePanel.svelte';
  import ArtworkManager from './ArtworkManager.svelte';
  import IdentityPanel from './IdentityPanel.svelte';
  import SectionRail from './SectionRail.svelte';
  import ValidationPanel from './ValidationPanel.svelte';
  import SonicPanel from './SonicPanel.svelte';
  import WaveformPanel from './WaveformPanel.svelte';
  import CreateDialog from './CreateDialog.svelte';

  let { onReset }: { onReset: () => void } = $props();


  let coverUrl = $state<string | null>(null);
  let candidates = $state<IdentifyCandidate[] | null>(null);

  async function loadCover(): Promise<void> {
    coverUrl = null;
    const d = draft.get();
    if (!d) return;
    const file = d.artwork.find((a) => a.path);
    if (!file?.path) return;
    try {
      const img = await api.readImage(`${d.sourceRoot}/${file.path}`);
      coverUrl = `data:${img.mime};base64,${img.dataBase64}`;
    } catch {
      coverUrl = null;
    }
  }

  // ---- autosave + window title + debounced auto-validation ----------------

  let saveTimer: ReturnType<typeof setTimeout> | null = null;
  let validateTimer: ReturnType<typeof setTimeout> | null = null;

  $effect(() => {
    const d = $draft;
    if (!d) return;

    // window title reflects the album being authored
    const artist = d.album.artists?.[0]?.name ?? '';
    document.title =
      `${d.album.title || 'Untitled album'}${artist ? ` — ${artist}` : ''} · MusicPack Author`;

    // autosave (debounced): an accidental quit no longer discards the draft.
    // Skipped while encode staging is active — that transformed draft points
    // at temporary encoded files which won't survive a restart.
    const staged = encodeStaging.get();
    if (!staged && saveTimer) clearTimeout(saveTimer);
    if (!staged) {
      saveTimer = setTimeout(() => {
        void api.draftSave(JSON.stringify(d)).catch(() => {
          /* persistence is best-effort; authoring continues in memory */
        });
      }, 800);
    }

    // auto-validation (debounced): keeps the verdict fresh after edits so
    // Create is gated by a current result rather than a stale one
    if (validateTimer) clearTimeout(validateTimer);
    validateTimer = setTimeout(() => {
      if (!validating.get()) void runValidation();
    }, 1200);
  });

  onMount(() => {
    return () => {
      if (saveTimer) clearTimeout(saveTimer);
      if (validateTimer) clearTimeout(validateTimer);
      document.title = 'MusicPack Author';
    };
  });

  function handleIdentify(result: IdentifyResult): void {
    if (result.kind === 'applied') {
      draftStore.setDraft(result.draft);
      candidates = null;
      void loadCover();
    } else {
      candidates = result.candidates;
    }
  }

  function handleArtworkChange(): Promise<void> {
    return loadCover();
  }

  onMount(() => {
    void loadCover();
  });
</script>

{#if $draft}
  <div class="page">
    <div class="content">
      <header class="author-header">
    <div class="cover">
      {#if coverUrl}
        <img src={coverUrl} alt="Front artwork" />
      {:else}
        <div class="cover-empty">
          {#if $draft.artwork.length > 0}
            embedded · extracted at build
          {:else}
            no artwork
          {/if}
        </div>
      {/if}
    </div>
    <div>
      <h1>{$draft.album.title || 'Untitled album'}</h1>
      <div class="artist">{artistLine($draft.album.artists)}</div>
      {#if $draft.release?.edition}
        <div class="edition">{$draft.release.edition}</div>
      {/if}
      <div class="edition">
        {#if $draft.album.originalReleaseDate}original {formatDate($draft.album.originalReleaseDate)}{/if}
        {#if $draft.release?.releaseDate} · released {formatDate($draft.release.releaseDate)}{/if}
        {#if $draft.release?.country} · {$draft.release.country}{/if}
        {#if $draft.album.releaseType} · {$draft.album.releaseType}{/if}
      </div>
      <div class="edition">
        <ConfidenceBadge confidence={$draft.identity?.confidence} />
      </div>
      <div class="source-root">{$draft.sourceRoot}</div>
      <button class="btn ghost" onclick={onReset}>← Choose a different album</button>
    </div>
  </header>

  <section class="section" id="sec-identity">
    <h2>Identity</h2>
    <IdentityPanel onIdentified={handleIdentify} candidates={candidates} />
  </section>

  <div id="sec-release">
    <ReleaseForm />
  </div>

  <section class="section" id="sec-tracks">
    <h2>Tracks</h2>
    <TrackList />
  </section>

  <section class="section" id="sec-artwork">
    <h2>Artwork &amp; assets</h2>
    <ArtworkManager onChange={handleArtworkChange} />
  </section>

  <div id="sec-encode">
    <EncodePanel />
  </div>

  <div id="sec-sonic">
    <SonicPanel />
  </div>

  <div id="sec-waveform">
    <WaveformPanel />
  </div>

  <section class="section" id="sec-validation">
    <h2>Validation</h2>
    <p class="muted smallcaps">Runs automatically after edits; the button forces a fresh check.</p>
    <button class="btn ghost" onclick={runValidation} disabled={$validating}>
      {$validating ? 'Validating…' : 'Validate now'}
    </button>
    {#if $validation}
      <ValidationPanel result={$validation} />
    {:else}
      <p class="muted">Not validated yet.</p>
    {/if}
  </section>
</div>

<nav class="rail-slot" aria-label="Album sections">
  <SectionRail />
</nav>
  </div>

  <CreateDialog />
{/if}

<style>
  .page {
    display: flex;
    gap: 24px;
    align-items: flex-start;
  }
  .content {
    flex: 1;
    min-width: 0;
  }
  .rail-slot {
    position: sticky;
    top: 76px;
  }
</style>
