// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

import { existsSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../../..');
const moduleJs = process.env.MUSICPACK_WASM_JS ?? path.join(root, 'build-wasm/wasm/musepack.js');
const harness = path.join(here, 'wasm-gapless.mjs');
const trackA = path.join(root, 'tests/fixtures/sine44-q5.mpc');
const trackB = path.join(root, 'tests/fixtures/sine44-q7.mpc');
const seekFixture = path.join(root, 'tests/fixtures/sine44-q5-48s.mpc');

if (!existsSync(moduleJs)) {
  console.error(`WASM module not found: ${moduleJs}\nBuild the build-wasm target or set MUSICPACK_WASM_JS.`);
  process.exit(1);
}

const result = spawnSync(process.execPath, [harness, moduleJs, trackA, trackB, seekFixture], {
  stdio: 'inherit',
});
process.exit(result.status ?? 1);
