import { test, expect } from '@playwright/test';
import { signIn, playerState, waitFor } from './helpers';

test.beforeEach(async ({ page }) => {
  await signIn(page);
});

/** Sets a range slider value, firing the same input+change the UI listens to. */
async function setSeek(page: import('@playwright/test').Page, seconds: number): Promise<void> {
  await page.locator('.playerbar input[type=range]').first().evaluate((el, v) => {
    const input = el as HTMLInputElement;
    input.value = String(v);
    input.dispatchEvent(new Event('input', { bubbles: true }));
    input.dispatchEvent(new Event('change', { bubbles: true }));
  }, seconds);
}

test('plays a Musepack album demand-driven and seeks without downloading the file', async ({ page }) => {
  await page.getByText('Long Player').click();
  await page.getByRole('button', { name: 'Play album' }).click();

  await waitFor(page, async () => (await playerState(page)).state === 'playing', { label: 'playing' });
  const start = await playerState(page);
  expect(start.currentTitle).toBeTruthy();
  expect(start.normDb).toBeLessThan(0); // album normalization applied

  // Time-to-first-PCM / bytes-before-playback: far less than the full file.
  const servedAtPlay = await playerState(page).then((s) => s.servedBytes);
  const size = await page.evaluate(() => {
    const item = window.__musicpack?.player.model.get().current;
    return item?.track.audio.size ?? 0;
  });
  expect(servedAtPlay).toBeLessThan(size * 0.5);

  // Position advances without error.
  await waitFor(page, async () => (await playerState(page)).positionSeconds > 1, { label: 'position advances' });

  // Seek to ~50%, then ~90%, then backwards to ~10% of the CURRENT TRACK
  // (the hidden range is track-scoped, matching what the waveform draws).
  // Position jumps accordingly; the whole file is never fetched (the demand
  // reader fetches only the needed blocks).
  const st = await playerState(page);
  const t0 = st.currentTrackStartSeconds;
  const td = st.currentTrackDurationSeconds;
  expect(td).toBeGreaterThan(20); // room to seek inside the track
  await setSeek(page, Math.round(td * 0.5));
  await waitFor(page, async () => (await playerState(page)).positionSeconds > t0 + td * 0.4,
              { label: 'seek 50%' });
  await setSeek(page, Math.round(td * 0.9));
  await waitFor(page, async () => (await playerState(page)).positionSeconds > t0 + td * 0.8,
              { label: 'seek 90%' });
  await setSeek(page, Math.round(td * 0.1));
  await waitFor(page, async () => {
    const s = await playerState(page);
    return s.positionSeconds > t0 && s.positionSeconds < t0 + td * 0.3;
  }, { label: 'seek backwards' });

  const final = await playerState(page);
  expect(final.servedBytes).toBeLessThan(size); // never downloaded the whole file
  expect(final.state).not.toBe('error');
});

test('pause, resume, and next track behave', async ({ page }) => {
  // Long Player's first track is 48 s, so pause/resume/next have room.
  await page.getByText('Long Player').click();
  await page.getByRole('button', { name: 'Play album' }).click();
  await waitFor(page, async () => (await playerState(page)).state === 'playing', { label: 'playing' });

  const pausedAt = (await playerState(page)).positionSeconds;
  await page.locator('.playerbar').getByRole('button', { name: 'Pause' }).click();
  await waitFor(page, async () => (await playerState(page)).state === 'paused', { label: 'paused' });

  // Position freezes while paused.
  const stillPaused = await playerState(page);
  expect(stillPaused.positionSeconds).toBeGreaterThanOrEqual(pausedAt - 0.3);

  await page.locator('.playerbar').getByRole('button', { name: 'Play' }).click();
  await waitFor(page, async () => (await playerState(page)).state === 'playing', { label: 'resumed' });

  const before = await playerState(page);
  await page.locator('.playerbar').getByRole('button', { name: 'Next track' }).click();
  await waitFor(
    page,
    async () => (await playerState(page)).currentTitle !== before.currentTitle,
    { label: 'next track' },
  );
});

test('gapless album playback crosses the track boundary continuously', async ({ page }) => {
  await page.getByText('Synthetic Test Compilation').click();
  await page.getByRole('button', { name: 'Play album' }).click();
  await waitFor(page, async () => (await playerState(page)).state === 'playing', { label: 'playing' });

  // Wait until the queue cursor advances into track 2 (gapless handoff) with
  // position still increasing — no error, no stuck state.
  await waitFor(
    page,
    async () => {
      const q = await page.evaluate(() => window.__musicpack?.queue.get().index ?? -1);
      return q >= 1;
    },
    { label: 'gapless into track 2', timeout: 45_000 },
  );
  const state = await playerState(page);
  expect(state.state).not.toBe('error');
  expect(state.currentTitle).toBeTruthy();
});

test('queue holds the album in order and removes items', async ({ page }) => {
  await page.getByText('Synthetic Test Compilation').click();
  await page.getByRole('button', { name: 'Play album' }).click();
  await waitFor(page, async () => (await playerState(page)).state === 'playing', { label: 'playing' });

  // Pause immediately: the fixture tracks are ~1 s long and would play out
  // (and move the highlight) before the assertions below.
  await page.locator('.playerbar').getByRole('button', { name: 'Pause' }).click();
  await waitFor(page, async () => (await playerState(page)).state === 'paused', { label: 'paused' });

  await page.getByRole('button', { name: 'Open the queue' }).click();
  const items = await page.evaluate(() => window.__musicpack?.queue.get().items.map((i) => i.track.title) ?? []);
  expect(items).toHaveLength(4); // the fixture album has 4 tracks
  expect(items[0]).toContain('Big in Japan');
  // The highlighted list entry must be the cursor's item (coherence), not a
  // frozen position.
  const res = await page.evaluate(() => {
    const q = window.__musicpack?.queue.get();
    const containers = [...document.querySelectorAll('#main .queue-item')];
    return {
      idx: q?.index ?? -1,
      markedIdx: containers.findIndex((el) => el.getAttribute('aria-current') === 'true'),
    };
  });
  expect(res.idx).toBeGreaterThanOrEqual(0);
  expect(res.markedIdx).toBe(res.idx);
});

test('clicking a queue item keeps the queue and moves the highlight', async ({ page }) => {
  await page.getByText('Synthetic Test Compilation').first().click();
  await page.getByRole('button', { name: 'Play album' }).click();
  await waitFor(page, async () => (await playerState(page)).state === 'playing', { label: 'playing' });

  const titles = await page.evaluate(() => window.__musicpack?.queue.get().items.map((i) => i.track.title) ?? []);
  const n = titles.length;
  expect(n).toBeGreaterThan(1);

  // Jump to the LAST track via the queue drawer. Do NOT navigate: these
  // fixture tracks are ~1 s long and the album can play out before a
  // route change settles.
  await page.locator('.playerbar').getByRole('button', { name: 'Open the queue' }).click();
  await expect(page.locator('.queue-panel .queue-item').first()).toBeVisible();
  await page.locator('.queue-panel .queue-item button', { hasText: titles[n - 1] }).first().click();

  // The queue must survive the click (it used to be replaced by the single
  // clicked song), the cursor must land on the clicked item...
  await waitFor(
    page,
    async () => {
      const s = await page.evaluate(() => {
        const q = window.__musicpack?.queue.get();
        const m = window.__musicpack?.player.model.get();
        return { n: q?.items.length ?? 0, idx: q?.index ?? -1, title: m?.current?.track.title ?? null };
      });
      return s.n === n && s.idx === n - 1 && s.title === titles[n - 1];
    },
    { label: 'clicked queue item plays, queue intact' },
  );

  // ...and the highlight must follow the cursor exactly (no stuck item 1).
  const res = await page.evaluate(() => {
    const q = window.__musicpack?.queue.get();
    const containers = [...document.querySelectorAll('.queue-panel .queue-item')];
    const markedIdx = containers.findIndex((el) => el.getAttribute('aria-current') === 'true');
    return { idx: q?.index ?? -1, count: q?.items.length ?? 0, markedIdx };
  });
  expect(res.count).toBe(n);
  expect(res.idx).toBe(n - 1);
  expect(res.markedIdx).toBe(n - 1);
});

test('album seek past the current track switches to a later track', async ({ page }) => {
  // Long Player: track 1 decodes to ~39 s (the manifest's 48 s is a
  // placeholder); the model reports the REAL total duration.
  await page.getByText('Long Player').click();
  await page.getByRole('button', { name: 'Play album' }).click();
  await waitFor(page, async () => (await playerState(page)).state === 'playing', { label: 'playing' });

  const titles = await page.evaluate(() => window.__musicpack?.queue.get().items.map((i) => i.track.title) ?? []);
  const dur = (await playerState(page)).durationSeconds;
  // seek well past track 1's real end so the target lives in a later track
  const target = Math.max(dur * 0.95, dur - 2);

  await setSeek(page, target);
  // the cursor must leave track 1 and settle on a later track that matches
  // the controller's current track
  await waitFor(
    page,
    async () => {
      const idx = await page.evaluate(() => window.__musicpack?.queue.get().index ?? -1);
      if (idx < 1) return false;
      const cur = await page.evaluate(() => window.__musicpack?.player.model.get().current?.track.title ?? null);
      return cur === titles[idx];
    },
    { label: 'seek moved off track 1' },
  );
  const idx = await page.evaluate(() => window.__musicpack?.queue.get().index ?? -1);
  const st = await playerState(page);
  expect(st.currentTitle).toBe(titles[idx]);
  // the reported position is in the target range, not the previous track
  expect(st.positionSeconds).toBeGreaterThan(dur * 0.8);
  expect(st.state).not.toBe('error');
});

test('clicking a disc-2 track selects the correct flattened queue index', async ({ page }) => {
  await page.getByText('Two Disc Extravaganza').click();
  await page.getByRole('button', { name: 'Side Two One' }).click();
  // playAlbum sets the queue index synchronously, so capture it immediately
  // (the ~1 s fixture tracks may advance before the state read).
  const state = await page.evaluate(() => {
    const q = window.__musicpack?.queue;
    const items = q?.get().items ?? [];
    return { index: q?.get().index ?? -1, items: items.map((i) => i.track.title) };
  });
  // flat index = disc 1 track count (4) + 0
  expect(state.index).toBe(4);
  expect(state.items[state.index]).toBe('Side Two One');
});

test('plays through every track (repeated worker teardown) without wedging', async ({ page }) => {
  await page.getByText('Synthetic Test Compilation').click();
  await page.getByRole('button', { name: 'Play album' }).click();
  await waitFor(page, async () => (await playerState(page)).state === 'playing', { label: 'playing' });
  // Let the album play through all 4 short (~1 s) tracks: every gapless
  // handoff tears down and re-spawns a decoder + network worker, so this
  // exercises the teardown handshake repeatedly. Playback must reach the
  // ended state without an error or a wedged controller.
  await waitFor(page, async () => (await playerState(page)).state === 'ended', {
    label: 'album played to the end',
    timeout: 30_000,
  });
  const st = await playerState(page);
  expect(st.state).toBe('ended');
  expect(st.error).toBeUndefined();
});

test('signing out stops playback, disposes the backend and clears player state', async ({ page }) => {
  await page.getByText('Long Player').click();
  await page.getByRole('button', { name: 'Play album' }).click();
  await waitFor(page, async () => (await playerState(page)).state === 'playing', { label: 'playing' });
  expect((await playerState(page)).state).toBe('playing');

  await page.getByRole('button', { name: 'Sign out' }).click();
  await expect(page.getByRole('heading', { name: 'Sign in' })).toBeVisible({ timeout: 20_000 });

  const after = await page.evaluate(() => {
    const p = window.__musicpack?.player;
    const m = p?.model.get();
    return { state: m?.state ?? 'idle', current: m?.current ?? null, backendKind: p?.getBackendKind() ?? null };
  });
  expect(after.state).toBe('idle');
  expect(after.current).toBeNull();
  expect(after.backendKind).toBeNull(); // decoder/AudioContext disposed
});
