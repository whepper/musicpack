/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved. (BSD-3-Clause; see LICENSES/BSD-3-Clause.txt for the full text.)
  SPDX-License-Identifier: BSD-3-Clause

  Per-track embedding cache. Cache identity = audio SHA-256 + profile id +
  weights SHA-256; any of those changing invalidates the entry. Completed
  entries remain reusable across albums and after a cancelled run. The cache
  lives outside the final package.
*/
#ifndef SONIC_CACHE_H_
#define SONIC_CACHE_H_

#include <stddef.h>

/* Loads a cached track embedding. `audio_sha` is the lowercase hex SHA-256
   of the audio file. On a hit: `present` is set (0 => cached "no
   embedding") and, when present, `out` receives `dims` floats. Returns 1 on
   a usable cache hit, 0 on a miss/stale entry. */
int sonic_cache_load(const char *cache_dir, const char *profile_id,
                     const char *weights_sha, const char *audio_sha,
                     float *out, size_t dims, int *present);

/* Stores a track embedding (or a null marker when `present` == 0). Returns
   1 on success. */
int sonic_cache_store(const char *cache_dir, const char *profile_id,
                      const char *weights_sha, const char *audio_sha,
                      const float *vec, size_t dims, int present);

#endif /* SONIC_CACHE_H_ */
