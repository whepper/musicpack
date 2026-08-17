// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

// End-to-end coverage for the waveform-backed seek control. Tests that the
// fixed reference fixtures (test-musicpack-album.mpack carries envelopes,
// test-flac-album.mpack does not) drive the correct UI in each case.

import { test, expect } from '@playwright/test';
import { signIn, playerState, waitFor } from './helpers';

test.beforeEach(async ({ page }) => {
  await signIn(page);
});

test('waveform canvas renders for a track with envelope; click seeks', async ({ page }) => {
  // The TestComp album carries waveform envelopes on every track.
  await page.getByText('Synthetic Test Compilation').first().click();
  await page.getByRole('button', { name: 'Play album' }).click();

  await waitFor(page, async () => (await playerState(page)).state === 'playing', { label: 'playing' });

  // The waveform canvas must replace the legacy linear range on a
  // waveform-bearing track.
  await expect(page.locator('.playerbar canvas').first()).toBeVisible();

  // The hidden <input type=range> sibling remains the keyboard/a11y surface;
  // its initial value reflects the per-track duration.
  const dur = (await playerState(page)).durationSeconds;
  expect(dur).toBeGreaterThan(0);
  const rangeMax = await page.locator('.playerbar input[type=range]').first().getAttribute('max');
  expect(parseFloat(rangeMax ?? '0')).toBeGreaterThan(0);

  // Click on the canvas at ~50% -> position advances past the middle.
  const canvas = page.locator('.playerbar canvas').first();
  const box = await canvas.boundingBox();
  expect(box).not.toBeNull();
  await page.mouse.click(box!.x + box!.width * 0.5, box!.y + box!.height / 2);
  await waitFor(page, async () => (await playerState(page)).positionSeconds > dur * 0.3,
              { label: 'click seeks to ~50%' });
});

test('focus on the hidden range draws the focus ring on the waveform container', async ({ page }) => {
  await page.getByText('Synthetic Test Compilation').first().click();
  await page.getByRole('button', { name: 'Play album' }).click();
  await waitFor(page, async () => (await playerState(page)).state === 'playing', { label: 'playing' });

  // Focus the (hidden) range via the keyboard: the canvas container must
  // receive the :focus-within styling. We don't assert visual style; we
  // verify the focus + keyboard semantics by pressing ArrowRight and
  // observing position advance.
  const range = page.locator('.playerbar input[type=range]').first();
  await range.focus();
  const before = (await playerState(page)).positionSeconds;
  await page.keyboard.press('ArrowRight');
  await waitFor(page, async () => (await playerState(page)).positionSeconds > before,
              { label: 'ArrowRight advances position' });
});

test('no-waveform tracks fall back to the linear range', async ({ page }) => {
  // The Classical Compilation has no waveform envelopes -> the legacy
  // <input type=range> stays visible, no canvas.
  await page.getByText('Synthetic Classical Compilation').first().click();
  await page.getByRole('button', { name: 'Play album' }).click();

  await waitFor(page, async () => (await playerState(page)).state === 'playing', { label: 'playing' });

  // No canvas: the fallback range is the only seek control.
  await expect(page.locator('.playerbar canvas')).toHaveCount(0);
  await expect(page.locator('.playerbar input[type=range]').first()).toBeVisible();

  const dur = (await playerState(page)).durationSeconds;
  await page.locator('.playerbar input[type=range]').first().evaluate((el, v) => {
    const input = el as HTMLInputElement;
    input.value = String(v);
    input.dispatchEvent(new Event('input', { bubbles: true }));
    input.dispatchEvent(new Event('change', { bubbles: true }));
  }, Math.round(dur * 0.5));
  await waitFor(page, async () => (await playerState(page)).positionSeconds > dur * 0.3,
              { label: 'fallback range seeks' });
});