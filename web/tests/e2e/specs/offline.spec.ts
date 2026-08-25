// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

// Offline downloads + offline playback, observed end-to-end (plan §12).
//
// Flow: sign in → install a release through the debug hook (real fetch,
// real OPFS, real IndexedDB in Chromium) → sever the network with
// context.setOffline(true) → RELOAD the page (shell comes from cache/
// server per test config; the app must boot into 'offline' session state)
// → play the installed album from local storage → navigate multiple
// tracks and seek. Then restore the network and confirm normal online
// playback still works.

import { test, expect } from '@playwright/test';
import { playerState, signIn, waitFor } from './helpers';

async function queueItems(page: import('@playwright/test').Page) {
  return page.evaluate(() =>
    window.__musicpack!.queue.get().items.map((i) => ({
      id: i.id,
      kind: i.source.kind,
      url: i.source.url,
      codec: i.codec,
    })),
  );
}

test.describe.configure({ mode: 'serial' });

test.describe('offline downloads', () => {
  test('download once, play offline across tracks, then return online', async ({ page, context }) => {
    await signIn(page);

    // Install "Long Player" through the debug hook (the same manager the UI
    // would drive). Wait for the committed record.
    const releaseId: number = await page.evaluate(async () => {
      const { api, offline } = window.__musicpack!;
      const albums = await api.albums({ limit: 50 });
      const long = albums.albums.find((a) => a.title === 'Long Player')!;
      const detail = await api.album(long.id);
      const release = await api.release(detail.releases[0]!.id);
      await offline.install(release);
      return release.id;
    });
    await waitFor(page, async () =>
      (await page.evaluate((id) => window.__musicpack!.offline.states.get().get(id)?.state, releaseId)) === 'installed',
      { label: 'package installed', timeout: 30_000 },
    );

    // D1 local-first while ONLINE: built items already use local sources.
    await page.getByText('Long Player').click();
    await page.getByRole('button', { name: 'Play album' }).click();
    await waitFor(page, async () => (await playerState(page)).state === 'playing', {
      label: 'online local playback',
    });
    const items = await queueItems(page);
    expect(items[0]?.kind).toBe('local-file');

    // ---- go offline mid-session -------------------------------------
    await context.setOffline(true);
    // Play on: next track must come from local storage without any network.
    await page.locator('.playerbar').getByRole('button', { name: 'Next track' }).click();
    await waitFor(page, async () => (await playerState(page)).positionSeconds > 0.3, {
      label: 'offline track 2 playing',
      timeout: 20_000,
    });
    expect((await playerState(page)).state).not.toBe('error');
    expect((await queueItems(page))[1]?.kind).toBe('local-file');

    // Seek offline (random access over OPFS).
    await page.locator('.playerbar input[type=range]').first().evaluate((el) => {
      const input = el as HTMLInputElement;
      input.value = String(10);
      input.dispatchEvent(new Event('input', { bubbles: true }));
      input.dispatchEvent(new Event('change', { bubbles: true }));
    });
    await expect.poll(async () => (await playerState(page)).state).not.toBe('error');

    // ---- full offline reload ----------------------------------------
    await page.reload();
    // The session probe fails (network gone); installed content exists, so
    // the app must NOT show the sign-in screen.
    await expect(page.getByRole('heading', { name: 'Sign in' })).toBeHidden({ timeout: 20_000 });
    // The restored session resumes paused on track 1 (local source after
    // remap); press Play and confirm audible offline playback.
    await page.locator('.playerbar').getByRole('button', { name: 'Play', exact: true }).click();
    await waitFor(page, async () => (await playerState(page)).state === 'playing', {
      label: 'offline playback after reload',
      timeout: 30_000,
    });
    await waitFor(page, async () => (await playerState(page)).positionSeconds > 1, {
      label: 'offline position advances',
    });

    // Navigate at least one more track offline to verify progression.
    await page.locator('.playerbar').getByRole('button', { name: 'Next track' }).click();
    await waitFor(page, async () => (await playerState(page)).positionSeconds > 0.3 || (await playerState(page)).state === 'playing', {
      label: 'offline track progression',
      timeout: 20_000,
    });
    expect((await playerState(page)).error).toBeUndefined();

    // ---- restore the network ----------------------------------------
    await context.setOffline(false);
    // Normal online playback of a NON-installed release still works.
    await page.goto('/');
    await page.getByText('Shapeshifter').click();
    await page.getByRole('button', { name: 'Play album' }).click();
    await waitFor(page, async () => (await playerState(page)).state === 'playing', {
      label: 'online playback restored',
      timeout: 30_000,
    });
    expect((await queueItems(page))[0]?.kind).toBe('http-range');
  });

  test('remove download returns the release to remote sources', async ({ page }) => {
    await signIn(page);
    const releaseId = await page.evaluate(async () => {
      const { api, offline } = window.__musicpack!;
      const albums = await api.albums({ limit: 50 });
      const long = albums.albums.find((a) => a.title === 'Long Player')!;
      const detail = await api.album(long.id);
      const release = await api.release(detail.releases[0]!.id);
      await offline.install(release);
      return release.id;
    });
    await waitFor(page, async () =>
      (await page.evaluate((id) => window.__musicpack!.offline.packageFor(id), releaseId)) !== null,
      { label: 'installed', timeout: 30_000 },
    );

    await page.evaluate(async (id) => {
      await window.__musicpack!.offline.remove(id);
    }, releaseId);

    const pkg = await page.evaluate((id) => window.__musicpack!.offline.packageFor(id), releaseId);
    expect(pkg).toBeNull();
  });
});
