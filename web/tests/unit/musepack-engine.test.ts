// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

import { describe, expect, it, vi } from 'vitest';
import { MusepackEngine } from '../../app/src/lib/playback/musepack-engine';
import type { WorkletReport } from '../../app/src/lib/playback/worklet-protocol';

interface EngineHarness {
  generation: number;
  transitioning: boolean;
  current: {
    worker: { postMessage: ReturnType<typeof vi.fn> };
    info: null;
    eos: false;
    nextUrl: null;
    cancelOpen?: (() => void) | null;
  };
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
    }
  }

  terminate(): void {
    this.terminated = true;
  }

  emitInfo(generation: number): void {
    this.onmessage?.({
      data: { type: 'info', rate: 44100, channels: 2, version: 8, lengthSamples: 441000, generation },
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
      eos: false,
      nextUrl: null,
    };
    engine.startPumping();

    harness.onWorkletMessage({ type: 'underrun', frames: 0, generation: 3, available: 0 });
    harness.onWorkletMessage({ type: 'need', frames: 0, generation: 3, available: 0 });
    expect(postMessage).not.toHaveBeenCalled();
  });
});
