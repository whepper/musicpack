<!--
Copyright (c) 2026, The MusicPack Development Team
SPDX-License-Identifier: BSD-3-Clause
-->

<script lang="ts">
  import { onMount } from 'svelte';
  import { api, draft, draftStore } from '../bootstrap';
  import { chipState } from '../draft-store';
  import { validating, validation } from '../authoring-state';
  import type { IdentifyCandidate, IdentifyResult, ValidationResult } from '../types';
  import { artistLine, formatDate } from '../format';
  import ConfidenceBadge from './ConfidenceBadge.svelte';
  import ReleaseForm from './ReleaseForm.svelte';
  import TrackList from './TrackList.svelte';
  import EncodePanel from './EncodePanel.svelte';
  import ArtworkManager from './ArtworkManager.svelte';
  import IdentityPanel from './IdentityPanel.svelte';
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

  async function runValidation(): Promise<void> {
    const d = draft.get();
    if (!d) return;
    validating.set(true);
    try {
      const result: ValidationResult = await api.validateDraft(d);
      validation.set(result);
    } catch (e) {
      validation.set({
        ok: false,
        errors: [e instanceof Error ? e.message : 'Validation failed'],
        warnings: [],
      });
    } finally {
      validating.set(false);
    }
  }

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

  <ReleaseForm />

  <section class="section">
    <h2>Tracks</h2>
    <TrackList />
  </section>

  <EncodePanel />

  <section class="section">
    <h2>Artwork &amp; assets</h2>
    <ArtworkManager onChange={handleArtworkChange} />
  </section>

  <section class="section">
    <h2>Identity</h2>
    <IdentityPanel onIdentified={handleIdentify} candidates={candidates} />
  </section>

  <SonicPanel />

  <WaveformPanel />

  <section class="section">
    <h2>Validation</h2>
    <button class="btn ghost" onclick={runValidation} disabled={$validating}>
      {$validating ? 'Validating…' : 'Validate'}
    </button>
    {#if $validation}
      <ValidationPanel result={$validation} />
    {/if}
  </section>

  <CreateDialog />
{/if}
