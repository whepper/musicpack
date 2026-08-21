<!--
Copyright (c) 2026, The MusicPack Development Team
SPDX-License-Identifier: BSD-3-Clause
-->

<script lang="ts">
  import { onMount } from 'svelte';
  import { api, draft, draftStore } from '../bootstrap';
  import { createOpen, createResult, encodeStaging } from '../authoring-state';
  import { defaultPackageName } from '../format';
  import type { CreateResult, ValidationResult } from '../types';


  let outputParent = $state<string | null>(null);
  let packageName = $state('');
  let creating = $state(false);
  let reVerifying = $state(false);
  /** Save-as-copy switches an opened .mpack to the classic new-package
   * form; by default an opened package saves back into itself. */
  let copyFlow = $state(false);
  let panel: HTMLDivElement | null = $state(null);

  const editingPackage = $derived($draft?.openedFrom ?? null);

  onMount(() => {
    const fn = (e: KeyboardEvent): void => {
      if (e.key === 'Escape') close();
      // keep Tab cycling inside the modal
      if (e.key === 'Tab' && panel) {
        const focusable = Array.from(
          panel.querySelectorAll<HTMLElement>(
            'button, input, select, [tabindex]:not([tabindex="-1"])',
          ),
        );
        if (focusable.length === 0) return;
        const first = focusable[0] as HTMLElement;
        const last = focusable[focusable.length - 1] as HTMLElement;
        if (e.shiftKey && document.activeElement === first) {
          e.preventDefault();
          last.focus();
        } else if (!e.shiftKey && document.activeElement === last) {
          e.preventDefault();
          first.focus();
        }
      }
    };
    window.addEventListener('keydown', fn);
    return () => window.removeEventListener('keydown', fn);
  });

  function mount(node: HTMLDivElement): void {
    panel = node;
    node.querySelector<HTMLElement>('button')?.focus();
  }

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
    if (!d) return;
    const inPlace = editingPackage !== null && !copyFlow;
    const out = inPlace ? editingPackage : outputPath();
    if (!out) return;
    creating = true;
    let result: CreateResult;
    try {
      result = await api.createPackage(d, out, {
        replace: inPlace,
        syncTags: inPlace,
      });
    } catch (e) {
      result = {
        ok: false,
        error: { code: 'create_failed', message: e instanceof Error ? e.message : 'Create failed.' },
      };
    }
    creating = false;
    if (result.ok) {
      // the built package no longer needs its encode staging area, and the
      // authoring session is complete — drop the autosaved draft too
      void api.draftClear().catch(() => {});
      const staging = encodeStaging.get();
      if (staging) {
        encodeStaging.set(null);
        try {
          await api.cleanupStaging(staging);
        } catch {
          /* the temp directory is eventually reclaimed; not a user error */
        }
      }
      if (inPlace) {
        // the saved package is the new truth (audio files may even have been
        // renamed after a retitle): reopen it so the editor shows that state.
        // Must happen before the result is shown — setDraft() resets the
        // shared session state, including the result itself.
        try {
          const fresh = await api.inspectAlbum(d.openedFrom!);
          draftStore.setDraft(fresh);
          void api.recentsAdd(d.openedFrom!, fresh.album?.title).catch(() => {});
        } catch {
          /* keep the edited draft in memory; reopening can happen later */
        }
      }
    }
    createResult.set(result);
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
    // Reset the result and both flows so reopening the dialog always starts
    // fresh (no stale success/error state, no stale output path).
    createOpen.set(false);
    createResult.set(null);
    outputParent = null;
    packageName = '';
    creating = false;
    reVerifying = false;
    copyFlow = false;
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
      use:mount
    >
      {#if $createResult}
        {@const r = $createResult}
        <h2>{r.ok ? (r.replaced ? 'Package updated' : 'Package created') : 'Package creation failed'}</h2>
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
      {:else if editingPackage && !copyFlow}
        <h2>Save changes</h2>
        <p class="muted">
          Your edits are written back into the opened package. The audio is
          already encoded and stays untouched; embedded tags are re-projected
          from the manifest where they differ.
        </p>
        <p class="path">{editingPackage}</p>
        <div class="artwork-row">
          <button class="btn" onclick={runCreate} disabled={creating}>
            {creating ? 'Saving…' : 'Save changes'}
          </button>
          <button class="btn ghost" onclick={() => (copyFlow = true)} disabled={creating}>
            Save as copy…
          </button>
          <button class="btn ghost" onclick={close}>Cancel</button>
        </div>
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
