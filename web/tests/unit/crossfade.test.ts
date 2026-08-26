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

/** Item with an explicit decoded length in seconds (short-track tests). */
function mk(n: number, secs: number): PlaybackItem {
  return { ...item(n), durationHintSeconds: secs };
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
  /** Item id the engine's standby was prepared with. beginCrossfade refuses
   *  (returns null) when the requested target differs — standby/policy
   *  agreement, as the real engines enforce. */
  standbyItemId?: string;
  /** Content-aware policy port under test (Sweet Fades). */
  planTransition?: ConstructorParameters<typeof Player>[1]['planTransition'];
}): { player: Player; h: Harness; queue: ReturnType<typeof createQueueModel> } {
  const queue = createQueueModel({ rng: () => 0.5 });
  /** Exact decoded length the fake engine reports for an item. */
  const infoFor = (it: PlaybackItem) => ({
    rate: RATE,
    channels: 2,
    version: 0,
    lengthSamples: Math.floor(((it.durationHintSeconds ?? 30) as number) * RATE),
  });
  const h: Harness = {
    handlers: {} as Harness['handlers'],
    // NOTE: `??` would treat null as absent — but null means "the engine
    // DECLINED the fade" and must be returned as-is.
    beginCrossfade: vi.fn(
      async (it: PlaybackItem, _seconds: number): Promise<CrossfadeResult | null> => {
        // Standby/policy agreement (mirrors the real engines): a fade may
        // only start for the item the standby was actually prepared with.
        if (opts.standbyItemId !== undefined && opts.standbyItemId !== it.id) {
          return null;
        }
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
        if (opts.fadeResult !== undefined) return opts.fadeResult;
        return {
          info: infoFor(it),
          overlapFrames: opts.overlapFrames ?? 0,
        };
      },
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
        open: async (it: PlaybackItem) => infoFor(it),
        prepareNext: async (it: PlaybackItem) => infoFor(it),
        advance: async (expected?: PlaybackItem | null) =>
          // Standby/policy agreement: without a prepared-item model this
          // fake promotes only when an expectation was supplied (the real
          // core always supplies one at EOS).
          expected ? infoFor(expected) : null,
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
  it('refuses a fade when the standby does not match policy and falls back to the normal boundary', async () => {
    // Standby/policy agreement: the fake engine's standby was prepared for
    // t9 (a stale item), so beginCrossfade(t2) must be declined and the
    // positional trigger must leave the boundary to the EOS path.
    const { player, h, queue } = makePlayer({
      crossfadeCapable: true,
      standbyItemId: 't9',
    });
    player.init();
    await player.playSequence([item(1), item(2)], 'AL', 'A');
    player.setCrossfade(8);
    await primeAndPlay(h);

    h.rendered = 25 * RATE; // inside track 1's fade window
    h.handlers.tick();
    await flush();

    expect(h.beginCrossfade).toHaveBeenCalledOnce();
    expect(h.beginCrossfade.mock.calls[0]?.[0].id).toBe('t2'); // policy target
    // Fade declined: cursor untouched, no error, no duplicate trigger.
    expect(player.model.get().current?.id).toBe('t1');
    expect(player.model.get().state).toBe('playing');

    // The natural boundary still advances exactly once.
    h.handlers.eos();
    await flush();
    expect(queue.get().index).toBe(1);
    expect(player.model.get().current?.id).toBe('t2');
    expect(player.model.get().state).not.toBe('error');
  });

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

  it('consults the planner on the EOS-race path and uses the planned overlap', async () => {
    const { player, h } = makePlayer({
      crossfadeCapable: true,
      planTransition: () => ({ type: 'sweet-fade', overlapSeconds: 3 }),
    });
    await player.playSequence([item(1), item(2)], 'AL', 'A');
    player.setCrossfade(8);
    await primeAndPlay(h);
    // Decode raced ahead: worker EOS arrives before any positional tick.
    h.handlers.eos();
    await flush();
    expect(h.beginCrossfade).toHaveBeenCalledTimes(1);
    // Planner's 3 s is used, NOT the raw 8 s cap.
    expect(h.beginCrossfade.mock.calls[0]?.[1]).toBe(3);
    expect(player.model.get().current?.id).toBe('t2');
  });

  it('clamps the planned EOS overlap to the audio actually left', async () => {
    const { player, h } = makePlayer({
      crossfadeCapable: true,
      // Ask for more than remains; the path must clamp to remaining audio.
      planTransition: () => ({ type: 'sweet-fade', overlapSeconds: 12 }),
    });
    await player.playSequence([item(1), item(2)], 'AL', 'A');
    player.setCrossfade(12);
    await primeAndPlay(h);
    // Render most of the track so only ~5 s remains at the EOS.
    h.rendered = 25 * RATE;
    h.handlers.eos();
    await flush();
    expect(h.beginCrossfade).toHaveBeenCalledTimes(1);
    // Clamped to remaining (~5 s), not the planned 12 s.
    expect(h.beginCrossfade.mock.calls[0]?.[1]).toBeCloseTo(5, 1);
    expect(h.beginCrossfade.mock.calls[0]?.[1] ?? 0).toBeLessThan(6);
  });

  it('declines an EOS gapless/hard-cut plan and hands off normally', async () => {
    const { player, h, queue } = makePlayer({
      crossfadeCapable: true,
      planTransition: () => ({ type: 'gapless' }),
    });
    await player.playSequence([item(1), item(2)], 'AL', 'A');
    player.setCrossfade(8);
    await primeAndPlay(h);
    h.handlers.eos();
    await flush();
    // No overlapped transition: the fade is never started.
    expect(h.beginCrossfade).not.toHaveBeenCalled();
    // The normal EOS handoff still advances exactly once.
    expect(queue.get().index).toBe(1);
    expect(player.model.get().current?.id).toBe('t2');
  });

  it('keeps the fixed-cap fallback on EOS when no planner is wired', async () => {
    const { player, h } = makePlayer({ crossfadeCapable: true });
    await player.playSequence([item(1), item(2)], 'AL', 'A');
    player.setCrossfade(8);
    await primeAndPlay(h);
    h.handlers.eos();
    await flush();
    expect(h.beginCrossfade).toHaveBeenCalledTimes(1);
    // No port → legacy fixed behaviour: min(cap 8 s, remaining 30 s) = 8 s.
    expect(h.beginCrossfade.mock.calls[0]?.[1]).toBe(8);
  });

  it('progresses through short tracks into a normal track without stalling', async () => {
    const { player, h } = makePlayer({ crossfadeCapable: true });
    await player.playSequence([mk(1, 48), mk(2, 1), mk(3, 1), mk(4, 48)], 'AL', 'A');
    player.setCrossfade(4);
    await primeAndPlay(h);

    // Boundary 1 (normal -> short): positional trigger near the opener end.
    h.rendered = 46.5 * RATE;
    h.handlers.tick();
    await flush();
    expect(h.beginCrossfade).toHaveBeenCalled();
    expect(player.model.get().current?.id).toBe('t2');

    // Boundaries 2 and 3 (short -> short -> normal): each decode-EOS lands
    // while audio remains, so the last-chance fade advances exactly one
    // track per EOS — never zero, never two.
    for (const id of ['t3', 't4']) {
      h.handlers.eos();
      await flush();
      expect(player.model.get().current?.id).toBe(id);
    }

    // Termination: the final track's EOS still ends the queue cleanly.
    h.rendered = 99 * RATE; // past the 98 s compressed total
    h.handlers.eos();
    await flush();
    h.handlers.tick();
    await flush();
    expect(player.model.get().state).toBe('ended');
  });

  it('handles several consecutive short tracks with a fade at every boundary', async () => {
    const { player, h } = makePlayer({ crossfadeCapable: true });
    await player.playSequence([mk(1, 1), mk(2, 1), mk(3, 1), mk(4, 1)], 'AL', 'A');
    player.setCrossfade(12); // window far longer than any track
    await primeAndPlay(h);

    // One logical advancement per decode-EOS, across every boundary,
    // including the final termination.
    for (const id of ['t2', 't3', 't4']) {
      h.handlers.eos();
      await flush();
      expect(player.model.get().current?.id).toBe(id);
    }
    const calls = h.beginCrossfade.mock.calls.length;
    expect(calls).toBeGreaterThanOrEqual(2); // faded, not merely gapless

    h.rendered = 10 * RATE; // past the 4 s total
    h.handlers.eos();
    await flush();
    h.handlers.tick();
    await flush();
    expect(player.model.get().state).toBe('ended');
  });
});
