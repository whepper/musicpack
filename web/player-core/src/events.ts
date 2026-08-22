// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

// Player events (player-core M7).
//
// The core Player emits a small typed event union alongside its model
// store. Events are the INTEGRATION surface: UI projections, media-control
// adapters, persistence hooks and future consumers (scrobbling, analytics)
// subscribe here instead of polling the model. The model remains the full
// state snapshot; events describe what CHANGED.
//
// Delivery rules (plan §A.5): ordered, synchronous with the state change,
// never dropped (except position which may be coalesced by subscribers).

import type { PlaybackItem } from './types';
import type { PlayerState } from './player';

export type PlayerEvent =
  | { t: 'state'; state: PlayerState }
  | { t: 'track'; item: PlaybackItem | null }
  | { t: 'position'; positionSeconds: number; trackStartSeconds: number; trackDurationSeconds: number }
  | { t: 'policy'; repeat: 'off' | 'one' | 'all'; shuffle: boolean }
  | { t: 'gain'; normDb: number }
  | { t: 'error'; message: string };

export type PlayerListener = (event: PlayerEvent) => void;

/** Minimal fan-out list. No event bus, no async queue: listeners run
 *  synchronously in emission order on the calling thread (purity law 5). */
export class PlayerEventSink {
  private listeners = new Set<PlayerListener>();

  subscribe(fn: PlayerListener): () => void {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  emit(event: PlayerEvent): void {
    for (const fn of [...this.listeners]) fn(event);
  }
}
