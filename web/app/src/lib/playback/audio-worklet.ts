// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

// MusicPack PCM AudioWorkletProcessor (Phase 6).
//
// Consumes already-decoded interleaved PCM delivered as transferred
// Float32Array messages, buffers it in a bounded ring, and renders it to the
// audio output. Decoding and network never happen here. The ring fill is
// reported through watermarks so the main thread applies backpressure to the
// decoder (pause/resume) and never overflows the buffer.
//
// Bundled as a standalone entry by Vite (imported via `?url`); the ring
// buffer module is inlined at build time.
import { RingBuffer } from './ring-buffer';
import { HIGH_WATER, LOW_WATER, PRIME_FRACTION, RING_SECONDS } from './worklet-protocol';

interface PendingPcm {
  data: Float32Array;
  frameOffset: number;
}

export class MusicPackPcmProcessor extends AudioWorkletProcessor {
  private ring: RingBuffer | null = null;
  private rate = 44100;
  private channels = 2;
  private lastRenderedReport = -1;
  private primed = false;
  private lastFull = false;
  private lastLow = true;
  private underrunning = false;
  private pending: PendingPcm | null = null;
  private generation = 0;

  constructor() {
    super();
    this.port.onmessage = (ev: MessageEvent) => {
      const msg = ev.data;
      switch (msg.type) {
        case 'config': {
          if (msg.generation < this.generation) break;
          this.generation = msg.generation;
          this.rate = msg.rate;
          this.channels = msg.channels;
          this.ring = new RingBuffer(Math.round(this.rate * RING_SECONDS), this.channels);
          this.primed = false;
          this.lastFull = false;
          this.lastLow = true;
          this.underrunning = false;
          this.pending = null;
          this.lastRenderedReport = -1;
          break;
        }
        case 'reset': {
          if (msg.generation < this.generation) break;
          this.generation = msg.generation;
          this.ring?.reset();
          this.primed = false;
          this.lastFull = false;
          this.lastLow = true;
          this.underrunning = false;
          this.pending = null;
          this.lastRenderedReport = -1;
          break;
        }
        case 'samples': {
          if (!this.ring || msg.generation !== this.generation) break;
          if (this.pending) {
            this.port.postMessage({
              type: 'error',
              frames: this.ring.renderedFrames,
              generation: this.generation,
              message: 'received PCM without an available producer credit',
            });
            break;
          }
          const data = new Float32Array(msg.buffer);
          this.pending = { data, frameOffset: 0 };
          const accepted = this.flushPending();
          this.reportLevel();
          if (accepted) this.reportAccepted();
          break;
        }
      }
    };
  }

  private flushPending(): boolean {
    const ring = this.ring;
    const chunk = this.pending;
    if (!ring || !chunk) return false;
    while (ring.freeFrames > 0) {
      const totalFrames = Math.floor(chunk.data.length / this.channels);
      if (chunk.frameOffset >= totalFrames) {
        this.pending = null;
        return true;
      }
      const samples = chunk.data.subarray(chunk.frameOffset * this.channels);
      const written = ring.writeInterleaved(samples);
      chunk.frameOffset += written;
      if (written === 0) break;
    }
    const complete = chunk.frameOffset >= Math.floor(chunk.data.length / this.channels);
    if (complete) this.pending = null;
    return complete;
  }

  private reportAccepted(): void {
    if (!this.ring) return;
    this.port.postMessage({
      type: 'accepted',
      frames: this.ring.renderedFrames,
      generation: this.generation,
      available: this.ring.availableFrames,
    });
  }

  private reportLevel(): void {
    if (!this.ring) return;
    const fill = this.ring.availableFrames / this.ring.capacity;
    if (fill >= PRIME_FRACTION && !this.primed) {
      this.primed = true;
      this.port.postMessage({
        type: 'primed',
        frames: this.ring.renderedFrames,
        generation: this.generation,
      });
    }
    if (fill >= HIGH_WATER && !this.lastFull) {
      this.lastFull = true;
      this.port.postMessage({
        type: 'full',
        frames: this.ring.renderedFrames,
        generation: this.generation,
        available: this.ring.availableFrames,
      });
    } else if (fill < HIGH_WATER) {
      this.lastFull = false;
    }
    if (fill < LOW_WATER && !this.lastLow) {
      this.lastLow = true;
      this.port.postMessage({
        type: 'need',
        frames: this.ring.renderedFrames,
        generation: this.generation,
        available: this.ring.availableFrames,
      });
    } else if (fill >= LOW_WATER) {
      this.lastLow = false;
    }
  }

  private reportRendered(): void {
    if (!this.ring) return;
    const t = currentTime;
    if (this.lastRenderedReport < 0 || t - this.lastRenderedReport >= 0.2) {
      this.lastRenderedReport = t;
      this.port.postMessage({
        type: 'rendered',
        frames: this.ring.renderedFrames,
        generation: this.generation,
      });
    }
  }

  override process(inputs: Float32Array[][], outputs: Float32Array[][]): boolean {
    const output = outputs[0];
    if (!output || output.length === 0) return true;
    const frames = output[0]?.length ?? 0;
    const ring = this.ring;

    if (!ring) {
      for (const ch of output) ch.fill(0);
      return true;
    }

    const available = ring.availableFrames;
    if (available < frames) {
      for (const ch of output) ch.fill(0);
      ring.readPlanar(output, frames);
      if (!this.underrunning) {
        this.underrunning = true;
        this.primed = false;
        this.port.postMessage({
          type: 'underrun',
          frames: ring.renderedFrames,
          generation: this.generation,
          available,
        });
      }
    } else {
      ring.readPlanar(output, frames);
      this.underrunning = false;
    }
    const accepted = this.flushPending();
    this.reportLevel();
    if (accepted) this.reportAccepted();
    this.reportRendered();
    return true;
  }
}

registerProcessor('musicpack-pcm', MusicPackPcmProcessor);
