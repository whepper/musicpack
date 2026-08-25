// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

// Web composition facade (M4). All transport/queue/position/persistence
// semantics live in the platform-independent Player (player-core/src/
// player.ts); this file only wires web adapters:
//
//   localStorage            → StoragePort (same key + throttle behavior)
//   navigator.mediaSession  → MediaControlsPort (metadata + track-relative
//                             position; incoming seekto converted by the core)
//   MusepackEngine | NativeBackend → Engine port via resolveKind
//
// The public surface is identical to the pre-M4 PlayerController so the
// Svelte components, bootstrap, e2e debug hooks and unit tests are unchanged.

import { writable } from '../store';
import { Player } from '../../../../player-core/src/player';
import type {
  MediaControlsPort,
  PlayerPorts,
  PlayerState as CorePlayerState,
  StoragePort,
} from '../../../../player-core/src/player';
import type { QueueModel } from '../../../../player-core/src/queue';
import type { Engine } from '../../../../player-core/src/engine';
import type { PlaybackItem } from '../../../../player-core/src/types';
import type { NormalizationMode } from './loudness';
import { bindMediaActions, setMediaMetadata, setMediaPosition } from './media-session';
import { browserSupportsMime, isMusepackCodec } from './capability';
import { MusepackEngine } from './musepack-engine';
import { NativeBackend } from './native-backend';
import type { QueueItem, QueueStore, SelectionContext } from '../state/queue';
import type { PlayerEvent } from '../../../../player-core/src/events';

export interface PlayerModel {
  state: CorePlayerState;
  current: QueueItem | null;
  positionSeconds: number;
  durationSeconds: number;
  /** Album-absolute start time of the current track. */
  currentTrackStartSeconds: number;
  /** Per-track duration in seconds, 0 when unknown. */
  currentTrackDurationSeconds: number;
  volume: number;
  normalizeMode: NormalizationMode;
  normDb: number;
  /** Queue playback policy (mirrored from the core model). */
  repeat: 'off' | 'one' | 'all';
  shuffle: boolean;
  /** Crossfade seconds at natural boundaries; 0 = off. */
  crossfadeSeconds: number;
  error?: string;
}
export type { CorePlayerState as PlayerState };

const STORAGE_KEY = 'musicpack.player.v1';

function localStoragePort(): StoragePort {
  return {
    get: () => (typeof localStorage === 'undefined' ? null : localStorage.getItem(STORAGE_KEY)),
    set: (value) => {
      if (typeof localStorage === 'undefined') return;
      if (value === null) localStorage.removeItem(STORAGE_KEY);
      else localStorage.setItem(STORAGE_KEY, value);
    },
  };
}

const mediaControls: MediaControlsPort = {
  bind(handlers) {
    bindMediaActions({
      play: handlers.play,
      pause: handlers.pause,
      next: handlers.next,
      previous: handlers.previous,
      seek: handlers.seek,
      seekBy: handlers.seekBy,
    });
  },
  setMetadata(item) {
    const qi = item as QueueItem | null;
    setMediaMetadata(qi, qi?.artworkUrl);
  },
  setPosition(durationSeconds, positionSeconds) {
    setMediaPosition(durationSeconds, positionSeconds);
  },
};

/** Backend selection (web policy). Codec string → musepack; else a browser
 *  capability probe → native; else a friendly failure. The capability rules
 *  live in ./capability so representation selection shares one truth. */
function chooseBackend(item: PlaybackItem): 'musepack' | 'native' {
  if (isMusepackCodec(item.codec)) return 'musepack';
  if (browserSupportsMime(item.mimeType)) return 'native';
  throw new Error('This format is not supported by this browser.');
}

export interface HandlerSet {
  primed(): void;
  buffering(): void;
  eos(): void;
  error(message: string): void;
  tick(): void;
}

function createWebEngine(kind: 'musepack' | 'native', h: HandlerSet): Engine {
  if (kind === 'musepack') {
    return new MusepackEngine({
      primed: h.primed,
      buffering: h.buffering,
      eos: h.eos,
      error: h.error,
      tick: () => h.tick(),
    });
  }
  const native = new NativeBackend();
  native.onPrimed = h.primed;
  native.onBuffering = h.buffering;
  native.onEos = h.eos;
  native.onError = h.error;
  native.onPosition = h.tick;
  return native;
}

export interface ControllerOptions {
  token?: () => string | null;
  initialVolume?: number;
  initialNormalize?: NormalizationMode;
  /** Test seam: replace engine construction. */
  backendFactory?: (kind: 'musepack' | 'native', events: HandlerSet) => Engine;
  /** Storage for cross-reload player persistence (defaults to localStorage). */
  storage?: { get: () => string | null; set: (value: string | null) => void };
  /** Content-aware transition policy (Sweet Fades); see transition-profiles. */
  planTransition?: PlayerPorts['planTransition'];
  /** Representation-selection context provider (Phase 4): consulted at item
   *  construction time so PlaybackItems carry the preferred audio source.
   *  Omitted ⇒ default-only behavior. */
  selection?: () => SelectionContext;
}

interface InternalControllerOptions extends ControllerOptions {
  /** Test seam (unit tests): inject the whole ports object. When present it
   *  overrides engineFactory/storage below. */
  portsOverride?: Partial<PlayerPorts>;
}

export class PlayerController {
  readonly model = writable<PlayerModel>({
    state: 'idle',
    current: null,
    positionSeconds: 0,
    durationSeconds: 0,
    currentTrackStartSeconds: 0,
    currentTrackDurationSeconds: 0,
    volume: 0.8,
    normalizeMode: 'album',
    normDb: 0,
    repeat: 'off',
    shuffle: false,
    crossfadeSeconds: 0,
  });
  private core: Player;
  private queue: QueueStore;
  private readonly selection?: () => SelectionContext;

  constructor(queue: QueueStore, opts: ControllerOptions = {}) {
    this.queue = queue;
    this.selection = opts.selection;
    const o = opts as InternalControllerOptions;
    const ports: PlayerPorts = {
      engineFactory: (kind, handlers) =>
        o.portsOverride?.engineFactory
          ? o.portsOverride.engineFactory(kind, handlers)
          : opts.backendFactory
            ? opts.backendFactory(kind, handlers)
            : createWebEngine(kind, handlers),
      resolveKind: chooseBackend,
      token: opts.token ?? (() => null),
      storage:
        (opts.storage as StoragePort | undefined) ??
        o.portsOverride?.storage ??
        localStoragePort(),
      mediaControls,
      // Host-owned clock + timer (core purity law 2). Looked up dynamically
      // so fake timers in tests intercept these as intended.
      now: () => Date.now(),
      schedulePersist: (fn, ms) => setTimeout(fn, ms),
      clearPersistTimer: (h) => clearTimeout(h as ReturnType<typeof setTimeout>),
    };
    this.core = new Player(
      queue as unknown as QueueModel,
      o.portsOverride
        ? { ...ports, ...o.portsOverride }
        : { ...ports, planTransition: opts.planTransition },
      { initialVolume: opts.initialVolume, initialNormalize: opts.initialNormalize },
    );    // Mirror the core model into the UI-facing writable with QueueItem-typed
    // `current` (the web queue only ever holds QueueItems). The model stays
    // the full state snapshot; M7 events are exposed alongside for event-
    // driven consumers (media controls, future scrobbling/analytics).
    this.core.model.subscribe((m) => this.model.set(m as PlayerModel));
  }

  /** Subscribe to typed player events (M7 integration surface). Returns an
   *  unsubscribe function. Events: state / track / position / policy /
   *  gain / error — see player-core/src/events.ts. */
  on(fn: (event: PlayerEvent) => void): () => void {
    return this.core.on(fn);
  }

  init(): void {
    this.core.init();
  }

  destroy(): void {
    this.core.destroy();
  }

  // ---- queue actions --------------------------------------------------------

  async playItem(item: QueueItem): Promise<void> {
    await this.core.playItem(item);
  }

  /** Jumps to an existing queue item without touching the rest of the queue
   *  (the queue-panel click). Deliberately NOT playItem(): replacing the
   *  queue would throw away everything the user had lined up. */
  async playQueueIndex(i: number): Promise<void> {
    await this.core.playQueueIndex(i);
  }

  async playAlbum(
    release: Parameters<QueueStore['playAlbum']>[0],
    title: string,
    artist: string,
    startIndex = 0,
  ): Promise<void> {
    // Historical flow preserved verbatim: the CONTROLLER performs the queue
    // mutation (flagged internal), then loads.
    await this.core.playSequence(this.queueItemsFor(release, title, artist), title, artist, startIndex);
  }

  /** The core Player mutates the generic QueueModel; playAlbum needs the
   *  built items BEFORE handing them over, so build + install here through
   *  the store's builder while still routing the install through the core. */
  private queueItemsFor(
    release: Parameters<QueueStore['playAlbum']>[0],
    title: string,
    artist: string,
  ): QueueItem[] {
    // Build items without installing: reuse the store's public builder by
    // calling playAlbum on a THROWAWAY probe? No — simpler: the store keeps
    // its builder; expose it as a pure function instead. See state/queue.ts
    // `itemsForRelease` export used here. Representation selection (Phase 4)
    // rides along through the injected context provider.
    return itemsForRelease(release, title, artist, this.selection?.());
  }

  async next(): Promise<void> {
    await this.core.next();
  }

  async previous(): Promise<void> {
    await this.core.previous();
  }

  // ---- transport -------------------------------------------------------------

  async togglePlay(): Promise<void> {
    await this.core.togglePlay();
  }

  async pause(): Promise<void> {
    await this.core.pause();
  }

  async resume(): Promise<void> {
    await this.core.resume();
  }

  async seek(seconds: number): Promise<void> {
    await this.core.seek(seconds);
  }

  async stop(): Promise<void> {
    await this.core.stop();
  }

  /** Stops playback and disposes the backend + Media Session state. Used on
   *  sign-out / session expiry so audio never leaks across auth boundaries.
   *  The controller stays usable: the next play rebuilds its backend. */
  async teardown(): Promise<void> {
    await this.core.teardown();
  }

  // ---- settings ---------------------------------------------------------------

  setVolume(v: number): void {
    this.core.setVolume(v);
  }

  setNormalizeMode(mode: NormalizationMode): void {
    this.core.setNormalizeMode(mode);
  }

  setRepeat(mode: 'off' | 'one' | 'all'): void {
    this.core.setRepeat(mode);
  }

  setShuffle(on: boolean): void {
    this.core.setShuffle(on);
  }

  setCrossfade(seconds: number): void {
    this.core.setCrossfade(seconds);
  }

  getBackendKind(): 'musepack' | 'native' | null {
    return this.core.getEngineKind();
  }

  /** Compressed bytes fetched by the demand reader (dev/perf instrumentation). */
  getServedBytes(): number {
    return this.core.getServedBytes();
  }
}

// Imported late to avoid a cycle at module-eval time (state/queue imports
// nothing from playback).
import { itemsForRelease } from '../state/queue';
