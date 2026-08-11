// @vitest-environment jsdom
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import SonicPanel from '../../app/src/lib/ui/SonicPanel.svelte';
import { api, draftStore } from '../../app/src/lib/bootstrap';
import { render, click, type RenderResult } from './helpers';
import type { Draft } from '../../app/src/lib/types';

function draft(): Draft {
  return {
    schema: 'musicpack-draft',
    version: 1,
    sourceRoot: '/music/A',
    album: { title: 'Album', artists: [{ name: 'Artist' }] },
    media: [
      { disc: 1, tracks: [{ track: 1, title: 'A', audioPath: '1.mpc' }, { track: 2, title: 'B', audioPath: '2.mpc' }] },
    ],
    artwork: [],
    booklet: [],
    lyrics: [],
    extras: [],
  };
}

let view: RenderResult;

describe('SonicPanel', () => {
  beforeEach(() => {
    draftStore.clear();
    draftStore.setDraft(draft());
    vi.restoreAllMocks();
  });
  afterEach(() => view?.cleanup());

  it('starts not analysed', () => {
    view = render(SonicPanel);
    expect(view.target.textContent).toContain('Not analysed');
    const btn = view.queryAll('button').find((b) => (b.textContent ?? '').includes('Analyse Sonic'));
    expect(btn).toBeTruthy();
    expect(draftStore.draft.get()?.sonicAnalysis).toBeUndefined();
  });

  it('runs analysis and marks the draft ready with the profile', async () => {
    vi.spyOn(api, 'sonicAnalyze').mockResolvedValue({
      ok: true,
      profile: 'musicpack-sonic-openl3-v1',
      outputPath: '/data/sonic.json',
      tracks: 2,
      contributing: 2,
    });
    view = render(SonicPanel);
    const btn = view.queryAll('button').find((b) => (b.textContent ?? '').includes('Analyse Sonic'))!;
    await click(btn);
    expect(api.sonicAnalyze).toHaveBeenCalledOnce();
    expect(draftStore.draft.get()?.sonicAnalysis?.status).toBe('ready');
    expect(draftStore.draft.get()?.sonicAnalysis?.profile).toBe('musicpack-sonic-openl3-v1');
    expect(draftStore.draft.get()?.sonicAnalysis?.path).toBe('/data/sonic.json');
  });

  it('shows ready state with the profile label', async () => {
    draftStore.updateSonicAnalysis((s) => {
      s.status = 'ready';
      s.profile = 'musicpack-sonic-openl3-v1';
      s.tracksAnalysed = 2;
      s.tracksTotal = 2;
    });
    view = render(SonicPanel);
    expect(view.target.textContent).toContain('Ready');
    expect(view.target.textContent).toContain('MusicPack OpenL3 v1');
    expect(view.target.textContent).toContain('2 / 2 tracks');
    const btn = view.queryAll('button').find((b) => (b.textContent ?? '').includes('Re-analyse'));
    expect(btn).toBeTruthy();
  });

  it('shows ready-with-warnings for partial analysis', () => {
    draftStore.updateSonicAnalysis((s) => {
      s.status = 'ready-with-warnings';
      s.profile = 'musicpack-sonic-openl3-v1';
      s.tracksAnalysed = 1;
      s.tracksTotal = 2;
      s.warnings = ['disc 1 track 2 has no embedding'];
    });
    view = render(SonicPanel);
    expect(view.target.textContent).toContain('Ready with warnings');
    expect(view.target.textContent).toContain('has no embedding');
  });

  it('shows analysis failure', () => {
    draftStore.updateSonicAnalysis((s) => {
      s.status = 'error';
      s.error = 'model unavailable';
    });
    view = render(SonicPanel);
    expect(view.target.textContent).toContain('Analysis failed');
    expect(view.target.textContent).toContain('model unavailable');
    const btn = view.queryAll('button').find((b) => (b.textContent ?? '').includes('Retry'));
    expect(btn).toBeTruthy();
  });

  it('marks not_analysed on cancel', async () => {
    vi.spyOn(api, 'sonicCancel').mockResolvedValue(undefined);
    vi.spyOn(api, 'sonicAnalyze').mockResolvedValue({ ok: false, cancelled: true });
    view = render(SonicPanel);
    const btn = view.queryAll('button').find((b) => (b.textContent ?? '').includes('Analyse Sonic'))!;
    await click(btn);
    expect(draftStore.draft.get()?.sonicAnalysis?.status).toBe('not_analysed');
  });
});
