// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

// Message contract between the playback controller (main thread) and the
// MusicPack PCM AudioWorklet processor.

export const RING_SECONDS = 8;       // ring capacity
export const HIGH_WATER = 0.8;       // >= this fraction filled -> pause decode
export const LOW_WATER = 0.2;        // < this fraction -> resume decode
export const PRIME_FRACTION = 0.2;   // enough buffered before resuming the clock

export interface WorkletConfig {
  type: 'config';
  sourceRate: number;
  sourceChannels: number;
  outputRate: number;
  outputChannels: number;
  generation: number;
}

export interface WorkletTrack {
  type: 'track';
  sourceRate: number;
  sourceChannels: number;
  generation: number;
}

export interface WorkletSamples {
  type: 'samples';
  buffer: ArrayBuffer; // interleaved Float32Array, transferred
  generation: number;
}

export interface WorkletReset {
  type: 'reset';
  generation: number;
}

export interface WorkletEnd {
  type: 'end';
  generation: number;
}

// --- Crossfade lane (M8 Phase B). The lane carries the INCOMING track of a
// crossfade; the regular messages keep feeding the OUTGOING one. Mixing and
// the final ring swap happen inside the worklet. `token` disambiguates
// overlapping attempts after an aborted fade.

export interface WorkletXfade {
  type: 'xfade';
  sourceRate: number;
  sourceChannels: number;
  /** Length of the equal-power overlap in output-rate frames. */
  fadeFrames: number;
  token: number;
  generation: number;
}

export interface WorkletXsamples {
  type: 'xsamples';
  buffer: ArrayBuffer; // interleaved Float32Array, transferred
  /** Must match the armed/last-swapped lane; anything else is dropped. */
  token: number;
  generation: number;
}

export interface WorkletXend {
  type: 'xend';
  token: number;
  generation: number;
}

export interface WorkletXgo {
  type: 'xfade-go';
  token: number;
  generation: number;
}

export interface WorkletXcancel {
  type: 'xfade-cancel';
  generation: number;
}

// worklet -> main
export interface WorkletReport {
  type:
    | 'rendered'
    | 'primed'
    | 'need'
    | 'full'
    | 'underrun'
    | 'accepted'
    | 'trackEnded'
    | 'xfadeReady'
    | 'xfaded'
    | 'error';
  frames: number; // cumulative AudioContext-rate frames since reset
  generation: number;
  available?: number;
  message?: string;
  /** Which decode lane an `accepted` credit belongs to (default 1). */
  lane?: number;
  /** Echoed from the initiating `xfade` message; stale tokens are ignored. */
  token?: number;
  /** `xfaded`: frames consumed from the outgoing/incoming lane at the swap. */
  outgoingFrames?: number;
  incomingFrames?: number;
}
