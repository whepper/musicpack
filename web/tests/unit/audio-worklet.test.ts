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

describe('MusicPackPcmProcessor crossfade lane (M8 Phase B)', () => {
  function armFade(
    fadeFrames = 4,
    sourceRate = 10,
    generation = 1,
    token = 7,
  ): { processor: Processor; port: FakePort; go: () => void } {
    const { processor, port } = createProcessor(sourceRate, 1, generation);
    port.send({ type: 'xfade', sourceRate, sourceChannels: 1, fadeFrames, token, generation });
    return {
      processor,
      port,
      go: (): void => port.send({ type: 'xfade-go', token, generation }),
    };
  }

  /** Sends lane PCM/end stamped with the given attempt token. */
  function sendX(
    port: FakePort,
    type: 'xsamples' | 'xend',
    data: Float32Array | null,
    token: number,
    generation = 1,
  ): void {
    if (type === 'xsamples' && data) {
      port.send({ type, buffer: data.buffer, token, generation });
    } else if (type === 'xend') {
      port.send({ type, token, generation });
    }
  }

  it('mixes an equal-power overlap and swaps rings at the end of the window', () => {
    // Outgoing: a constant 1.0 stream; incoming: a constant 0.5 stream.
    const { processor, port, go } = armFade(4);
    sendX(port, 'xsamples', Float32Array.of(0.5, 0.5, 0.5), 7);
    port.send({ type: 'samples', buffer: Float32Array.of(1, 1, 1, 1, 1, 1).buffer, generation: 1 });
    sendX(port, 'xend', null, 7);
    go();

    // The whole track fits in one callback: mix 4 frames, then swap. The
    // lane holds exactly 3 frames (the track's full output), so the 4th
    // fade frame has no incoming audio (silence at full sine gain — the
    // engine only starts a fade when the lane holds the whole fade window,
    // so this cannot happen in production; the test pins the defensive
    // behavior). The 0.5-frames were consumed by the mix, so after the swap
    // the new ring is empty: frame 4 renders silence.
    const out = render(processor, 5);
    expect(out[0]).toBeCloseTo(1, 5); // cos(0)=1, sin(0)=0
    expect(out[1]).toBeCloseTo(Math.cos((1 / 4) * Math.PI / 2) + 0.5 * Math.sin((1 / 4) * Math.PI / 2), 5);
    expect(out[2]).toBeCloseTo(Math.cos((2 / 4) * Math.PI / 2) + 0.5 * Math.sin((2 / 4) * Math.PI / 2), 5);
    expect(out[3]).toBeCloseTo(Math.cos((3 / 4) * Math.PI / 2), 5);
    expect(out[4]).toBe(0); // post-swap ring drained

    const xfaded = port.messages.find((m) => m.type === 'xfaded');
    expect(xfaded).toMatchObject({ outgoingFrames: 4, incomingFrames: 3, token: 7 });
    expect(xfaded?.available).toBe(0); // ring swapped to the drained lane
  });

  it('reports xfadeReady only when the whole incoming track is queued', () => {
    const { port } = armFade(2);
    expect(port.messages.some((m) => m.type === 'xfadeReady')).toBe(false);

    sendX(port, 'xsamples', Float32Array.of(9, 9), 7);
    expect(port.messages.some((m) => m.type === 'xfadeReady')).toBe(false);

    sendX(port, 'xend', null, 7);
    const ready = port.messages.find((m) => m.type === 'xfadeReady');
    expect(ready).toMatchObject({ available: 2, token: 7 });
  });

  it('keeps rendering the outgoing track while the armed lane fills up', () => {
    const { processor, port } = armFade(4);
    port.send({ type: 'samples', buffer: Float32Array.of(3, 3, 3).buffer, generation: 1 });
    sendX(port, 'xsamples', Float32Array.of(7, 7, 7), 7);

    const out = render(processor, 3);
    expect(Array.from(out)).toEqual([3, 3, 3]); // no go yet: no mixing
    expect(port.messages.some((m) => m.type === 'xfaded')).toBe(false);
  });

  it('ignores stale xsamples from a cancelled attempt and honours a re-armed lane', () => {
    const { processor, port } = armFade(2, 10, 1, 7);
    sendX(port, 'xsamples', Float32Array.of(1), 7);
    sendX(port, 'xend', null, 7);
    port.send({ type: 'xfade-cancel', generation: 1 }); // token bumped, lane dropped

    // Stale-token stragglers from the cancelled attempt are dropped.
    sendX(port, 'xsamples', Float32Array.of(5), 7);
    expect(port.messages.filter((m) => m.type === 'error')).toHaveLength(0);
    expect((processor as unknown as { ring: { availableFrames: number } }).ring.availableFrames).toBe(0);

    port.send({ type: 'xfade', sourceRate: 10, sourceChannels: 1, fadeFrames: 2, token: 8, generation: 1 });
    sendX(port, 'xsamples', Float32Array.of(5), 8);
    sendX(port, 'xend', null, 8);
    expect(
      port.messages.some((m) => m.type === 'xfadeReady' && m.token === 8),
    ).toBe(true);
  });

  it('absorbs late xsamples/xend into the normal path after the swap', () => {
    const { processor, port, go } = armFade(2);
    port.send({ type: 'samples', buffer: Float32Array.of(1, 1).buffer, generation: 1 });
    sendX(port, 'xsamples', Float32Array.of(4), 7);
    sendX(port, 'xend', null, 7);
    go();

    const first = render(processor, 3); // mixes 2 frames + swaps mid-callback
    // The lane's single 4-frame is consumed at fade-frame 0 where sin(0)=0,
    // so it is inaudible; frame 1 has only the fading-outgoing contribution.
    expect(first[0]).toBeCloseTo(1, 5);
    expect(first[1]).toBeCloseTo(Math.cos(Math.PI / 4), 5);
    expect(first[2]).toBe(0); // post-swap: rings drained

    // A straggler lane chunk (same token as the swap) flows through the
    // normal credit path; a WRONG-token chunk would be dropped.
    sendX(port, 'xsamples', Float32Array.of(6, 6), 7);
    const second = render(processor, 2);
    expect(Array.from(second)).toEqual([6, 6]);

    sendX(port, 'xsamples', Float32Array.of(9, 9), 99); // stale token
    const third = render(processor, 2);
    expect(Array.from(third)).toEqual([0, 0]);
  });

  it('defers the producer credit when a chunk only partially flushes', () => {
    const { processor, port } = armFade(2);
    // Send more lane audio than fits the lane's 80-frame ring: the chunk
    // must NOT be credited immediately (the old behavior double-credited
    // and corrupted the producer's accounting).
    sendX(port, 'xsamples', Float32Array.from({ length: 120 }, () => 1), 7);
    expect(
      port.messages.filter((m) => m.type === 'accepted' && m.lane === 2),
    ).toHaveLength(0);
    const lane = (
      processor as unknown as { xlane: { creditOwed: boolean; pending: unknown } | null }
    ).xlane;
    expect(lane?.creditOwed).toBe(true);
    expect(lane?.pending).not.toBeNull();
  });

  it('drops crossfade state on reset and config', () => {
    const { processor, port } = armFade(2);
    port.send({ type: 'reset', generation: 1 });
    sendX(port, 'xsamples', Float32Array.of(1), 7);
    expect(port.messages.filter((m) => m.type === 'accepted')).toHaveLength(0);

    port.send({ type: 'xfade', sourceRate: 10, sourceChannels: 1, fadeFrames: 2, token: 9, generation: 1 });
    port.send({ type: 'reset', generation: 2 });
    sendX(port, 'xsamples', Float32Array.of(1), 9);
    expect(port.messages.filter((m) => m.type === 'accepted')).toHaveLength(0);
    void processor;
  });
});
