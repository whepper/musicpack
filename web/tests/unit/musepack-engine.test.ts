// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

import { describe, expect, it, vi } from 'vitest';
import { MusepackEngine } from '../../app/src/lib/playback/musepack-engine';
import type { EngineStreamInfo } from '../../app/src/lib/playback/musepack-engine';
import type { WorkletReport } from '../../app/src/lib/playback/worklet-protocol';

interface EngineHarness {
  generation: number;
  transitioning: boolean;
  current: {
    worker: { postMessage: ReturnType<typeof vi.fn> };
    info: EngineStreamInfo | null;
    sourceInfo: EngineStreamInfo | null;
    eos: boolean;
    nextUrl: null;
    cancelOpen?: (() => void) | null;
  };
  standby: EngineHarness['current'] | null;
  ctx: AudioContext | null;
  node: { port: { postMessage: ReturnType<typeof vi.fn> } } | null;
  makeWorker(): EngineHarness['current'];
  onWorkletMessage(message: WorkletReport): void;
  onWorkerMessage(handle: EngineHarness['current'], message: Record<string, unknown>): void;
}

class AsyncWorker {
  onmessage: ((event: MessageEvent) => void) | null = null;
  onerror: ((event: ErrorEvent) => void) | null = null;
  messages: Record<string, unknown>[] = [];
  terminated = false;

  postMessage(message: Record<string, unknown>): void {
    this.messages.push(message);
    if (message.type === 'close') {
      queueMicrotask(() => this.onmessage?.({ data: { type: 'closed' } } as MessageEvent));
    } else if (message.type === 'seek') {
      queueMicrotask(() =>
        this.onmessage?.({
          data: {
            type: 'seeked',
            sample: message.sample,
            generation: message.generation,
          },
        } as MessageEvent),
      );
    }
  }

  terminate(): void {
    this.terminated = true;
  }

  emitInfo(generation: number, rate = 44100, channels = 2, lengthSamples = rate * 10): void {
    this.onmessage?.({
      data: { type: 'info', rate, channels, version: 8, lengthSamples, generation },
    } as MessageEvent);
  }
}

describe('MusepackEngine backpressure reports', () => {
  it('keeps the newest worker when overlapping opens complete out of order', async () => {
    const engine = new MusepackEngine({
      onPrimed: vi.fn(),
      onBuffering: vi.fn(),
      onEos: vi.fn(),
      onError: vi.fn(),
      onPosition: vi.fn(),
    });
    const harness = engine as unknown as EngineHarness;
    const workers: AsyncWorker[] = [];
    harness.ctx = {} as AudioContext;
    harness.node = { port: { postMessage: vi.fn() } };
    harness.makeWorker = () => {
      const worker = new AsyncWorker();
      workers.push(worker);
      return {
        worker: worker as unknown as { postMessage: ReturnType<typeof vi.fn> },
        info: null,
        sourceInfo: null,
        eos: false,
        nextUrl: null,
        cancelOpen: null,
      };
    };

    const first = engine.open('/first.mpc', 100).then(
      () => 'resolved',
      () => 'rejected',
    );
    await vi.waitFor(() => expect(workers).toHaveLength(1));
    const second = engine.open('/second.mpc', 200);
    await vi.waitFor(() => expect(workers).toHaveLength(2));
    workers[1]?.emitInfo(2);

    await expect(second).resolves.toMatchObject({ rate: 44100, lengthSamples: 441000 });
    await expect(first).resolves.toBe('rejected');
    expect(workers[0]?.terminated).toBe(true);
    expect((harness.current.worker as unknown as AsyncWorker)).toBe(workers[1]);

    workers[0]?.emitInfo(1);
    expect((harness.current.worker as unknown as AsyncWorker)).toBe(workers[1]);
  });

  it('pauses at high water, resumes below low water, and reports underruns', () => {
    const onBuffering = vi.fn();
    const engine = new MusepackEngine({
      onPrimed: vi.fn(),
      onBuffering,
      onEos: vi.fn(),
      onError: vi.fn(),
      onPosition: vi.fn(),
    });
    const harness = engine as unknown as EngineHarness;
    const postMessage = vi.fn();
    harness.current = {
      worker: { postMessage },
      info: null,
      sourceInfo: null,
      eos: false,
      nextUrl: null,
    };

    engine.startPumping();
    postMessage.mockClear();
    harness.onWorkletMessage({ type: 'full', frames: 0, available: 64, generation: 0 });
    engine.startPumping(); // transport resume must not bypass the full latch
    harness.onWorkletMessage({ type: 'accepted', frames: 0, available: 64, generation: 0 });
    harness.onWorkletMessage({ type: 'need', frames: 49, available: 15, generation: 0 });
    harness.onWorkletMessage({ type: 'underrun', frames: 64, available: 0, generation: 0 });

    expect(postMessage.mock.calls.map(([message]) => message)).toEqual([
      { type: 'pause', generation: 0 },
      { type: 'play', generation: 0 },
    ]);
    expect(onBuffering).toHaveBeenCalledOnce();
  });

  it('ignores stale worklet reports and stale worker PCM', () => {
    const engine = new MusepackEngine({
      onPrimed: vi.fn(),
      onBuffering: vi.fn(),
      onEos: vi.fn(),
      onError: vi.fn(),
      onPosition: vi.fn(),
    });
    const harness = engine as unknown as EngineHarness;
    const workerPost = vi.fn();
    const workletPost = vi.fn();
    harness.generation = 2;
    harness.current = {
      worker: { postMessage: workerPost },
      info: null,
      sourceInfo: null,
      eos: false,
      nextUrl: null,
    };
    harness.node = { port: { postMessage: workletPost } };

    harness.onWorkletMessage({ type: 'need', frames: 0, generation: 1 });
    harness.onWorkletMessage({ type: 'full', frames: 0, generation: 1 });
    harness.onWorkerMessage(harness.current, {
      type: 'pcm',
      generation: 1,
      samples: Float32Array.of(1),
    });
    expect(workerPost).not.toHaveBeenCalled();
    expect(workletPost).not.toHaveBeenCalled();

    const current = Float32Array.of(2);
    harness.onWorkerMessage(harness.current, { type: 'pcm', generation: 2, samples: current });
    expect(workletPost).toHaveBeenCalledWith(
      { type: 'samples', buffer: current.buffer, generation: 2 },
      [current.buffer],
    );
  });

  it('does not pull PCM from reset reports while a generation transition is pending', () => {
    const engine = new MusepackEngine({
      onPrimed: vi.fn(),
      onBuffering: vi.fn(),
      onEos: vi.fn(),
      onError: vi.fn(),
      onPosition: vi.fn(),
    });
    const harness = engine as unknown as EngineHarness;
    const postMessage = vi.fn();
    harness.generation = 3;
    harness.transitioning = true;
    harness.current = {
      worker: { postMessage },
      info: null,
      sourceInfo: null,
      eos: false,
      nextUrl: null,
    };
    engine.startPumping();

    harness.onWorkletMessage({ type: 'underrun', frames: 0, generation: 3, available: 0 });
    harness.onWorkletMessage({ type: 'need', frames: 0, generation: 3, available: 0 });
    expect(postMessage).not.toHaveBeenCalled();
  });

  it('normalizes stream timing to the AudioContext rate and converts seeks to source frames', async () => {
    const onPosition = vi.fn();
    const engine = new MusepackEngine({
      onPrimed: vi.fn(),
      onBuffering: vi.fn(),
      onEos: vi.fn(),
      onError: vi.fn(),
      onPosition,
    });
    const harness = engine as unknown as EngineHarness;
    const workers: AsyncWorker[] = [];
    harness.ctx = { sampleRate: 48000 } as AudioContext;
    harness.node = { port: { postMessage: vi.fn() } };
    harness.makeWorker = () => {
      const worker = new AsyncWorker();
      workers.push(worker);
      return {
        worker: worker as unknown as { postMessage: ReturnType<typeof vi.fn> },
        info: null,
        sourceInfo: null,
        eos: false,
        nextUrl: null,
        cancelOpen: null,
      };
    };

    const opening = engine.open('/441.mpc', 100);
    await vi.waitFor(() => expect(workers).toHaveLength(1));
    workers[0]?.emitInfo(1, 44100, 2, 441000);
    await expect(opening).resolves.toMatchObject({ rate: 48000, channels: 2, lengthSamples: 480000 });
    if (workers[0]) {
      workers[0].onmessage = (event) => harness.onWorkerMessage(harness.current, event.data);
    }

    const seeking = engine.seek(48000);
    const seekMessage = workers[0]?.messages.find((message) => message.type === 'seek');
    expect(seekMessage).toMatchObject({ sample: 44100, generation: 2 });
    await seeking;

    harness.onWorkletMessage({ type: 'rendered', frames: 4800, generation: 2 });
    expect(engine.getPositionSamples()).toBe(52800);
    expect(engine.getRenderedSamples()).toBe(4800);
    expect(onPosition).toHaveBeenLastCalledWith(52800);
  });

  it('does not resume decoder pumping after a paused seek', async () => {
    const engine = new MusepackEngine({
      onPrimed: vi.fn(),
      onBuffering: vi.fn(),
      onEos: vi.fn(),
      onError: vi.fn(),
      onPosition: vi.fn(),
    });
    const harness = engine as unknown as EngineHarness;
    const postMessage = vi.fn();
    harness.generation = 4;
    harness.current = {
      worker: { postMessage },
      info: null,
      sourceInfo: { rate: 44100, channels: 2, version: 8, lengthSamples: 441000 },
      eos: false,
      nextUrl: null,
    };
    harness.node = { port: { postMessage: vi.fn() } };
    engine.pausePumping();
    postMessage.mockClear();

    const seeking = engine.seek(48000);
    harness.onWorkerMessage(harness.current, { type: 'seeked', sample: 44100, generation: 5 });
    await seeking;
    expect(postMessage.mock.calls.map(([message]) => message.type)).toEqual(['seek']);
  });

  it('does not restore pumping when pause arrives during an in-flight seek', async () => {
    const engine = new MusepackEngine({
      onPrimed: vi.fn(),
      onBuffering: vi.fn(),
      onEos: vi.fn(),
      onError: vi.fn(),
      onPosition: vi.fn(),
    });
    const harness = engine as unknown as EngineHarness;
    const postMessage = vi.fn();
    harness.generation = 8;
    harness.current = {
      worker: { postMessage },
      info: null,
      sourceInfo: { rate: 44100, channels: 2, version: 8, lengthSamples: 441000 },
      eos: false,
      nextUrl: null,
    };
    harness.node = { port: { postMessage: vi.fn() } };
    engine.startPumping();
    postMessage.mockClear();

    const seeking = engine.seek(44100);
    engine.pausePumping();
    harness.onWorkerMessage(harness.current, { type: 'seeked', sample: 44100, generation: 9 });
    await seeking;
    expect(postMessage.mock.calls.map(([message]) => message.type)).toEqual(['seek', 'pause']);
  });

  it('re-suspends the AudioContext when pause wins an in-flight resume race', async () => {
    const engine = new MusepackEngine({
      onPrimed: vi.fn(),
      onBuffering: vi.fn(),
      onEos: vi.fn(),
      onError: vi.fn(),
      onPosition: vi.fn(),
    });
    const harness = engine as unknown as EngineHarness;
    let finishResume = () => {};
    const ctx = {
      state: 'suspended',
      resume: vi.fn(() => new Promise<void>((resolve) => {
        finishResume = () => {
          ctx.state = 'running';
          resolve();
        };
      })),
      suspend: vi.fn(async () => {
        ctx.state = 'suspended';
      }),
    };
    harness.ctx = ctx as unknown as AudioContext;

    const playing = engine.play();
    const pausing = engine.pause();
    finishResume();
    await Promise.all([playing, pausing]);
    expect(ctx.state).toBe('suspended');
    expect(ctx.suspend).toHaveBeenCalled();
  });

  it('finishes the prior track before switching a gapless stream to a new source format', async () => {
    const engine = new MusepackEngine({
      onPrimed: vi.fn(),
      onBuffering: vi.fn(),
      onEos: vi.fn(),
      onError: vi.fn(),
      onPosition: vi.fn(),
    });
    const harness = engine as unknown as EngineHarness;
    const portPost = vi.fn();
    const currentWorker = new AsyncWorker();
    const standbyWorker = new AsyncWorker();
    harness.generation = 7;
    harness.node = { port: { postMessage: portPost } };
    harness.current = {
      worker: currentWorker as unknown as { postMessage: ReturnType<typeof vi.fn> },
      info: { rate: 48000, channels: 2, version: 8, lengthSamples: 480000 },
      sourceInfo: { rate: 44100, channels: 2, version: 8, lengthSamples: 441000 },
      eos: true,
      nextUrl: null,
    };
    harness.standby = {
      worker: standbyWorker as unknown as { postMessage: ReturnType<typeof vi.fn> },
      info: { rate: 48000, channels: 2, version: 8, lengthSamples: 240000 },
      sourceInfo: { rate: 48000, channels: 1, version: 8, lengthSamples: 240000 },
      eos: false,
      nextUrl: null,
    };

    const advancing = engine.advance();
    expect(portPost).toHaveBeenCalledWith({ type: 'end', generation: 7 });
    expect(harness.current.worker).toBe(
      currentWorker as unknown as { postMessage: ReturnType<typeof vi.fn> },
    );

    harness.onWorkletMessage({ type: 'trackEnded', frames: 100, generation: 7 });
    await expect(advancing).resolves.toMatchObject({ rate: 48000, lengthSamples: 240000 });
    expect(portPost).toHaveBeenLastCalledWith({
      type: 'track',
      sourceRate: 48000,
      sourceChannels: 1,
      generation: 7,
    });
    expect(portPost.mock.calls.some(([message]) => message.type === 'reset')).toBe(false);
  });
});
