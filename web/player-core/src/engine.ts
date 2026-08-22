// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

// The playback engine port (player-core M2).
//
// A host-implementable abstraction over "the thing that turns one or more
// queued sources into audible, seekable output". The web hosts implement it
// with AudioContext/AudioWorklet/WASM (musepack) and HTMLMediaElement
// (native codecs); future Android/iOS hosts implement it natively. The core
// orchestrator knows nothing about audio plumbing beyond this contract.
//
// Honesty rules:
// - Capabilities declare what an engine actually does. Nothing here claims
//   sample-accurate gapless for element-based playback.
// - DecodeGate is a capability, not part of Engine: priming requires
//   decoding while the output clock is still stopped (a suspended
//   AudioContext pulls no render callbacks), so folding pump control into
//   play()/pause() would deadlock priming on pull-style pipelines.
// - All positions are OUTPUT-rate samples within the currently open track.
//   Source-rate conversion stays engine-internal.

import type { PlaybackItem, StreamInfo } from './types';

export interface EngineCapabilities {
  /** prepareNext()/advance() standby support. */
  readonly preloadNext: boolean;
  /** True only when track handoff is sample-exact (Musepack pipeline). */
  readonly sampleAccurateGapless: boolean;
  /** DecodeGate control is meaningful (pull-style decode pipelines). */
  readonly decodeGate: boolean;
}

/** Events an engine reports back. Delivered asynchronously, in order,
 *  tagged with the engine instance (see plan §A.5 delivery rules). */
export type EngineEventName = 'primed' | 'buffering' | 'eos' | 'error' | 'tick';

export interface EngineEvents {
  /** Enough output is buffered to start/keep the clock running. */
  primed(): void;
  /** Output starved; playback cannot continue at full rate. */
  buffering(): void;
  /** The current source finished decoding/reached its end. */
  eos(): void;
  error(message: string): void;
  /** Coarse position change; may be coalesced by either side. */
  tick(): void;
}

export interface Engine {
  readonly capabilities: EngineCapabilities;

  init(authToken: string | null): Promise<void>;
  /** Opens a source; resolves once stream facts are known. */
  open(item: PlaybackItem): Promise<StreamInfo>;
  play(): Promise<void>;
  pause(): Promise<void>;
  /** Seeks within the OPEN track (output-rate samples). */
  seekSample(samples: number): Promise<void>;
  /** Combined linear gain (user volume × normalization). */
  setGain(linear: number): void;
  /** Output-rate frames rendered since the last open/seek reset. */
  renderedSamples(): number;
  close(): Promise<void>;

  on(name: EngineEventName, cb: (sender: Engine) => void): void;
}

/** Standby/preload capability: open the next source ahead of time and
 *  promote it at the boundary without re-opening the output graph. */
export interface PreloadEngine extends Engine {
  prepareNext(item: PlaybackItem): Promise<StreamInfo | null>;
  advance(): Promise<StreamInfo | null>;
}

/** Pull-decode gate capability (e.g. the WASM musepack pipeline). */
export interface DecodeGate {
  start(): void;
  stop(): void;
}

export function isPreloadEngine(e: Engine): e is PreloadEngine {
  return e.capabilities.preloadNext && 'prepareNext' in e && 'advance' in e;
}

export function decodeGateOf(e: Engine): DecodeGate | null {
  if (!e.capabilities.decodeGate) return null;
  const g = e as unknown as Partial<DecodeGate>;
  return typeof g.start === 'function' && typeof g.stop === 'function' ? (e as unknown as DecodeGate) : null;
}
