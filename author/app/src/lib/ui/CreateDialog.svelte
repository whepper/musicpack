<script lang="ts">
  import { onMount } from 'svelte';
  import { api, draft, draftStore } from '../bootstrap';
  import { createOpen, createResult } from '../authoring-state';
  import type { ValidationResult } from '../types';


  let outputDir = $state<string | null>(null);
  let creating = $state(false);
  let reVerifying = $state(false);

  onMount(() => {
    const fn = (e: KeyboardEvent): void => {
      if (e.key === 'Escape') close();
    };
    window.addEventListener('keydown', fn);
    return () => window.removeEventListener('keydown', fn);
  });

  async function chooseOutput(): Promise<void> {
    outputDir = await api.pickOutputDirectory();
  }

  async function runCreate(): Promise<void> {
    const d = draft.get();
    if (!d || !outputDir) return;
    creating = true;
    try {
      createResult.set(await api.createPackage(d, outputDir));
    } catch (e) {
      createResult.set({
        ok: false,
        error: { code: 'create_failed', message: e instanceof Error ? e.message : 'Create failed.' },
      });
    } finally {
      creating = false;
    }
  }

  async function reVerify(): Promise<void> {
    const r = createResult.get();
    if (!r?.outputPath) return;
    reVerifying = true;
    try {
      const v: ValidationResult = await api.verifyPackage(r.outputPath);
      createResult.set({ ...r, ok: v.ok, verify: { errors: v.errors.length, warnings: v.warnings.length } });
    } catch (e) {
      createResult.set({ ...r, ok: false, error: { message: e instanceof Error ? e.message : 'Verify failed.' } });
    } finally {
      reVerifying = false;
    }
  }

  function close(): void {
    createOpen.set(false);
  }

  function reveal(): void {
    const r = createResult.get();
    if (r?.outputPath) void api.revealInFinder(r.outputPath);
  }
</script>

{#if $createOpen}
  <div
    style="position:fixed;inset:0;background:rgba(29,27,24,0.35);z-index:40;display:flex;align-items:center;justify-content:center"
    role="dialog"
    aria-modal="true"
    aria-label="Create MusicPack"
    tabindex="-1"
  >
    <div
      class="result-panel"
      style="text-align:left;max-width:640px;width:92%"
      role="document"
      tabindex="-1"
    >
      {#if $createResult}
        {@const r = $createResult}
        <h2>{r.ok ? 'Package created' : 'Package creation failed'}</h2>
        {#if r.ok}
          <p class="path">{r.outputPath}</p>
          <p class="smallcaps">
            Verification: {r.verify?.errors ?? 0} error(s), {r.verify?.warnings ?? 0} warning(s)
          </p>
          <div class="artwork-row">
            <button class="btn" onclick={reveal}>Reveal in Finder</button>
            <button class="btn ghost" onclick={reVerify} disabled={reVerifying}>
              {reVerifying ? 'Verifying…' : 'Re-verify'}
            </button>
            <button class="btn ghost" onclick={close}>Close</button>
          </div>
        {:else}
          <p class="error-banner" style="margin:0">{r.error?.message ?? 'Unknown error.'}</p>
          <p class="smallcaps">The package was not reported as successful.</p>
          <button class="btn ghost" onclick={close}>Close</button>
        {/if}
      {:else}
        <h2>Create MusicPack</h2>
        {#if outputDir}
          <p class="path">{outputDir}</p>
        {:else}
          <p class="muted">Choose where to write the <span class="smallcaps">.mpack</span> directory.</p>
        {/if}
        <div class="artwork-row">
          <button class="btn ghost" onclick={chooseOutput}>
            {outputDir ? 'Change output…' : 'Choose output…'}
          </button>
          <button class="btn" onclick={runCreate} disabled={!outputDir || creating}>
            {creating ? 'Creating…' : 'Create'}
          </button>
          <button class="btn ghost" onclick={close}>Cancel</button>
        </div>
      {/if}
    </div>
  </div>
{/if}
