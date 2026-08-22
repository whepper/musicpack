// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

// Platform-independent playback types (player-core M1).
//
// Purity laws (see web/player-core/README.md): this module must stay free of
// DOM / Svelte / Node / worker imports and ambient globals, and every type in
// a port signature must be JSON-representable. `PlaybackItem` is the
// platform-independent queue entry; the web `QueueItem` is its web-shaped
// alias (api/types.ts Track verbatim) until the orchestrator extraction (M4)
// moves the UI projection fully onto core events.

/** Where an engine obtains the bytes of one item. `http-range` is the
 *  demand-driven Musepack source (URL + total size); `stream` covers
 *  element/native playback where size is unknown or irrelevant. */
export interface PlaybackSource {
  kind: 'http-range' | 'stream';
  url: string;
  byteSize?: number;
}

export interface PlaybackItem {
  /** Stable per queue entry; the identity the player commands and events
   *  use (queue indices are presentation, this is identity). */
  id: string;
  /** Server track row identity. Key of the exact-decoded-lengths cache. */
  trackId: number;
  source: PlaybackSource;
  /** Manifest duration hint in seconds; exact decoded length overrides it. */
  durationHintSeconds?: number;
  /** Display + integration metadata (Media Session / notifications). */
  title: string;
  artist: string;
  albumTitle: string;
  edition?: string;
  artworkUrl?: string;
  /** BS.1770 loudness for normalization policy (see gain.ts). */
  loudness?: { lufs: number; truePeakDb: number };
  albumLoudness?: { albumLufs: number; albumTruePeakDb: number };
  /** Codec hint for backend resolution, e.g. 'musepack-sv8' | 'flac'. */
  codec?: string;
  /** MIME type hint for browser-native capability probing. */
  mimeType?: string;
}

/** Stream facts an engine reports when a source is opened/advanced to.
 *  `rate`/`lengthSamples` are in the engine's OUTPUT timeline (the web
 *  MusepackEngine normalizes source rate -> AudioContext rate). */
export interface StreamInfo {
  rate: number;
  channels: number;
  /** Codec stream version (Musepack SV7 = 7, SV8 = 8; 0 when unknown). */
  version: number;
  lengthSamples: number;
}
