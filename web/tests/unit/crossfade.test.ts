// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

import { describe, expect, it, vi } from 'vitest';
import { Player } from '../../player-core/src/player';
import { createQueueModel } from '../../player-core/src/queue';
import type { PlayerEvent } from '../../player-core/src/events';
import type { CrossfadeResult } from '../../player-core/src/engine';
import type { PlaybackItem } from '../../player-core/src/types';

const RATE = 44100;
const LEN = RATE * 30; // 30 s tracks

const FADE_OK: CrossfadeResult = {
  info: { rate: RATE, channels: 2, version: 0, lengthSamples: LEN },
  overlapFrames: 0,
};

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
  /** Set while a deferred fade is pending (opts.deferredFade). */
  resolveFade?: ((r: CrossfadeResult | null) => void) | null;
}

/** Fake engine with controllable crossfade support. */
function makePlayer(opts: {
  crossfadeCapable: boolean;
  fadeResult?: CrossfadeResult | null;
  /** Overlap reported by a successful fade (default 0 = legacy accounting). */
  overlapFrames?: number;
  /** Hold the first beginCrossfade call open until resolveFade is invoked. */
  deferredFade?: boolean;
  /** Content-aware policy port under test (Sweet Fades). */
  planTransition?: ConstructorParameters<typeof Player>[1]['planTransition'];
}): { player: Player; h: Harness; queue: ReturnType<typeof createQueueModel> } {
  const queue = createQueueModel({ rng: () => 0.5 });
  const h: Harness = {
    handlers: {} as Harness['handlers'],
    // NOTE: `??` would treat null as absent — but null means "the engine
    // DECLINED the fade" and must be returned as-is.
    beginCrossfade: vi.fn(async (): Promise<CrossfadeResult | null> => {
      if (opts.deferredFade) {
        if (!h.resolveFade) {
          return new Promise<CrossfadeResult | null>((resolve) => {
            h.resolveFade = (r) => {
              h.resolveFade = null;
              resolve(r);
            };
          });
        }
        return null;
      }
      return opts.fadeResult === undefined
        ? { ...FADE_OK, overlapFrames: opts.overlapFrames ?? 0 }
        : opts.fadeResult;
    }),
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
    planTransition: opts.planTransition,
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

  it('never fades under repeat-one (BUG-5: reload semantics win)', async () => {
    const { player, h } = makePlayer({ crossfadeCapable: true });
    // Multi-item queue so a fade target WOULD exist under the old policy.
    await player.playSequence([item(1), item(2), item(3)], 'AL', 'A');
    player.setRepeat('one');
    player.setCrossfade(8);
    await primeAndPlay(h);
    h.rendered = 25 * RATE; // inside the 8 s window of track 1
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

  it('shrinks the outgoing track by the reported overlap (BUG-2: album clock)', async () => {
    const { player, h } = makePlayer({ crossfadeCapable: true, overlapFrames: 8 * RATE });
    await player.playSequence([item(1), item(2)], 'AL', 'A');
    player.setCrossfade(8);
    await primeAndPlay(h);
    h.rendered = 25 * RATE; // inside the window
    h.handlers.tick();
    await flush();

    const m = player.model.get();
    expect(m.current?.id).toBe('t2');
    // t1 keeps 30 - 8 = 22 s of effective length: t2 starts at 22 s and the
    // album total compresses to 52 s. Positions, seek mapping and end
    // detection all key off these numbers.
    expect(m.currentTrackStartSeconds).toBeCloseTo(22, 3);
    expect(m.durationSeconds).toBeCloseTo(52, 3);
  });

  it('completes the handoff after a pause mid-fade and never re-triggers (BUG-4)', async () => {
    const { player, h } = makePlayer({ crossfadeCapable: true, deferredFade: true });
    await player.playSequence([item(1), item(2)], 'AL', 'A');
    player.setCrossfade(4);
    await primeAndPlay(h);

    h.rendered = 27 * RATE; // inside the 4 s window
    h.handlers.tick();
    await flush();
    expect(h.beginCrossfade).toHaveBeenCalledOnce();
    expect(h.resolveFade).toBeTruthy();

    // Pause while the engine-side fade is still awaiting its swap.
    await player.pause();

    // While the attempt is live, no second trigger may fire.
    h.handlers.tick();
    await flush();
    expect(h.beginCrossfade).toHaveBeenCalledOnce();

    // The fade completes (e.g. after resume): the handoff bookkeeping must
    // still apply even though transportSeq moved with the pause.
    h.resolveFade!({ ...FADE_OK });
    await flush();
    expect(player.model.get().current?.id).toBe('t2');
    expect(h.resolveFade).toBeNull();
  });

  it('skips handoff bookkeeping when a load superseded the transition', async () => {
    const { player, h } = makePlayer({ crossfadeCapable: true, deferredFade: true });
    await player.playSequence([item(1), item(2)], 'AL', 'A');
    player.setCrossfade(4);
    await primeAndPlay(h);

    h.rendered = 27 * RATE;
    h.handlers.tick();
    await flush();
    expect(h.beginCrossfade).toHaveBeenCalledOnce();

    // A seek starts a new load generation while the fade awaits: the fade
    // result is stale and must NOT move the cursor/media/gain state.
    await player.seek(5);
    h.resolveFade!({ ...FADE_OK });
    await flush();

    expect(player.model.get().current?.id).toBe('t1');
    expect(player.model.get().state).not.toBe('error');
  });

  it('honors a gapless plan from the policy port (no fade at the boundary)', async () => {
    const planCalls: unknown[] = [];
    const { player, h } = makePlayer({
      crossfadeCapable: true,
      planTransition: (query) => {
        planCalls.push(query);
        return { type: 'gapless' };
      },
    });
    await player.playSequence([item(1), item(2)], 'AL', 'A');
    player.setCrossfade(8);
    await primeAndPlay(h);
    h.rendered = 25 * RATE;
    h.handlers.tick();
    await flush();
    expect(h.beginCrossfade).not.toHaveBeenCalled();
    expect(planCalls.length).toBeGreaterThan(0);
  });

  it('passes the planned overlap to the engine instead of the raw setting', async () => {
    const { player, h } = makePlayer({
      crossfadeCapable: true,
      planTransition: () => ({ type: 'sweet-fade', overlapSeconds: 2 }),
    });
    await player.playSequence([item(1), item(2)], 'AL', 'A');
    player.setCrossfade(8);
    await primeAndPlay(h);
    h.rendered = 25 * RATE; // remaining ~5 s: inside the 8 s cap, outside 2 s+lead
    h.handlers.tick();
    await flush();
    expect(h.beginCrossfade).not.toHaveBeenCalled(); // not yet — 2 s + 1 s lead

    h.rendered = 28.5 * RATE; // remaining ~1.5 s ≤ 2 s + lead
    h.handlers.tick();
    await flush();
    expect(h.beginCrossfade).toHaveBeenCalledTimes(1);
    expect(h.beginCrossfade.mock.calls[0]?.[1]).toBe(2);
  });

  it('takes a last-chance fade when decoder EOS beats the positional trigger', async () => {
    const { player, h } = makePlayer({ crossfadeCapable: true });
    await player.playSequence([item(1), item(2)], 'AL', 'A');
    player.setCrossfade(8);
    await primeAndPlay(h);
    // Decode raced ahead: worker EOS arrives while NO positional tick has
    // observed the window yet (rendered still at 0).
    h.handlers.eos();
    await flush();
    expect(h.beginCrossfade).toHaveBeenCalledTimes(1);
    // Overlap clamped to what is actually left: min(cap 8 s, remaining 30 s).
    expect(h.beginCrossfade.mock.calls[0]?.[1]).toBe(8);
    expect(player.model.get().current?.id).toBe('t2');
  });

  it('falls back to the normal EOS handoff when the last-chance fade declines', async () => {
    const { player, h } = makePlayer({ crossfadeCapable: true, fadeResult: null });
    await player.playSequence([item(1), item(2)], 'AL', 'A');
    player.setCrossfade(8);
    await primeAndPlay(h);
    h.handlers.eos();
    await flush();
    expect(h.beginCrossfade).toHaveBeenCalledTimes(1);
    // Declined: the gapless advance owns the boundary as before.
    expect(player.model.get().current?.id).toBe('t2');
    expect(player.model.get().state).not.toBe('error');
  });

  it('never takes the last-chance fade under repeat-one', async () => {
    const { player, h } = makePlayer({ crossfadeCapable: true });
    await player.playSequence([item(1), item(2)], 'AL', 'A');
    player.setRepeat('one');
    player.setCrossfade(8);
    await primeAndPlay(h);
    h.handlers.eos();
    await flush(6);
    expect(h.beginCrossfade).not.toHaveBeenCalled();
    // Reload semantics: same item stays current through a fresh load.
    expect(player.model.get().current?.id).toBe('t1');
  });
});
