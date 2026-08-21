import { test, expect } from '@playwright/test';
import { signIn, playerState, waitFor } from './helpers';

test.beforeEach(async ({ page }) => {
  await signIn(page);
});

test('plays a native-codec (FLAC) track through the browser backend', async ({ page }) => {
  await page.getByText('Synthetic Classical Compilation').click();
  await page.getByRole('button', { name: 'Play album' }).click();
  await waitFor(page, async () => (await playerState(page)).state === 'playing', { label: 'playing' });
  await waitFor(page, async () => (await playerState(page)).positionSeconds > 0.5, { label: 'position advances' });

  const kind = await page.evaluate(() => window.__musicpack?.player.getBackendKind());
  expect(kind).toBe('native');
  const state = await playerState(page);
  expect(state.state).not.toBe('error');
});

test('exposes Media Session metadata and a track-relative position state', async ({ page }) => {
  // Capture setPositionState calls (write-only API -> wrap it before app boot).
  await page.addInitScript(() => {
    const w = window as unknown as Record<string, unknown>;
    w.__posStates = [];
    const ms = navigator.mediaSession;
    if (!ms) return;
    const orig = ms.setPositionState?.bind(ms);
    Object.defineProperty(ms, 'setPositionState', {
      configurable: true,
      value: (s: MediaPositionState) => {
        (w.__posStates as MediaPositionState[]).push(s);
        orig?.(s);
      },
    });
  });
  await page.getByText('Synthetic Test Compilation').click();
  await page.getByRole('button', { name: 'Play album' }).click();
  await waitFor(page, async () => (await playerState(page)).state === 'playing', { label: 'playing' });

  const meta = await page.evaluate(() => {
    const m = navigator.mediaSession?.metadata;
    return m ? { title: m.title, artist: m.artist, album: m.album } : null;
  });
  expect(meta?.title).toBeTruthy();
  expect(meta?.album).toBe('Synthetic Test Compilation');
  expect(meta?.artist).toBe('Alphaville');

  // The reported position state must describe the CURRENT TRACK, not an
  // album-spanning timeline: OS scrubbers derive their seekTo times from
  // it, and album-scale values send media-key seeks to the wrong place.
  const last = await page.evaluate(() => {
    const arr = (window as unknown as { __posStates?: MediaPositionState[] }).__posStates ?? [];
    return arr[arr.length - 1] ?? null;
  });
  expect(last).not.toBeNull();
  const st = await playerState(page);
  expect(last!.duration).toBeGreaterThan(0);
  expect(last!.duration).toBeLessThan(st.durationSeconds); // track < album
  expect(last!.position).toBeGreaterThanOrEqual(0);
  expect(last!.position).toBeLessThanOrEqual(last!.duration + 0.25);
});

test('an invalid session returns to the sign-in screen (re-auth)', async ({ page }) => {
  // Poison the HttpOnly cookie and load the app — the boot session probe 401s
  // and the UI falls back to the sign-in screen (no raw error strings).
  await page.context().addCookies([
    { name: 'musicpack_session', value: 'definitely-not-a-session', domain: '127.0.0.1', path: '/' },
  ]);
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Sign in' })).toBeVisible({ timeout: 20_000 });
  await expect(page.getByLabel('Server token')).toBeVisible();
});

test('a missing album shows a friendly error, not an internal string', async ({ page }) => {
  // beforeEach signs in; now an unknown album id must surface a friendly error.
  await page.goto('/albums/999999');
  await expect(page.getByRole('heading', { name: 'The shelf' })).toBeHidden();
  await expect(page.getByText('no longer in the collection', { exact: false }).first()).toBeVisible({ timeout: 20_000 });
});
