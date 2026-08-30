// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

/*
 * WASM smoke test for MPAK containers over HTTP Range (Emscripten builds).
 *
 * Starts a deterministic local HTTP server that serves a packed container
 * with real Range semantics, acquires it with demo/mpakrange.js (the
 * browser transport), installs the synchronous range-source imports, and
 * then exercises the MPAK core inside the module:
 *
 *   mpak_wasm_open_range -> mpak_wasm_verify -> track_open -> decode ->
 *   seek_sample -> decode-after-seek -> close (adopts + destroys source)
 *
 * Usage: node smoke_mpak.js <module.js> <container.mpak>
 * Run by tests/run_mpak_smoke.sh in Emscripten builds.
 */

"use strict";
const http = require("http");

const moduleJs = process.argv[2];
const containerPath = process.argv[3];
if (!moduleJs || !containerPath) {
  console.error("usage: node smoke_mpak.js <module.js> <container.mpak>");
  process.exit(2);
}

function fail(msg) {
  console.error("FAIL:", msg);
  process.exit(1);
}

const fs = require("fs");
const container = fs.readFileSync(containerPath);
const SIZE = container.length;

/* deterministic Range server (same semantics as the reference demo) */
const server = http.createServer((req, res) => {
  const m = /^bytes=(\d+)-(\d+)$/.exec(req.headers.range || "");
  if (!m) {
    res.writeHead(200, { "Content-Length": SIZE });
    res.end(container);
    return;
  }
  let start = parseInt(m[1], 10), end = parseInt(m[2], 10);
  if (start >= SIZE) {
    res.writeHead(416, { "Content-Range": "bytes */" + SIZE,
                         "Content-Length": 0 });
    res.end();
    return;
  }
  if (end >= SIZE) end = SIZE - 1;
  res.writeHead(206, {
    "Content-Range": "bytes " + start + "-" + end + "/" + SIZE,
    "Content-Length": end - start + 1,
    ETag: '"smoke"',
  });
  res.end(container.subarray(start, end + 1));
});

(async () => {
  await new Promise((r) => server.listen(0, "127.0.0.1", r));
  const url = "http://127.0.0.1:" + server.address().port + "/test.mpak";

  const MpakRange = require("../demo/mpakrange.js");
  const createModule = require(moduleJs);
  const Module = await createModule();

  const src = await MpakRange.acquire(url);
  if (src.size !== SIZE) fail("acquired size mismatch");
  src.install(Module);

  const h = Module._mpak_wasm_open_range(src.size);
  if (h < 0) fail("mpak_wasm_open_range failed");
  if (Module._mpak_wasm_verify(h) !== 0) fail("mpak_wasm_verify failed");

  const trackCount = Module._mpak_wasm_track_count(h);
  if (trackCount < 1) fail("no tracks in the container");
  const t = Module._mpak_wasm_track_open(h, 0);
  if (t < 0) fail("mpak_wasm_track_open failed");
  const rate = Module._mpak_wasm_track_sample_rate(t);
  if (rate <= 0) fail("no sample rate");

  const channels = 2;
  const pcmPtr = Module._malloc(1152 * channels * 4);
  let total = 0;
  for (;;) {
    const frames = Module._mpak_wasm_track_read(t, pcmPtr, 1152);
    if (frames < 0) {
      if (frames === -5 /* MUSEPACK_ERR_EOF */) break;
      fail("track_read returned " + frames);
    }
    if (frames === 0) fail("track_read stalled");
    total += frames;
  }
  if (total <= 0) fail("no frames decoded over HTTP");

  if (Module._mpak_wasm_track_seek_sample(t, Math.floor(total / 2)) !== 0)
    fail("seek_sample failed over HTTP");
  let afterSeek = 0;
  for (;;) {
    const frames = Module._mpak_wasm_track_read(t, pcmPtr, 1152);
    if (frames < 0) break;
    if (frames === 0) fail("track_read stalled after seek");
    afterSeek += frames;
  }
  if (afterSeek <= 0) fail("no frames decoded after seek");

  Module._free(pcmPtr);
  Module._mpak_wasm_track_destroy(t);
  Module._mpak_wasm_close(h);   /* adopts + destroys the JS-side source */

  console.log("mpak wasm smoke: rate=" + rate + " frames=" + total +
              " afterSeek=" + afterSeek);
  server.close();
})().catch((e) => {
  fail(String((e && e.message) || e));
});
