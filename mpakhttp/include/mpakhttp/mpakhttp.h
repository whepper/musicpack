/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

  * Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the following
  disclaimer in the documentation and/or other materials provided
  with the distribution.

  * Neither the name of the MusicPack Development Team nor the names of
  its contributors may be used to endorse or promote products derived
  from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
/// \file mpakhttp.h
/// MPAK HTTP Range transport adapter (embedding-layer component).
///
/// Turns an HTTP(S) URL into a transport-neutral
/// `musicpack_range_source` for `musicpack_package_open_range()`.
/// libcurl is used as the HTTP client; this component lives OUTSIDE
/// libmusicpack, which remains completely network-free (see
/// `specs/mpak-http-range-design.md`).
///
/// Behavior (normative for the adapter, per the design document):
///  - **Discovery** (`bytes=0-262143`): a 256 KiB ranged GET validates
///    the server's Range support. A proper `206 Partial Content` with a
///    well-formed `Content-Range` fixes the total object size, which is
///    then immutable for the session. The discovery bytes are retained
///    and served from memory (adapter read-ahead policy — the core
///    container cache is separate and unaffected).
///  - **Exact reads**: every subsequent read fetches exactly the
///    requested range; each `206` response is validated for status,
///    `Content-Range` start/end/total, and body length. Mismatches are
///    transport errors — bytes are never shifted, truncated, or
///    zero-filled.
///  - **No-Range servers**: a `200 OK` answer to a ranged request is
///    never interpreted as the requested range (design Tier-B:
///    fail-fast; the embedder may download the object itself and open
///    it from disk).
///  - **Changed-object detection**: a strong `ETag` from discovery is
///    replayed as `If-Range` on every fetch. A validator mismatch
///    (server answers `200`) or a `Content-Range` total change fails
///    the session — bytes from different object versions are never
///    mixed.
///  - **Identity encoding**: `Accept-Encoding: identity` is requested
///    and any `Content-Encoding` response header is rejected — byte
///    offsets must be transport-transparent.
///  - **Timeouts**: one timeout applies to each request (default 10 s,
///    design/demo policy). No retries: failures surface as
///    `MUSICPACK_ERR_IO` and mark the session failed (sticky).
///
/// Threading: source creation performs one-time libcurl global
/// initialization on first use, which is not thread-safe. HTTP range
/// sources must not be created concurrently from multiple threads;
/// created sources are independent and safe to use concurrently.
///
/// The adapter performs discovery eagerly at creation, so transport
/// problems surface before `musicpack_package_open_range()` is called.
///
/// Ownership: identical to `musicpack_range_source_stdio()`. On success
/// the returned source may be passed to `musicpack_package_open_range()`
/// (which adopts `ctx` and calls `destroy` when the package closes). If
/// `musicpack_package_open_range()` fails, or the source is never used,
/// the caller owns `ctx` and must call `out->destroy(out->ctx)` itself.
#ifndef MPAKHTTP_MPAKHTTP_H_
#define MPAKHTTP_MPAKHTTP_H_
#pragma once

#include <musicpack/error.h>
#include <musicpack/export.h>
#include <musicpack/range.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Export attribute for the adapter's public functions. The component is
/// built as a static library; the attribute exists so a shared build can
/// define it later without touching call sites.
#ifndef MUSICPACK_HTTP_API
#define MUSICPACK_HTTP_API
#endif

/// Default per-request timeout in milliseconds.
#define MUSICPACK_HTTP_TIMEOUT_DEFAULT_MS 10000L

/// Optional adapter settings. Pass NULL for the defaults.
typedef struct musicpack_http_source_opts {
    /// Per-request timeout in milliseconds (0 selects
    /// MUSICPACK_HTTP_TIMEOUT_DEFAULT_MS).
    long timeout_ms;
} musicpack_http_source_opts;

/// Creates a range source backed by an HTTP(S) URL.
///
/// Performs the discovery request immediately (one ranged GET per the
/// design's §3 strategy): the URL must already point at a complete
/// `.mpak` object; redirects are followed and validated. On success the
/// returned source is ready for `musicpack_package_open_range()`.
///
/// Transport failures — including Range-unsupported (200 answers),
/// malformed or inconsistent `Content-Range`, content-encoding
/// responses, timeouts, and HTTP error statuses (404 maps to
/// MUSICPACK_ERR_MISSING, everything else to MUSICPACK_ERR_IO) — are
/// reported here. No retries are performed.
///
/// \param url  absolute http(s) URL of the `.mpak` object
/// \param opts optional settings; NULL selects the defaults
/// \param out  receives the initialized source
/// \return MUSICPACK_OK, MUSICPACK_ERR_INVALID, MUSICPACK_ERR_MISSING
///         (404), or MUSICPACK_ERR_IO
MUSICPACK_HTTP_API musicpack_status
musicpack_http_range_source(const char *url,
                            const musicpack_http_source_opts *opts,
                            musicpack_range_source *out);

#ifdef __cplusplus
}
#endif
#endif /* MPAKHTTP_MPAKHTTP_H_ */
