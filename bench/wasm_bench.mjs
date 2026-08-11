/*
 * WebAssembly decoder benchmark (Node.js).
 *
 * Loads the Emscripten module (build-wasm/wasm/musepack.js), decodes each
 * .mpc fully via _mpc_wasm_open/_mpc_wasm_read, and reports wall-clock
 * decode time and the realtime multiplier. Also measures the JS<->wasm
 * boundary cost by decoding with two block sizes (1152 and 4608 frames per
 * _mpc_wasm_read call). Measure-only: no API change.
 *
 * Usage: node wasm_bench.mjs <module.js> <file.mpc> [--blocks 1152,4608]
 */

import { createRequire } from "module";
import path from "path";

const require = createRequire(import.meta.url);
const fs = require("fs");

const moduleJs = process.argv[2];
const inputMpc = process.argv[3];
const blocksArg = process.argv.find((a) => a.startsWith("--blocks="));
const blockSizes = blocksArg
  ? blocksArg.split("=")[1].split(",").map(Number)
  : [1152, 4608];

if (!moduleJs || !inputMpc) {
  console.error("usage: node wasm_bench.mjs <module.js> <file.mpc>");
  process.exit(2);
}

const bytes = fs.readFileSync(path.resolve(inputMpc));
const module_ = require(path.resolve(moduleJs));

async function main() {
  const M = await module_();
  const rate = M._mpc_wasm_sample_rate, ch = M._mpc_wasm_channels;
  const lengthSamples = M._mpc_wasm_length_samples;

  for (const block of blockSizes) {
    // Warm-up decode (lazy init, caches).
    {
      const h = M._mpc_wasm_open(bytes, bytes.length);
      const ptr = M._malloc(block * 4 * 2);
      let frames;
      do { frames = M._mpc_wasm_read(h, ptr, block); } while (frames > 0);
      M._free(ptr); M._mpc_wasm_destroy(h);
    }

    const h = M._mpc_wasm_open(bytes, bytes.length);
    const ptr = M._malloc(block * 4 * 2);
    const totalSamples = lengthSamples(h);
    const rateHz = rate(h);
    const chan = ch(h);
    const audioS = totalSamples / rateHz;

    const t0 = process.hrtime.bigint();
    let total = 0;
    let frames;
    do {
      frames = M._mpc_wasm_read(h, ptr, block);
      if (frames > 0) total += frames;
    } while (frames > 0);
    const wallMs = Number(process.hrtime.bigint() - t0) / 1e6;

    M._free(ptr);
    M._mpc_wasm_destroy(h);

    console.log(
      `${inputMpc}\t${rateHz}hz\t${chan}ch\tblock=${block}\t` +
      `${audioS.toFixed(3)}s\taudio\tsum=${total}\t${wallMs.toFixed(1)}ms\t` +
      `${(audioS / (wallMs / 1e3)).toFixed(2)}x`
    );
  }
}

main().catch((e) => { console.error(e); process.exit(1); });
