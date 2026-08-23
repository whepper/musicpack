import { defineConfig, devices } from '@playwright/test';

export default defineConfig({
  testDir: 'tests/e2e/specs',
  timeout: 60_000,
  expect: { timeout: 15_000 },
  fullyParallel: false,
  workers: 1,
  // CI runners are slow enough that ~1 in 30 browser-timing assertions can
  // miss; retry there so one flaky poll doesn't fail the whole push.
  retries: process.env.CI ? 2 : 0,
  reporter: [['list']],
  use: {
    baseURL: process.env.MUSICPACK_E2E_URL ?? 'http://127.0.0.1:8099',
    trace: 'on-first-retry',
    ...devices['Desktop Chrome'],
  },
  webServer: {
    command: 'bash tests/e2e/start-server.sh',
    url: 'http://127.0.0.1:8099/api/v1/health',
    reuseExistingServer: false,
    timeout: 30_000,
  },
});
