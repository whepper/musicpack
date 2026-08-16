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
  sourceRate = 10,
  sourceChannels = 1,
  generation = 1,
  outputRate = sourceRate,
  outputChannels = sourceChannels,
): { processor: Processor; port: FakePort } {
  const processor = new ProcessorCtor();
  const port = processor.port as unknown as FakePort;
  port.send({
    type: 'config',
    sourceRate,
    sourceChannels,
    outputRate,
    outputChannels,
    generation,
  });
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

  it('primes a short stream when EOS arrives below the normal watermark', () => {
    const { port } = createProcessor(); // 80-frame ring, 16-frame prime threshold
    const pcm = samples(5, 1);
    port.send({ type: 'samples', buffer: pcm.buffer, generation: 1 });
    expect(port.messages.some((message) => message.type === 'primed')).toBe(false);

    port.send({ type: 'end', generation: 1 });
    expect(port.messages.filter((message) => message.type === 'primed')).toHaveLength(1);
    expect(port.messages.filter((message) => message.type === 'trackEnded')).toHaveLength(1);
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

  it('renders 44.1 kHz source at the duration and pitch implied by a 48 kHz context', () => {
    const { processor, port } = createProcessor(44100, 1, 1, 48000, 1);
    const pcm = samples(441);
    port.send({ type: 'samples', buffer: pcm.buffer, generation: 1 });
    port.send({ type: 'end', generation: 1 });

    const output = render(processor, 480);
    expect(output).toHaveLength(480);
    expect(output[160]).toBeCloseTo(147, 5);
    expect(output[320]).toBeCloseTo(294, 5);
    expect(output[479]).toBeCloseTo(440, 5);
    expect(port.messages).toContainEqual(expect.objectContaining({ type: 'trackEnded' }));
  });

  it('renders 48 kHz source at the duration implied by a 44.1 kHz context', () => {
    const { processor, port } = createProcessor(48000, 1, 1, 44100, 1);
    const pcm = samples(480);
    port.send({ type: 'samples', buffer: pcm.buffer, generation: 1 });
    port.send({ type: 'end', generation: 1 });

    const output = render(processor, 441);
    expect(output[147]).toBeCloseTo(160, 5);
    expect(output[294]).toBeCloseTo(320, 5);
    expect(output[440]).toBeCloseTo((440 * 48000) / 44100, 5);
    expect(port.messages).toContainEqual(expect.objectContaining({ type: 'trackEnded' }));
  });

  it('keeps normalized output queued across tracks with different rates and channels', () => {
    const { processor, port } = createProcessor(10, 1, 1, 20, 2);
    const mono = Float32Array.of(1, 2, 3);
    port.send({ type: 'samples', buffer: mono.buffer, generation: 1 });
    port.send({ type: 'end', generation: 1 });
    port.send({ type: 'track', sourceRate: 20, sourceChannels: 2, generation: 1 });
    const stereo = Float32Array.of(10, 20, 11, 21);
    port.send({ type: 'samples', buffer: stereo.buffer, generation: 1 });
    port.send({ type: 'end', generation: 1 });

    const left = new Float32Array(8);
    const right = new Float32Array(8);
    processor.process([], [[left, right]]);
    expect(Array.from(left)).toEqual([1, 1.5, 2, 2.5, 3, 3, 10, 11]);
    expect(Array.from(right)).toEqual([1, 1.5, 2, 2.5, 3, 3, 20, 21]);
  });
});
