// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

// Download planning (offline downloads, plan §5 policy).
//
// v1 distribution policy (approved): the complete audio surface — every
// track's primary audio AND all declared representations — plus per-track
// waveforms (they feed both the seek control and Sweet-Fade planning) and
// referenced release artwork. Booklet/lyrics/extras/analysis are deferred
// (D3): no client feature consumes them; recorded here as known-absent so
// a later phase can back-fill without schema churn.
//
// Pure function over ReleaseDetail: Node-testable, no fetch, no storage.

import type { PlannedAsset } from './types';

function assetKey(parts: Array<string | number>): string {
  return parts.join('.');
}

/** Builds the deterministic download plan for a release in canonical
 *  media/track order. Assets without a usable size are skipped: size is
 *  what staging pre-allocates and verifies against. */
export function planReleaseAssets(release: {
  id: number;
  media: Array<{
    tracks: Array<{
      id: number;
      audio: { url: string; size: number; sha256?: string };
      representations?: Array<{
        id: number;
        url: string;
        size: number;
        sha256?: string;
      }>;
      waveform?: { url: string; points: number; sha256?: string } | null;
    }>;
  }>;
  artwork: Array<{ id: number; url: string; mimeType?: string; sha256?: string }>;
}): PlannedAsset[] {
  const out: PlannedAsset[] = [];
  // Media/track iteration follows the API's canonical manifest order; it
  // affects staging order only, never identity (keys are content-keyed).
  for (const disc of release.media) {
    for (const track of disc.tracks) {
      if (track.audio?.size > 0) {
        out.push({
          key: assetKey(['t', track.id, 'primary']),
          kind: 'audio-primary',
          trackId: track.id,
          url: track.audio.url,
          size: track.audio.size,
          sha256: track.audio.sha256,
        });
      }
      for (const rep of track.representations ?? []) {
        if (rep.size > 0) {
          out.push({
            key: assetKey(['t', track.id, 'r', rep.id]),
            kind: 'audio-representation',
            trackId: track.id,
            representationId: rep.id,
            url: rep.url,
            size: rep.size,
            sha256: rep.sha256,
          });
        }
      }
      if (track.waveform && track.waveform.points > 0) {
        out.push({
          key: assetKey(['t', track.id, 'w']),
          kind: 'waveform',
          trackId: track.id,
          url: track.waveform.url,
          size: track.waveform.points * 2, // peak-rms-u8: 2 bytes/bucket
          sha256: track.waveform.sha256,
        });
      }
    }
  }
  for (const art of release.artwork) {
    out.push({
      key: assetKey(['art', art.id]),
      kind: 'artwork',
      artworkId: art.id,
      url: art.url,
      size: 0, // artwork size is not declared by the API; staged then measured
      sha256: art.sha256,
    });
  }
  return out;
}
