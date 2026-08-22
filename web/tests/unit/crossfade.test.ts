// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

import { describe, expect, it, vi } from 'vitest';
import { Player } from '../../player-core/src/player';
import { createQueueModel } from '../../player-core/src/queue';
import type { PlayerEvent } from '../../player-core/src/events';
import type { PlaybackItem, StreamInfo } from '../../player-core/src/types';

const RATE = 44100;
const LEN = RATE * 30; // 30 s tracks

function item(n: number): PlaybackItem {
  return {
    id: `t${n}`,
    trackId: n,
    source: { kind: 'http-range', url: `/api/v1/tracks/${n}/audio`, byteSize: 100 },
    title: `T${n}`,
    artist: 'A',
    albumTitle: 'AL',
  };
}

interface Harness {
  handlers: {
    primed(): void;
    buffering(): void;
    eos(): void;
    error(m: string): void;
    tick(): void;
  };
  beginCrossfade: ReturnType<typeof vi.fn>;
  rendered: number;
}

/** Fake engine with controllable crossfade support. */
function makePlayer(opts: {
  crossfadeCapable: boolean;
  fadeResult?: StreamInfo | null;
}): { player: Player; h: Harness; queue: ReturnType<typeof createQueueModel> } {
  const queue = createQueueModel({ rng: () => 0.5 });
  const h: Harness = {
    handlers: {} as Harness['handlers'],
    // NOTE: `??` would treat null as absent — but null means "the engine
    // DECLINED the fade" and must be returned as-is.
    beginCrossfade: vi.fn(async (): Promise<StreamInfo | null> =>
      opts.fadeResult === undefined
        ? { rate: RATE, channels: 2, version: 0, lengthSamples: LEN }
        : opts.fadeResult,
    ),
    rendered: 0,
  };
  const player = new Player(queue as never, {
    engineFactory: (_kind, handlers) => {
      h.handlers = handlers;
      return {
        capabilities: {
          preloadNext: true,
          sampleAccurateGapless: false,
          decodeGate: false,
          crossfade: opts.crossfadeCapable,
        },
        init: async () => undefined,
        open: async () => ({ rate: RATE, channels: 2, version: 0, lengthSamples: LEN }),
        prepareNext: async () => ({ rate: RATE, channels: 2, version: 0, lengthSamples: LEN }),
        advance: async () => ({ rate: RATE, channels: 2, version: 0, lengthSamples: LEN }),
        play: async () => undefined,
        pause: async () => undefined,
        seekSample: async () => undefined,
        startPumping: () => undefined,
        pausePumping: () => undefined,
        setGain: () => undefined,
        renderedSamples: () => h.rendered,
        close: async () => undefined,
        on: () => () => undefined,
        beginCrossfade: h.beginCrossfade,
      };
    },
    resolveKind: () => 'musepack',
    storage: { get: () => null, set: () => undefined },
    now: () => Date.now(),
    schedulePersist: () => 0,
  });
  return { player, h, queue };
}

async function flush(times = 3): Promise<void> {
  for (let i = 0; i < times; i++) await Promise.resolve();
}

function primeAndPlay(h: Harness): Promise<void> {
  h.handlers.primed();
  return flush();
}

describe('crossfade trigger semantics (M8 Phase A)', () => {
  it('fires once near the end of the current track and advances cursor/policy', async () => {
    const { player, h, queue } = makePlayer({ crossfadeCapable: true });
    player.init();
    const tracks: (PlaybackItem | null)[] = [];
    player.on((e) => { if (e.t === 'track') tracks.push(e.item); });

    // Multi-item queue: a single-item queue has no fade target by design.
    await player.playSequence([item(1), item(2), item(3)], 'AL', 'A');
    player.setCrossfade(8);
    await primeAndPlay(h);
    expect(queue.get().items.length).toBe(3);

    // render to within the 8 s window of track 1 (30 s track)
    h.rendered = 25 * RATE;
    h.handlers.tick();
    await flush();

    expect(h.beginCrossfade).toHaveBeenCalledOnce();
    expect(h.beginCrossfade.mock.calls[0]?.[1]).toBe(8);
    expect(tracks.some((t) => t?.id === 't2')).toBe(true);
    expect(player.model.get().current?.id).toBe('t2');
    expect(player.model.get().state).not.toBe('error');

    // Phase B: each track can fade once. The next tick lands in track 2's
    // own fade window (cursor advanced to t2), so a chained fade fires.
    h.rendered = 57 * RATE;
    h.handlers.tick();
    await flush();
    expect(h.beginCrossfade).toHaveBeenCalledTimes(2);
  });

  it('does nothing when disabled (default)', async () => {
    const { player, h } = makePlayer({ crossfadeCapable: true });
    await player.playItem(item(1));
    await primeAndPlay(h);
    h.rendered = 29 * RATE;
    h.handlers.tick();
    await flush();
    expect(h.beginCrossfade).not.toHaveBeenCalled();
  });

  it('falls back when the engine lacks the capability', async () => {
    const { player, h } = makePlayer({ crossfadeCapable: false });
    await player.playItem(item(1));
    player.setCrossfade(4);
    await primeAndPlay(h);
    h.rendered = 29 * RATE;
    h.handlers.tick();
    await flush();
    expect(h.beginCrossfade).not.toHaveBeenCalled();
  });

  it('falls back and keeps state clean when beginCrossfade returns null', async () => {
    const { player, h } = makePlayer({ crossfadeCapable: true, fadeResult: null });
    await player.playSequence([item(1), item(2)], 'AL', 'A');
    player.setCrossfade(4);
    await primeAndPlay(h);
    h.rendered = 29 * RATE;
    h.handlers.tick();
    await flush();
    expect(h.beginCrossfade).toHaveBeenCalledOnce();
    // normal EOS path still owns advancement
    expect(player.model.get().current?.id).toBe('t1');
    expect(player.model.get().state).not.toBe('error');
  });

  it('never fades on the last track of the queue', async () => {
    const { player, h } = makePlayer({ crossfadeCapable: true });
    await player.playItem(item(9)); // single-item queue
    player.setCrossfade(12);
    await primeAndPlay(h);
    h.rendered = 29 * RATE;
    h.handlers.tick();
    await flush();
    expect(h.beginCrossfade).not.toHaveBeenCalled();
  });

  it('never fades a single-track repeat-all loop into itself', async () => {
    const { player, h } = makePlayer({ crossfadeCapable: true });
    await player.playItem(item(5));
    player.setRepeat('all');
    player.setCrossfade(8);
    await primeAndPlay(h);
    h.rendered = 29 * RATE;
    h.handlers.tick();
    await flush();
    expect(h.beginCrossfade).not.toHaveBeenCalled();
  });

  it('emits a crossfade event on setCrossfade and persists the setting', () => {
    const { player } = makePlayer({ crossfadeCapable: true });
    const events: PlayerEvent[] = [];
    player.on((e) => events.push(e));
    player.setCrossfade(8);
    expect(events.some((e) => e.t === 'crossfade' && e.seconds === 8)).toBe(true);
    player.setCrossfade(6); // invalid -> clamps to 0
    expect(events.at(-1)).toEqual({ t: 'crossfade', seconds: 0 });
    expect(player.model.get().crossfadeSeconds).toBe(0);
  });
});
