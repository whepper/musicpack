<script lang="ts">
  import { onMount } from 'svelte';
  import { api, draft, draftStore } from '../bootstrap';
  import { createOpen, createResult, encodeStaging } from '../authoring-state';
  import { defaultPackageName } from '../format';
  import type { ValidationResult } from '../types';


  let outputParent = $state<string | null>(null);
  let packageName = $state('');
  let creating = $state(false);
  let reVerifying = $state(false);

  onMount(() => {
    const fn = (e: KeyboardEvent): void => {
      if (e.key === 'Escape') close();
    };
    window.addEventListener('keydown', fn);
    return () => window.removeEventListener('keydown', fn);
  });

  function outputPath(): string | null {
    if (!outputParent) return null;
    const name = packageName.trim() || 'Untitled';
    return `${outputParent}/${name}.mpack`;
  }

  async function chooseOutput(): Promise<void> {
    const parent = await api.pickOutputDirectory();
    if (!parent) return;
    outputParent = parent;
    const d = draft.get();
    if (d) packageName = defaultPackageName(d);
  }

  async function runCreate(): Promise<void> {
    const d = draft.get();
    const out = outputPath();
    if (!d || !out) return;
    creating = true;
    try {
      createResult.set(await api.createPackage(d, out));
    } catch (e) {
      createResult.set({
        ok: false,
        error: { code: 'create_failed', message: e instanceof Error ? e.message : 'Create failed.' },
      });
    } finally {
      creating = false;
    }
    // a successfully built package no longer needs its encode staging area
    if (createResult.get()?.ok) {
      const staging = encodeStaging.get();
      if (staging) {
        encodeStaging.set(null);
        try {
          await api.cleanupStaging(staging);
        } catch {
          /* the temp directory is eventually reclaimed; not a user error */
        }
      }
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
        {#if outputParent}
          <label class="smallcaps" for="pkg-name">Package name</label>
          <input
            id="pkg-name"
            type="text"
            bind:value={packageName}
            placeholder="Artist - Album"
            style="width:100%;margin:0.35rem 0 0.75rem;box-sizing:border-box"
          />
          <p class="path">{outputPath()}</p>
        {:else}
          <p class="muted">Choose where to write the <span class="smallcaps">.mpack</span> directory.</p>
        {/if}
        <div class="artwork-row">
          <button class="btn ghost" onclick={chooseOutput}>
            {outputParent ? 'Change output…' : 'Choose output…'}
          </button>
          <button class="btn" onclick={runCreate} disabled={!outputPath() || creating}>
            {creating ? 'Creating…' : 'Create'}
          </button>
          <button class="btn ghost" onclick={close}>Cancel</button>
        </div>
      {/if}
    </div>
  </div>
{/if}
