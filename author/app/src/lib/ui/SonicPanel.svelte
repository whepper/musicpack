<script lang="ts">
  import { api, draft, draftStore } from '../bootstrap';
  import type { SonicProgress } from '../types';

  /** The profiles exposed in the UI. Only the permissive openl3-v1 profile is
   * shipped; the list keeps future profiles selectable without a redesign.
   * Research/restricted profiles are intentionally not exposed. */
  const PROFILES: { id: string; label: string }[] = [
    { id: 'musicpack-sonic-openl3-v1', label: 'MusicPack OpenL3 v1' },
  ];
  const DEFAULT_PROFILE = PROFILES[0] as { id: string; label: string };

  let running = $state(false);
  let done = $state(0);
  let total = $state(0);

  function selectedProfile(): { id: string; label: string } {
    const id = $draft?.sonicAnalysis?.profile ?? DEFAULT_PROFILE.id;
    return PROFILES.find((p) => p.id === id) ?? DEFAULT_PROFILE;
  }

  function trackCount(): number {
    return $draft ? $draft.media.reduce((n, m) => n + m.tracks.length, 0) : 0;
  }

  async function analyse(): Promise<void> {
    const d = draft.get();
    if (!d || running) return;
    running = true;
    const n = trackCount();
    done = 0;
    total = n;
    draftStore.updateSonicAnalysis((s) => {
      s.status = 'pending';
      s.profile = selectedProfile().id;
      s.tracksTotal = n;
      s.tracksAnalysed = 0;
      s.error = undefined;
      s.warnings = undefined;
    });
    let warnings: string[] = [];
    try {
      const result = await api.sonicAnalyze(d, (p: SonicProgress) => {
        if (p.event === 'track') {
          done = p.done ?? done;
          total = p.total ?? total;
          draftStore.updateSonicAnalysis((s) => {
            s.status = 'pending';
            s.tracksAnalysed = p.done ?? 0;
            s.tracksTotal = p.total ?? s.tracksTotal;
          });
          if (p.status === 'no-embedding')
            warnings.push(`disc ${p.disc} track ${p.track} has no embedding`);
        }
      });
      if (result.cancelled) {
        draftStore.updateSonicAnalysis((s) => {
          s.status = 'not_analysed';
          s.path = undefined;
          s.tracksAnalysed = undefined;
        });
      } else if (result.ok) {
        draftStore.updateSonicAnalysis((s) => {
          s.status = warnings.length > 0 ? 'ready-with-warnings' : 'ready';
          s.path = result.outputPath;
          s.tracksAnalysed = result.tracks ?? n;
          s.tracksTotal = result.tracks ?? n;
          s.warnings = warnings.length > 0 ? warnings : undefined;
          s.error = undefined;
        });
      }
    } catch (e) {
      draftStore.updateSonicAnalysis((s) => {
        s.status = 'error';
        s.error = e instanceof Error ? e.message : 'Sonic analysis failed';
      });
    } finally {
      running = false;
    }
  }

  async function cancel(): Promise<void> {
    try {
      await api.sonicCancel();
    } catch {
      /* the run will end on its own */
    }
  }
</script>

{#if $draft}
  <section class="section">
    <h2>Sonic Analysis</h2>
    <p class="muted">
      Content-based similarity vectors, computed once at authoring time with the
      <strong>{selectedProfile().label}</strong> profile and stored in the package.
    </p>

    {#if running}
      <p><span class="chip idle">◐</span> Analysing {done} / {total} tracks…</p>
      <button class="btn ghost" onclick={cancel}>Cancel</button>
    {:else}
      {@const s = $draft.sonicAnalysis}
      {#if !s}
        <p>○ Not analysed</p>
        <button class="btn" onclick={analyse}>Analyse Sonic</button>
      {:else if s.status === 'pending'}
        <p><span class="chip idle">◐</span> Analysing…</p>
        <button class="btn ghost" onclick={cancel}>Cancel</button>
      {:else if s.status === 'ready' || s.status === 'ready-with-warnings'}
        <p>
          <span class="chip {s.status === 'ready' ? 'ok' : 'warn'}">
            {s.status === 'ready' ? '●' : '⚠'}
          </span>
          {s.status === 'ready' ? 'Ready' : 'Ready with warnings'}
        </p>
        <p class="muted">
          {s.profile} · {s.tracksAnalysed ?? 0} / {s.tracksTotal ?? 0} tracks
        </p>
        {#if s.warnings?.length}
          <ul>
            {#each s.warnings as w}
              <li class="warn">{w}</li>
            {/each}
          </ul>
        {/if}
        <button class="btn ghost" onclick={analyse}>Re-analyse</button>
      {:else if s.status === 'error'}
        <p><span class="chip warn">✕</span> Analysis failed</p>
        {#if s.error}
          <p class="muted">{s.error}</p>
        {/if}
        <button class="btn" onclick={analyse}>Retry Sonic Analysis</button>
      {:else}
        <p>○ Not analysed</p>
        <button class="btn" onclick={analyse}>Analyse Sonic</button>
      {/if}
    {/if}
  </section>
{/if}
