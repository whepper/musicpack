// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

import { describe, expect, it } from 'vitest';
import { planReleaseAssets } from '../../../app/src/lib/offline/plan';

function releaseFixture() {
  return {
    id: 7,
    media: [
      {
        disc: 1,
        tracks: [
          {
            id: 101,
            audio: { url: '/api/v1/tracks/101/audio', size: 1000, sha256: 'a'.repeat(64) },
            representations: [
              {
                id: 201,
                url: '/api/v1/tracks/101/representations/201/audio',
                size: 5000,
                sha256: 'b'.repeat(64),
              },
              {
                // Zero-size alternates cannot be staged (no declared byte
                // budget); the planner skips them.
                id: 202,
                url: '/api/v1/tracks/101/representations/202/audio',
                size: 0,
                sha256: 'c'.repeat(64),
              },
            ],
            waveform: { url: '/api/v1/tracks/101/waveform', points: 480, sha256: 'd'.repeat(64) },
          },
          {
            id: 102,
            audio: { url: '/api/v1/tracks/102/audio', size: 2000 },
            waveform: null,
          },
        ],
      },
    ],
    artwork: [{ id: 301, url: '/api/v1/assets/301', mimeType: 'image/jpeg' }],
  };
}

describe('planReleaseAssets (v1 download policy)', () => {
  it('plans primaries + all usable representations + waveforms + artwork', () => {
    const plan = planReleaseAssets(releaseFixture());
    // The zero-size alternate is deliberately absent (skipped by the planner).
    const keys = plan.map((a) => a.key);
    expect(keys).toEqual([
      't.101.primary',
      't.101.r.201',
      't.101.w',
      't.102.primary',
      'art.301',
    ]);
  });

  it('carries hashes and sizes for verification', () => {
    const plan = planReleaseAssets(releaseFixture());
    const rep = plan.find((a) => a.key === 't.101.r.201')!;
    expect(rep.sha256).toBe('b'.repeat(64));
    expect(rep.size).toBe(5000);
    const wf = plan.find((a) => a.key === 't.101.w')!;
    // peak-rms-u8: 2 bytes per bucket
    expect(wf.size).toBe(960);
  });

  it('skips zero-size assets but keeps undeclared-size artwork', () => {
    const plan = planReleaseAssets(releaseFixture());
    expect(plan.find((a) => a.key === 't.101.r.202')).toBeUndefined();
    expect(plan.find((a) => a.key === 'art.301')!.size).toBe(0);
  });
  it('handles releases without representations or waveforms (pre-Phase-3 packages)', () => {
    const rel = releaseFixture();
    rel.media[0]!.tracks[0]!.representations = [];
    rel.media[0]!.tracks[0]!.waveform = null;
    const keys = planReleaseAssets(rel).map((a) => a.key);
    expect(keys).toEqual(['t.101.primary', 't.102.primary', 'art.301']);
  });
});
