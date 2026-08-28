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
/// \file range.h
/// Transport-neutral byte-range sources.
///
/// A `musicpack_range_source` is a random-access byte provider: a total
/// size plus exact absolute-offset reads. It is deliberately *not* an
/// HTTP or networking abstraction — libmusicpack performs no networking
/// and includes no transport dependencies. HTTP Range adapters, WASM
/// fetch adapters, and any other remote transport belong to the
/// embedder and implement this vtable over their own client code (see
/// `specs/mpak-http-range-design.md`; the browser demo's
/// `demo/networker.js` is a complete reference implementation of the
/// HTTP behavior).
///
/// Contract:
///  - `size` reports the total object size in bytes. The size MUST be
///    stable for the lifetime of the source: a remote object that
///    changes size mid-session must be reported as a transport failure
///    by the adapter, never served silently.
///  - `read` fills `buf` with exactly `len` bytes starting at absolute
///    byte `offset`. Reads are exact: a short read, an out-of-bounds
///    range (`offset + len > size`), or a transport failure returns an
///    error and leaves `buf` unspecified. `len == 0` succeeds without
///    touching the transport. Callers never read past the reported
///    size.
///  - `destroy` releases the source's resources; may be NULL if the
///    context needs no cleanup.
///
/// Ownership: `musicpack_package_open_range()` takes ownership of
/// `ctx` on success (destroy is called when the package is closed). On
/// a failed open the source is NOT destroyed — the caller retains
/// ownership and must clean it up itself. The source may have been
/// read from during a failed attempt.
#ifndef MUSICPACK_RANGE_H_
#define MUSICPACK_RANGE_H_
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <musicpack/error.h>
#include <musicpack/export.h>

#ifdef __cplusplus
extern "C" {
#endif

/// A random-access byte source (transport-neutral).
typedef struct musicpack_range_source {
    void *ctx;                 ///< opaque adapter context (may be NULL)
    /// Reports the total object size in bytes.
    musicpack_status (*size)(void *ctx, uint64_t *out);
    /// Exact read: fills \p buf with \p len bytes at absolute \p offset.
    musicpack_status (*read)(void *ctx, uint64_t offset,
                             unsigned char *buf, size_t len);
    /// Releases adapter resources; may be NULL.
    void (*destroy)(void *ctx);
} musicpack_range_source;

/// Local-file range source: the reference adapter and offline test
/// target. Opens \p path with the same hardened regular-file checks the
/// package backends use (no symlinks, single link, regular file) and
/// records its size.
///
/// The produced source must be released with `destroy` (either by
/// passing it to musicpack_package_open_range(), which adopts it on
/// success, or by calling `out->destroy(out->ctx)` directly).
///
/// \param path file to open
/// \param out  receives the initialized source
/// \return MUSICPACK_OK or an error
MUSICPACK_API musicpack_status
musicpack_range_source_stdio(const char *path, musicpack_range_source *out);

#ifdef __cplusplus
}
#endif
#endif /* MUSICPACK_RANGE_H_ */
