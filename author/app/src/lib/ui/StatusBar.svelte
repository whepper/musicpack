<!--
Copyright (c) 2026, The MusicPack Development Team
SPDX-License-Identifier: BSD-3-Clause
-->

<script lang="ts">
  import { api, draft, draftStore } from '../bootstrap';
  import { chipState } from '../draft-store';
  import { createOpen, validating, validation } from '../authoring-state';


  async function runValidate(): Promise<void> {
    const d = draft.get();
    if (!d) return;
    validating.set(true);
    try {
      validation.set(await api.validateDraft(d));
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

  function openCreate(): void {
    if (validation.get()?.ok) createOpen.set(true);
  }
</script>

{#if $draft}
  {@const c = chipState($draft)}
  <footer class="statusbar">
    <div class="chips">
      <span class="chip {c.audio}">Audio {c.audio === 'ok' ? '✓' : '?'}</span>
      <span class="chip {c.metadata}">Metadata {c.metadata === 'ok' ? '✓' : '?'}</span>
      <span class="chip {c.artwork}">Artwork {c.artwork === 'ok' ? '✓' : '?'}</span>
      <span class="chip idle">Loudness · at build</span>
      <span class="chip {c.identity}">
        Identity {c.identity === 'ok' ? '✓' : c.identity === 'warn' ? '≈' : '?'}
      </span>
      <span class="chip {c.sonic}">
        Sonic {c.sonic === 'ok' ? '✓' : c.sonic === 'warn' ? '≈' : '· not analysed'}
      </span>
      <span class="chip {c.waveform}">
        Waveform {c.waveform === 'ok' ? '✓' : c.waveform === 'warn' ? '⚠' : '· not generated'}
      </span>
    </div>
    <div class="actions">
      <button class="btn ghost" onclick={runValidate} disabled={$validating}>
        {$validating ? 'Validating…' : 'Validate'}
      </button>
      <button class="btn" onclick={openCreate} disabled={!$validation?.ok}>
        Create MusicPack
      </button>
    </div>
  </footer>
{/if}
