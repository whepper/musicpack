// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

import { beforeAll, beforeEach, describe, expect, it, vi } from 'vitest';
import type { WorkletReport } from '../../app/src/lib/playback/worklet-protocol';

class FakePort {
  onmessage: ((event: MessageEvent) => void) | null = null;
  messages: WorkletReport[] = [];

  postMessage(message: WorkletReport): void {
    this.messages.push(message);
  }

  send(data: unknown): void {
    this.onmessage?.({ data } as MessageEvent);
  }
}

class FakeAudioWorkletProcessor {
  readonly port = new FakePort() as unknown as MessagePort;
}

interface Processor {
  readonly port: MessagePort;
  process(inputs: Float32Array[][], outputs: Float32Array[][]): boolean;
}

let ProcessorCtor: new () => Processor;

beforeAll(async () => {
  vi.stubGlobal('AudioWorkletProcessor', FakeAudioWorkletProcessor);
  vi.stubGlobal('currentTime', 0);
  vi.stubGlobal('registerProcessor', (_name: string, ctor: new () => Processor) => {
    ProcessorCtor = ctor;
  });
  await import('../../app/src/lib/playback/audio-worklet');
});

beforeEach(() => {
  vi.stubGlobal('currentTime', 0);
});

function createProcessor(
  rate = 10,
  channels = 1,
  generation = 1,
): { processor: Processor; port: FakePort } {
  const processor = new ProcessorCtor();
  const port = processor.port as unknown as FakePort;
  port.send({ type: 'config', rate, channels, generation });
  return { processor, port };
}

function samples(frames: number, start = 0): Float32Array {
  return Float32Array.from({ length: frames }, (_, i) => start + i);
}

function render(processor: Processor, frames: number): Float32Array {
  const output = new Float32Array(frames);
  processor.process([], [[output]]);
  return output;
}

describe('MusicPackPcmProcessor backpressure', () => {
  it('pauses at high water and resumes after reads cross low water', () => {
    const { processor, port } = createProcessor(); // 80-frame ring
    const pcm = samples(100, 1);
    port.send({ type: 'samples', buffer: pcm.buffer, generation: 1 });

    expect(port.messages.filter((message) => message.type === 'full')).toHaveLength(1);
    expect(port.messages.some((message) => message.type === 'accepted')).toBe(false);
    render(processor, 40); // pending PCM refills 20 of the consumed frames
    expect(port.messages.filter((message) => message.type === 'accepted')).toHaveLength(1);
    expect(port.messages.some((message) => message.type === 'need')).toBe(false);
    render(processor, 45); // 15 frames remain: below the 16-frame low water
    expect(port.messages.filter((message) => message.type === 'need')).toHaveLength(1);
  });

  it('retains a partial ring write and renders every PCM frame in order', () => {
    const { processor, port } = createProcessor();
    const pcm = samples(100, 1);
    port.send({ type: 'samples', buffer: pcm.buffer, generation: 1 });

    const rendered = [
      ...render(processor, 40),
      ...render(processor, 45),
      ...render(processor, 20).slice(0, 15),
    ];
    expect(rendered).toEqual(Array.from(pcm));
  });

  it('reports an underrun when insufficient PCM makes it render silence', () => {
    const { processor, port } = createProcessor();
    const pcm = samples(5, 1);
    port.send({ type: 'samples', buffer: pcm.buffer, generation: 1 });

    const output = render(processor, 8);
    expect(Array.from(output)).toEqual([1, 2, 3, 4, 5, 0, 0, 0]);
    expect(port.messages.filter((message) => message.type === 'underrun')).toEqual([
      expect.objectContaining({ type: 'underrun', frames: 5, available: 5 }),
    ]);
  });

  it('ignores stale PCM and stale resets from an earlier generation', () => {
    const { processor, port } = createProcessor();
    const current = samples(4, 10);
    port.send({ type: 'reset', generation: 2 });
    port.send({ type: 'samples', buffer: samples(4, 100).buffer, generation: 1 });
    port.send({ type: 'reset', generation: 1 });
    port.send({ type: 'samples', buffer: current.buffer, generation: 2 });

    expect(Array.from(render(processor, 4))).toEqual(Array.from(current));
    expect(port.messages.filter((message) => message.type === 'accepted')).toEqual([
      expect.objectContaining({ type: 'accepted', generation: 2 }),
    ]);
  });
});
