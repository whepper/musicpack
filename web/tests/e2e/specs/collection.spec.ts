import { test, expect } from '@playwright/test';
import { signIn } from './helpers';

test.beforeEach(async ({ page }) => {
  await signIn(page);
});

test('the shelf shows the collection with collector lines', async ({ page }) => {
  const cards = page.locator('.album-card');
  await expect(cards).toHaveCount(6); // Compilation (3 editions) + Classical + Fade Rider + Shapeshifter
  await expect(page.getByText('Synthetic Test Compilation')).toBeVisible();
  await expect(page.getByText('3 versions')).toBeVisible(); // multi-edition collector line
  await expect(page.getByText('1998', { exact: true })).toBeVisible(); // classical year
});

test('album page groups editions and lets you switch between them', async ({ page }) => {
  await page.getByText('Synthetic Test Compilation').click();
  await expect(page.getByRole('heading', { name: 'Synthetic Test Compilation' })).toBeVisible();

  const firstTracklist = page.locator('.tracklist').innerText();

  // UI v2: the edition chips live in the Edition section (the rail's
  // thumbnails switch editions from the default view). The fixture album
  // has three editions — they must never be flattened.
  await page.getByRole('tab', { name: 'Edition' }).click();
  const chips = page.locator('.edition-chip');
  await expect(chips).toHaveCount(3);
  await expect(chips.nth(0)).toHaveAttribute('aria-pressed', 'true');
  await chips.nth(1).click();
  await expect(chips.nth(1)).toHaveAttribute('aria-pressed', 'true');

  // A different edition carries a different track list (nothing flattened).
  await page.getByRole('tab', { name: 'Overview' }).click();
  await expect(page.locator('.tracklist')).not.toHaveText(await firstTracklist);

  // Collector details remain exposed (Edition / Analysis sections).
  await page.getByRole('tab', { name: 'Edition' }).click();
  await expect(page.getByText('Catalogue number', { exact: false })).toBeVisible();
  await page.getByRole('tab', { name: 'Analysis' }).click();
  await expect(page.getByText('BS.1770 loudness', { exact: false })).toBeVisible();
});

test('search filters the shelf server-side', async ({ page }) => {
  await page.getByLabel('Search the collection').fill('Two Disc');
  await expect(page.locator('.album-card')).toHaveCount(1);
  await expect(page.getByText('Two Disc Extravaganza')).toBeVisible();
});

test('recently added sorts the shelf', async ({ page }) => {
  await page.getByRole('button', { name: 'Recently added' }).click();
  await expect(page.locator('.album-card').first()).toBeVisible();
});

test('artists browse to a release-group view', async ({ page }) => {
  await page.getByRole('link', { name: 'Artists' }).click();
  await expect(page.getByRole('heading', { name: 'Artists' })).toBeVisible();
  await page.getByText('Synthetic Chamber Orchestra').click();
  await expect(page.getByRole('heading', { name: 'Synthetic Chamber Orchestra' })).toBeVisible();
  await expect(page.getByText('Synthetic Classical Compilation')).toBeVisible();
});
