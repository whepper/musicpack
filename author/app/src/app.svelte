<script lang="ts">
  import { api, draft, busy, draftStore, error } from './lib/bootstrap';
  import BackendBanner from './lib/ui/BackendBanner.svelte';
  import Welcome from './lib/ui/Welcome.svelte';
  import AlbumAuthoring from './lib/ui/AlbumAuthoring.svelte';
  import StatusBar from './lib/ui/StatusBar.svelte';

  async function openAlbum(path: string): Promise<void> {
    draftStore.setBusy(true);
    draftStore.setError(null);
    try {
      const d = await api.inspectAlbum(path);
      draftStore.setDraft(d);
    } catch (e) {
      draftStore.setError(e instanceof Error ? e.message : 'Could not open album.');
    } finally {
      draftStore.setBusy(false);
    }
  }

  function reset(): void {
    draftStore.clear();
  }
</script>

<svelte:head>
  <title>MusicPack Author</title>
</svelte:head>

<div class="topnav">
  <span class="brand">MusicPack <em>Author</em></span>
  <span class="smallcaps">an album being authored</span>
</div>

<BackendBanner />

<main class="main">
  {#if $busy && !$draft}
    <div class="spinner" role="status"></div>
  {:else if $draft}
    <AlbumAuthoring onReset={reset} />
  {:else}
    <Welcome onOpen={openAlbum} />
    {#if $error}
      <div class="error-banner">{$error}</div>
    {/if}
  {/if}
</main>

<StatusBar />
