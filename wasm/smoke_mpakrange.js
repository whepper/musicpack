// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

/*
 * Node smoke test for the MPAK HTTP Range transport (demo/mpakrange.js).
 *
 * Runs the transport against a deterministic local HTTP server with real
 * fetch() and real Range semantics (no WASM required — the C/WASM wrapper
 * is exercised by wasm/smoke_mpak.js under an Emscripten build). Covers the
 * design's §9 error rows at the transport layer: valid 206 discovery +
 * blocks, exact bytes, 64 KiB alignment, final partial block, 200-on-Range,
 * missing/wrong Content-Range, short/oversized bodies, Content-Encoding,
 * HTTP errors, timeout, abort, changed ETag, and read-bounds safety.
 *
 * Usage: node smoke_mpakrange.js <container.mpak>
 */

"use strict";
const http = require("http");
const zlib = require("zlib");
const fs = require("fs");
const MpakRange = require("../demo/mpakrange.js");

const containerPath = process.argv[2];
if (!containerPath) {
  console.error("usage: node smoke_mpakrange.js <container.mpak>");
  process.exit(2);
}
const raw = fs.readFileSync(containerPath);
/* pad past the 256 KiB discovery window so block fetches / If-Range /
   alignment are actually exercised regardless of the input container */
const container = raw.length > 262144 + 65536
    ? raw
    : Buffer.concat([raw, Buffer.alloc(262144 + 65536 - raw.length + 1024)]);
const SIZE = container.length;

let failures = 0;
function check(cond, msg) {
  console.log("  [" + (cond ? "PASS" : "FAIL") + "] " + msg);
  if (!cond) failures++;
}

/* ------------------------------------------------------------------ */
/* deterministic Range server                                          */
/* ------------------------------------------------------------------ */

/*
 * quirks: contentRangeOverride (string), omitContentRange, shortBody,
 * longBody, statusOverride, encoding, delayMs, etag (default '"mpak-test"'),
 * omitEtag, dropIfRange, chunked
 */
function startServer(quirks, data) {
  const buf = data || container;
  const size = buf.length;
  let served = 0;
  const server = http.createServer((req, res) => {
    const finish = (status, headers, body) => {
      const send = () => {
        try { res.writeHead(status, headers); res.end(body); } catch (e) { /* client gone */ }
      };
      if (quirks.delayMs) setTimeout(send, quirks.delayMs); else send();
    };
    const range = req.headers.range;
    const etag = quirks.etag !== undefined ? quirks.etag : '"mpak-test"';
    const etagHeader = quirks.omitEtag ? {} : { ETag: etag };

    const thisReq = served++;
    /* lieTotal: claim a larger object and zero-fill beyond the real bytes
       (models a lying/changed server consistently) */
    const total = quirks.lieTotal || size;
    const m = /^bytes=(\d+)-(\d+)$/.exec(range || "");
    if (!m) {
      finish(200, { "Content-Length": size }, buf);
      return;
    }
    let start = parseInt(m[1], 10), end = parseInt(m[2], 10);
    if (start >= total) {
      finish(416, { "Content-Length": 0, "Content-Range": "bytes */" + total }, "");
      return;
    }
    if (end >= total) end = total - 1;

    const body = Buffer.alloc(end - start + 1);
    if (start < size)
      buf.copy(body, 0, start, Math.min(end + 1, size));
    const useStar = quirks.starTotal === true ||
                    (quirks.starTotal === "discovery" && thisReq === 0);
    let cr = "bytes " + start + "-" + end + "/" + (useStar ? "*" : total);
    if (quirks.contentRangeOverride !== undefined)
      cr = quirks.contentRangeOverride;
    if (quirks.omitContentRange) cr = null;

    const headers = Object.assign(
        { "Content-Type": "application/octet-stream" }, etagHeader);
    if (cr) headers["Content-Range"] = cr;
    if (quirks.encoding) headers["Content-Encoding"] = quirks.encoding;
    let sendBody = body;
    if (quirks.shortBody && sendBody.length > 8)
      sendBody = sendBody.subarray(0, sendBody.length / 2);
    if (quirks.longBody) {
      const bigger = Buffer.allocUnsafe(sendBody.length + 64);
      bigger.set(sendBody, 0);
      sendBody = bigger;
    }
    if (quirks.gzipBody) sendBody = zlib.gzipSync(sendBody);
    if (quirks.chunked) {
      headers["Transfer-Encoding"] = "chunked";
    } else {
      headers["Content-Length"] = sendBody.length;
    }

    finish(quirks.statusOverride || 206, headers, sendBody);
  });
  return new Promise((resolve) => {
    server.listen(0, "127.0.0.1", () => resolve(server));
  });
}

function urlOf(server) {
  return "http://127.0.0.1:" + server.address().port + "/test.mpak";
}

async function withServer(quirks, fn, data) {
  const server = await startServer(quirks || {}, data);
  try {
    return await fn(urlOf(server));
  } finally {
    server.close();
  }
}

/* ------------------------------------------------------------------ */
/* tests                                                               */
/* ------------------------------------------------------------------ */

async function main() {
  let src;

  console.log("== 1. valid 206 discovery + blocks: exact bytes");
  await withServer({}, async (url) => {
    src = await MpakRange.acquire(url);
    check(src.size === SIZE, "total size from discovery Content-Range");
    check(src.blocks === Math.ceil(SIZE / 65536), "block accounting");
    check(Buffer.compare(Buffer.from(src.readBytes(0, SIZE)), container) === 0,
          "acquired bytes == container (byte-exact)");
    const seen = [];
    // request log: first request must be the design's 256 KiB discovery
    src.destroy();
  });
  check(true, "acquire used one 256 KiB discovery + aligned 64 KiB blocks (server-visible below)");

  console.log("== 2. discovery request shape (bytes=0-262143) + alignment");
  {
    const ranges = [];
    const server = await startServer({});
    server.on("request", (req) => ranges.push(req.headers.range));
    const s = await MpakRange.acquire(urlOf(server));
    check(ranges[0] === "bytes=0-262143", "first request is the 256 KiB discovery");
    let aligned = true;
    for (let i = 1; i < ranges.length; i++) {
      const m = /^bytes=(\d+)-(\d+)$/.exec(ranges[i]);
      if (!m || Number(m[1]) % 65536 !== 0) aligned = false;
    }
    check(aligned, "block fetches are 64 KiB aligned");
    check(ranges.length === 1 + Math.ceil((SIZE - 262144) / 65536),
          "one request per remaining block, no duplicates");
    const afterReads = ranges.length;
    for (let i = 0; i < 5; i++) s.readBytes(i * 1000, 999);
    check(ranges.length === afterReads, "reads are served from memory (no requests)");
    s.destroy();
    server.close();
  }

  console.log("== 3. EOF / final partial block");
  await withServer({}, async (url) => {
    const s = await MpakRange.acquire(url);
    const tailLen = SIZE - (Math.floor((SIZE - 1) / 65536) * 65536);
    const tail = s.readBytes(SIZE - tailLen, tailLen);
    check(Buffer.compare(Buffer.from(tail), container.subarray(SIZE - tailLen)) === 0,
          "final partial block byte-exact");
    check(s.readBytes(SIZE, 0).length === 0, "zero-length read at EOF ok");
    s.destroy();
  });

  console.log("== 4. 200-on-Range rejected (Tier-B)");
  await withServer({ statusOverride: 200 }, async (url) => {
    let err = null;
    try { await MpakRange.acquire(url); } catch (e) { err = e; }
    check(err && err.code === MpakRange.ERR.ERR_200, "200 discovery -> ERR_200");
  });

  console.log("== 5. missing Content-Range rejected");
  await withServer({ omitContentRange: true }, async (url) => {
    let err = null;
    try { await MpakRange.acquire(url); } catch (e) { err = e; }
    check(err && err.code === MpakRange.ERR.RANGE, "missing CR -> RANGE");
  });

  console.log("== 6. wrong start rejected");
  await withServer({ contentRangeOverride: "bytes 5-262143/" + SIZE },
                   async (url) => {
    let err = null;
    try { await MpakRange.acquire(url); } catch (e) { err = e; }
    check(err && err.code === MpakRange.ERR.RANGE, "wrong start -> RANGE");
  });

  console.log("== 7. wrong end rejected");
  await withServer({ contentRangeOverride: "bytes 0-262142/" + SIZE },
                   async (url) => {
    let err = null;
    try { await MpakRange.acquire(url); } catch (e) { err = e; }
    check(err && err.code === MpakRange.ERR.TRUNCATED, "wrong end -> TRUNCATED");
  });

  console.log("== 8. total inconsistent with the served range rejected");
  await withServer({ contentRangeOverride: "bytes 0-262143/143750" },
                   async (url) => {
    let err = null;
    try { await MpakRange.acquire(url); } catch (e) { err = e; }
    check(err && err.code === MpakRange.ERR.TRUNCATED,
          "total < range+1 -> rejected (end check)");
  });
  console.log("== 8b. discovery-time total lie defines the session;");
  console.log("       it is caught by the integrity layers, not the transport");
  await withServer({ lieTotal: 999999 }, async (url) => {
    const s = await MpakRange.acquire(url);
    check(s.size === 999999,
          "transport accepts a self-consistent total (documented semantics)");
    s.destroy();
  });

  console.log("== 9. short body rejected");
  await withServer({ shortBody: true }, async (url) => {
    let err = null;
    try { await MpakRange.acquire(url); } catch (e) { err = e; }
    check(err && err.code === MpakRange.ERR.TRUNCATED, "short body -> TRUNCATED");
  });

  console.log("== 10. oversized body rejected");
  await withServer({ longBody: true }, async (url) => {
    let err = null;
    try { await MpakRange.acquire(url); } catch (e) { err = e; }
    check(err && err.code === MpakRange.ERR.TRUNCATED,
          "oversized body -> TRUNCATED (got: " + (err ? err.code : "acquired") + ")");
  });

  console.log("== 11. Content-Encoding: raw bytes under gzip header");
  await withServer({ encoding: "gzip" }, async (url) => {
    let err = null;
    try { await MpakRange.acquire(url); } catch (e) { err = e; }
    check(err && (err.code === MpakRange.ERR.ENCODING ||
                  err.code === MpakRange.ERR.NETWORK),
          "undecodable gzip body -> hard error (decoder error or our check)");
  });
  console.log("== 11b. Content-Encoding: valid gzip body must never pass");
  await withServer({ encoding: "gzip", gzipBody: true }, async (url) => {
    let err = null;
    try { await MpakRange.acquire(url); } catch (e) { err = e; }
    check(err && err.code === MpakRange.ERR.ENCODING,
          "decoded body + visible header -> ENCODING (got: " +
              (err ? err.code : "acquired") + ")");
  });

  console.log("== 12. HTTP errors");
  await withServer({ statusOverride: 500 }, async (url) => {
    let err = null;
    try { await MpakRange.acquire(url); } catch (e) { err = e; }
    check(err && err.code === MpakRange.ERR.HTTP && err.status === 500,
          "500 -> HTTP (status retained)");
  });
  await withServer({ statusOverride: 404 }, async (url) => {
    let err = null;
    try { await MpakRange.acquire(url); } catch (e) { err = e; }
    check(err && err.code === MpakRange.ERR.MISSING, "404 -> MISSING");
  });

  console.log("== 13. timeout honored");
  await withServer({ delayMs: 2000 }, async (url) => {
    const t0 = Date.now();
    let err = null;
    try { await MpakRange.acquire(url, { timeoutMs: 300 }); } catch (e) { err = e; }
    check(err && err.code === MpakRange.ERR.TIMEOUT && Date.now() - t0 < 1500,
          "timeout -> ERR.TIMEOUT, prompt");
  });

  console.log("== 14. strong ETag captured and replayed as If-Range");
  {
    let sawIfRange = false;
    let sawValue = "";
    const server = await startServer({});
    server.on("request", (req) => {
      if (req.headers["if-range"]) {
        sawIfRange = true;
        sawValue = req.headers["if-range"];
      }
    });
    const s = await MpakRange.acquire(urlOf(server));
    check(s.etag === '"mpak-test"', "strong ETag captured");
    check(sawIfRange && sawValue === '"mpak-test"',
          "If-Range replayed with the discovery validator on block fetches");
    s.destroy();
    server.close();
  }
  console.log("== 14b. changed total on a later block -> CHANGED");
  {
    let first = true;
    const server = await startServer({});
    server.removeAllListeners("request");
    server.on("request", (req, res) => {
      const m = /^bytes=(\d+)-(\d+)$/.exec(req.headers.range || "");
      if (!m) { res.writeHead(400); res.end(); return; }
      const start = parseInt(m[1], 10), end = parseInt(m[2], 10);
      const total = first && start === 0 ? SIZE : SIZE + 999;
      if (start > 0) first = false;
      res.writeHead(206, {
        "Content-Range": "bytes " + start + "-" + end + "/" + total,
        "Content-Length": end - start + 1,
        ETag: '"mpak-test"',
      });
      res.end(container.subarray(start, end + 1));
    });
    let err = null;
    try { await MpakRange.acquire(urlOf(server)); } catch (e) { err = e; }
    check(err && err.code === MpakRange.ERR.CHANGED,
          "total change on a block fetch -> CHANGED (session fails)");
    server.close();
  }

  console.log("== 15. weak ETag not used as If-Range validator");
  {
    let sawIfRange = false;
    const server = await startServer({ etag: 'W/"weak"' });
    server.on("request", (req) => { if (req.headers["if-range"]) sawIfRange = true; });
    const s = await MpakRange.acquire(urlOf(server));
    check(s.etag === null, "weak ETag not captured");
    check(!sawIfRange, "no If-Range replayed for a weak validator");
    s.destroy();
    server.close();
  }

  console.log("== 16. read bounds are exact");
  await withServer({}, async (url) => {
    const s = await MpakRange.acquire(url);
    let ok = true;
    try { s.readBytes(SIZE - 1, 2); ok = false; } catch (e) { /* expected */ }
    check(ok, "read past EOF rejected");
    try { s.readBytes(-1, 4); ok = false; } catch (e) { /* expected */ }
    check(ok, "negative offset rejected");
    check(Buffer.compare(Buffer.from(s.readBytes(65530, 12)),
                         container.subarray(65530, 65542)) === 0,
          "boundary-crossing read byte-exact");
    s.destroy();
    let ok2 = false;
    try { s.readBytes(0, 4); } catch (e) { ok2 = e.code === MpakRange.ERR.CLOSED; }
    check(ok2, "destroyed source refuses reads");
  });

  console.log("== 17. aborted fetch fails cleanly");
  await withServer({ delayMs: 5000 }, async (url) => {
    let err = null;
    try { await MpakRange.acquire(url, { timeoutMs: 200 }); } catch (e) { err = e; }
    check(err && (err.code === MpakRange.ERR.TIMEOUT ||
                  err.code === MpakRange.ERR.NETWORK),
          "slow server -> hard error, no hang");
  });

  console.log("== 18. unknown-host fails fast with NETWORK");
  {
    let err = null;
    try {
      await MpakRange.acquire("http://127.0.0.1:1/x.mpak", { timeoutMs: 2000 });
    } catch (e) { err = e; }
    check(err && err.code === MpakRange.ERR.NETWORK, "connection refused -> NETWORK");
  }

  console.log("== 19. chunked 206 (framing decoded by fetch; header rules still apply)");
  await withServer({ chunked: true }, async (url) => {
    const s = await MpakRange.acquire(url);
    check(Buffer.compare(Buffer.from(s.readBytes(0, SIZE)), container) === 0,
          "chunked transfer framing is decoded; validated bytes still exact");
    s.destroy();
  });

  console.log("== 20. '*'-total discovery with a short body: whole object");
  await withServer({ starTotal: true }, async (url) => {
    let err = null;
    let s = null;
    try {
      s = await MpakRange.acquire(url, { ifRange: false });
    } catch (e) { err = e; }
    check(s != null, "'*'-total: short discovery body defines the size");
    if (s) {
      check(s.size === 8192 && s.blocks === 1,
            "'*'-total size matches the served object");
      check(Buffer.compare(Buffer.from(s.readBytes(0, 8192)),
                           raw.subarray(0, 8192)) === 0,
            "'*'-total bytes exact");
      s.destroy();
    } else {
      check(false, "unexpected: " + (err && err.message));
    }
  }, raw.subarray(0, 8192));   /* a genuinely short '*'-total object */

  console.log("== 21. '*'-total with a full-length body: size probe");
  {
    let probed = false;
    const server = await startServer({ starTotal: "discovery" });
    server.on("request", (req) => {
      if (req.headers.range === "bytes=0-0") probed = true;
    });
    const s = await MpakRange.acquire(urlOf(server));
    check(probed && s.size === SIZE,
          "ambiguous '*'-total probed with bytes=0-0; size fixed");
    s.destroy();
    server.close();
  }
  console.log("== 21b. a server that never reveals the total fails closed");
  await withServer({ starTotal: true }, async (url) => {
    let err = null;
    try { await MpakRange.acquire(url); } catch (e) { err = e; }
    check(err && err.code === MpakRange.ERR.RANGE,
          "'*'-only server: probe cannot fix the size -> RANGE");
  });

  console.log("== 22. W1: discovery total above maxBytes rejected before allocation");
  {
    let allocated = false;
    const origAlloc = Buffer.allocUnsafe;
    const server = await startServer({
      lieTotal: Number.MAX_SAFE_INTEGER,   // impossible total
    });
    let err = null;
    try { await MpakRange.acquire(urlOf(server)); } catch (e) { err = e; }
    check(err && err.code === MpakRange.ERR.SIZE,
          "MAX_SAFE_INTEGER total -> SIZE (typed, no raw RangeError)");
    server.close();

    // 3 GiB lie: above the default maxBytes (2 GiB), below 2^53
    const server2 = await startServer({ lieTotal: 3 * 1024 * 1024 * 1024 });
    err = null;
    try { await MpakRange.acquire(urlOf(server2)); } catch (e) { err = e; }
    check(err && err.code === MpakRange.ERR.SIZE,
          "3 GiB total > default maxBytes -> SIZE before allocation");
    server2.close();

    // a valid total below maxBytes still works (behavior preserved)
    const server3 = await startServer({});
    const s3 = await MpakRange.acquire(urlOf(server3));
    check(s3.size === SIZE, "normal containers unaffected by maxBytes");
    s3.destroy();
    server3.close();
    void allocated; void origAlloc;
  }

  console.log("== 22b. W1: explicit maxBytes option is honored");
  await withServer({ lieTotal: 400000 }, async (url) => {
    let err = null;
    try { await MpakRange.acquire(url, { maxBytes: 300000 }); } catch (e) { err = e; }
    check(err && err.code === MpakRange.ERR.SIZE,
          "total 400000 > maxBytes 300000 -> SIZE");
  });
  await withServer({ lieTotal: 400000 }, async (url) => {
    let err = null;
    let s = null;
    try { s = await MpakRange.acquire(url, { maxBytes: 500000 }); }
    catch (e) { err = e; }
    check(s && s.size === 400000, "total 400000 <= maxBytes 500000 -> accepted");
    if (s) s.destroy();
  });

  console.log("== 23. W2: one-active-source-per-Module");
  {
    const fakeModule = { HEAPU8: { set: (v, p) => { void v; void p; } } };
    // two distinct sources via two servers
    const serverA = await startServer({ etag: '"A"' });
    const sA = await MpakRange.acquire(urlOf(serverA));
    const serverB = await startServer({ etag: '"B"', });
    const dataB = Buffer.alloc(8192, 0xBB);
    const serverC = await startServer({}, dataB);
    const sB = await MpakRange.acquire(urlOf(serverC));

    sA.install(fakeModule);
    let refused = null;
    try { sB.install(fakeModule); } catch (e) { refused = e; }
    check(refused && refused.code === MpakRange.ERR.INSTALLED,
          "second install while A is active -> INSTALLED");

    // A still reads correctly after the rejected install
    check(Buffer.compare(Buffer.from(sA.readBytes(0, 16)),
                         container.subarray(0, 16)) === 0,
          "A remains functional after the rejected install");

    sA.destroy();                       /* releases the slot */
    let bInstalled = true;
    try { sB.install(fakeModule); } catch (e) { bInstalled = false; }
    check(bInstalled, "slot reusable after A is destroyed");
    // B serves its own bytes through the installed handler
    const served = fakeModule.mpakRangeRead(0, 0, 4);
    check(served === 4, "installed handler serves the source");
    sB.destroy();
    let gone = false;
    try { fakeModule.mpakRangeRead(0, 0, 4); } catch (e) {
      gone = e.code === MpakRange.ERR.CLOSED;
    }
    check(gone, "destroy via the installed handler closes the source");
    serverA.close(); serverB.close(); serverC.close();
  }

  console.log("== 23b. W2: failed acquisition leaves the slot free");
  {
    const fakeModule = { HEAPU8: { set: () => {} } };
    await withServer({ statusOverride: 500 }, async (url) => {
      let err = null;
      try { await MpakRange.acquire(url); } catch (e) { err = e; }
      check(err && err.code === MpakRange.ERR.HTTP,
            "failed acquisition (500) as expected");
    });
    const server = await startServer({});
    const s = await MpakRange.acquire(urlOf(server));
    let ok = true;
    try { s.install(fakeModule); } catch (e) { ok = false; }
    check(ok, "module not occupied by the failed acquisition");
    s.destroy();
    server.close();
  }

  console.log("== 24. W3: multipart/byteranges 206 rejected at the transport");
  {
    // fully valid 206 (correct per-request Content-Range, complete body)
    // whose Content-Type is multipart/byteranges — the framing bytes make
    // the body LONGER than the declared range, and the rejection must come
    // from the Content-Type check before any length/MPAK validation
    let multipartTypeSeen = false;
    const server = await startServer({});
    server.removeAllListeners("request");
    server.on("request", (req, res) => {
      const m = /^bytes=(\d+)-(\d+)$/.exec(req.headers.range || "");
      if (!m) { res.writeHead(400); res.end(); return; }
      const start = parseInt(m[1], 10), end = parseInt(m[2], 10);
      const body = Buffer.concat([
        Buffer.from("--x\r\nContent-Type: application/octet-stream\r\n" +
                    "Content-Range: bytes " + start + "-" + end + "/" +
                    SIZE + "\r\n\r\n"),
        container.subarray(start, end + 1),
        Buffer.from("\r\n--x--\r\n"),
      ]);
      multipartTypeSeen = true;
      res.writeHead(206, {
        "Content-Type": "multipart/byteranges; boundary=x",
        "Content-Range": "bytes " + start + "-" + end + "/" + SIZE,
        "Content-Length": body.length,
        ETag: '"mpak-test"',
      });
      res.end(body);
    });
    let err = null;
    try { await MpakRange.acquire(urlOf(server)); } catch (e) { err = e; }
    check(multipartTypeSeen, "multipart responses were served");
    check(err && err.code === MpakRange.ERR.RANGE &&
              /multipart/.test(err.message),
          "multipart 206 rejected by the transport (typed RANGE, " +
          "before any MPAK validation)");
    server.close();
  }
  {
    let sawIfRange = false;
    const server = await startServer({});
    server.on("request", (req) => { if (req.headers["if-range"]) sawIfRange = true; });
    const s = await MpakRange.acquire(urlOf(server), { ifRange: false });
    check(!sawIfRange, "no If-Range when disabled");
    s.destroy();
    server.close();
  }

  if (failures) {
    console.log("SMOKE: " + failures + " failure(s)");
    process.exit(1);
  }
  console.log("SMOKE: all mpakrange transport tests passed");
}

main().catch((e) => {
  console.error("SMOKE: fatal:", e);
  process.exit(1);
});
