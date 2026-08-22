// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

// The unified playback controller: a single state machine over two backends
// (Musepack demand-driven WASM, and browser-native codecs), the queue, BS.1770
// normalization, and Media Session. The UI talks only to this controller.
import { writable, type Writable } from '../store';
import { MusepackEngine, type EngineStreamInfo } from './musepack-engine';
import { NativeBackend } from './native-backend';
import { combinedGain, normalizationGainDb, type NormalizationMode } from './loudness';
import { bindMediaActions, setMediaMetadata, setMediaPosition } from './media-session';
import type { QueueItem, QueueStore } from '../state/queue';
import type { Track } from '../api/types';

export type PlayerState =
  | 'idle'
  | 'loading'
  | 'buffering'
  | 'playing'
  | 'paused'
  | 'ended'
  | 'error';

/** End-of-album tolerance (samples). The worklet renders 128 frames per
 *  process call, so the final process can underrun with < 128 frames left in
 *  the ring; the rendered counter then stops a few samples short of
 *  totalLength. A `-2`-sample tolerance (as used originally) never fires and
 *  the player would sit in `playing` forever after the last track. */
const END_TOLERANCE_SAMPLES = 256;

export interface PlayerModel {
  state: PlayerState;
  current: QueueItem | null;
  positionSeconds: number;
  durationSeconds: number;
  /// Album-absolute start sample/time of the current track (0 when no
  /// current track). Used by the waveform seek control to map within-
  /// track position <-> album-absolute seconds.
  currentTrackStartSeconds: number;
  /// Per-track duration in seconds (lengthOf(idx)/rate), 0 when unknown.
  currentTrackDurationSeconds: number;
  volume: number;
  normalizeMode: NormalizationMode;
  normDb: number;
  error?: string;
}

export interface BackendEvents {
  onPrimed: () => void;
  onBuffering: () => void;
  onEos: () => void;
  onError: (message: string) => void;
  onPosition: () => void;
}

export interface Backend {
  readonly kind: 'musepack' | 'native';
  init(token: string | null): Promise<void>;
  open(url: string, size: number): Promise<EngineStreamInfo>;
  prepareNext(url: string, size: number): Promise<EngineStreamInfo | null>;
  advance(): Promise<EngineStreamInfo | null>;
  startPumping(): void;
  pausePumping(): void;
  play(): Promise<void>;
  pause(): Promise<void>;
  seek(sample: number): Promise<void>;
  setGain(linear: number): void;
  getRenderedSamples(): number;
  getInfo(): EngineStreamInfo | null;
  getServedBytes?(): number;
  readonly lengthSamples: number;
  readonly rate: number;
  close(): Promise<void>;
}

function createBackend(kind: 'musepack' | 'native', events: BackendEvents): Backend {
  if (kind === 'musepack') {
    return new MusepackEngine({
      onPrimed: events.onPrimed,
      onBuffering: events.onBuffering,
      onEos: events.onEos,
      onError: events.onError,
      onPosition: () => events.onPosition(),
    });
  }
  const native = new NativeBackend();
  native.onPrimed = events.onPrimed;
  native.onBuffering = events.onBuffering;
  native.onEos = events.onEos;
  native.onError = events.onError;
  native.onPosition = events.onPosition;
  return native;
}

export interface ControllerOptions {
  token?: () => string | null;
  initialVolume?: number;
  initialNormalize?: NormalizationMode;
  /** Test seam: replace backend construction. */
  backendFactory?: (kind: 'musepack' | 'native', events: BackendEvents) => Backend;
}

export class PlayerController {
  readonly model: Writable<PlayerModel>;
  private queue: QueueStore;
  private token: () => string | null;
  private backend: Backend | null = null;
  private backendKind: 'musepack' | 'native' | null = null;
  /** Exact decoded length per track id (samples at output rate). Keyed by
   *  id, not queue index, so entries survive loads and navigation: album-
   *  absolute bookkeeping must never fall back to unreliable manifest
   *  durations for tracks we have already decoded. */
  private lengths = new Map<number, number>();
  private resetOffset = 0;
  private pendingEnded = false;
  private loadingSeq = 0;
  private transportSeq = 0;
  private mutating = false;
  private pauseIntent = false;
  private backendFactory: ControllerOptions['backendFactory'];
  private unsub: (() => void) | null = null;

  constructor(queue: QueueStore, opts: ControllerOptions = {}) {
    this.queue = queue;
    this.token = opts.token ?? (() => null);
    this.backendFactory = opts.backendFactory;
    this.model = writable<PlayerModel>({
      state: 'idle',
      current: null,
      positionSeconds: 0,
      durationSeconds: 0,
      currentTrackStartSeconds: 0,
      currentTrackDurationSeconds: 0,
      volume: opts.initialVolume ?? 0.8,
      normalizeMode: opts.initialNormalize ?? 'album',
      normDb: 0,
    });
  }

  init(): void {
    bindMediaActions({
      play: () => void this.togglePlay(),
      pause: () => void this.pause(),
      next: () => void this.next(),
      previous: () => void this.previous(),
      // Media Session delivers TRACK-relative seek times (see tick());
      // convert to the album-absolute targets the transport speaks.
      seek: (trackSeconds) =>
        void this.seek(this.model.get().currentTrackStartSeconds + trackSeconds),
      seekBy: (d) => this.seek(this.model.get().positionSeconds + d),
    });
    // React to queue changes made OUTSIDE the controller (e.g. the queue
    // panel removing the current item, or clearing the queue). Mutations the
    // controller itself performs are flagged `mutating` and skipped here.
    this.unsub = this.queue.subscribe(() => {
      if (this.mutating) return;
      const m = this.model.get();
      const cur = this.queue.current();
      if (!cur && m.state !== 'idle' && m.state !== 'error') {
        void this.stop();
      } else if (cur && m.current && cur.track.id !== m.current.track.id && m.state !== 'loading') {
        void this._load();
      }
    });
  }

  destroy(): void {
    this.unsub?.();
    this.unsub = null;
    void this.backend?.close();
  }

  // ---- state helpers ------------------------------------------------------

  private setState(state: PlayerState): void {
    this.model.update((m) => ({ ...m, state }));
  }

  private rate(): number {
    return this.backend?.rate ?? 44100;
  }

  private lengthOf(i: number): number {
    const item = this.queue.at(i);
    if (!item) return 0;
    const exact = this.lengths.get(item.track.id);
    if (exact !== undefined) return exact;
    return Math.floor((item.track.duration ?? 0) * this.rate());
  }

  private offsets(): number[] {
    const items = this.queue.get().items;
    const offs: number[] = [];
    let acc = 0;
    for (let i = 0; i < items.length; i++) {
      offs[i] = acc;
      acc += this.lengthOf(i);
    }
    return offs;
  }

  private totalLength(): number {
    const offs = this.offsets();
    const n = offs.length;
    if (n === 0) return 0;
    return (offs[n - 1] ?? 0) + this.lengthOf(n - 1);
  }

  private albumPosition(): number {
    if (!this.backend) return 0;
    const rendered = this.backend.getRenderedSamples();
    if (this.backendKind === 'musepack') return this.resetOffset + rendered;
    const idx = this.queue.get().index;
    const base = idx >= 0 ? (this.offsets()[idx] ?? 0) : 0;
    return base + rendered;
  }

  // ---- queue actions -------------------------------------------------------

  async playItem(item: QueueItem): Promise<void> {
    this.pauseIntent = false;
    this.setState('loading');
    this.mutating = true;
    this.queue.playNow(item);
    this.mutating = false;
    await this._load();
  }

  /** Jumps to an existing queue item without touching the rest of the queue
   *  (the queue-panel click). Deliberately NOT playItem(): replacing the
   *  queue would throw away everything the user had lined up. */
  async playQueueIndex(i: number): Promise<void> {
    if (!this.queue.at(i) || i === this.queue.get().index) return;
    this.pauseIntent = false;
    this.setState('loading');
    this.mutating = true;
    this.queue.moveTo(i);
    this.mutating = false;
    await this._load();
  }

  async playAlbum(
    release: Parameters<QueueStore['playAlbum']>[0],
    title: string,
    artist: string,
    startIndex = 0,
  ): Promise<void> {
    this.pauseIntent = false;
    this.setState('loading');
    this.mutating = true;
    this.queue.playAlbum(release, title, artist, startIndex);
    this.mutating = false;
    await this._load();
  }

  async next(): Promise<void> {
    ++this.loadingSeq;
    this.mutating = true;
    const item = this.queue.next();
    this.mutating = false;
    if (item) await this._load();
  }

  async previous(): Promise<void> {
    const m = this.model.get();
    // "Previous" restarts the CURRENT song; only a second press (or pressing
    // it right after the song began) moves to the previous queue item.
    if (m.current && m.positionSeconds - m.currentTrackStartSeconds > 3) {
      await this.restartCurrentTrack();
      return;
    }
    ++this.loadingSeq;
    this.mutating = true;
    const item = this.queue.previous();
    this.mutating = false;
    if (item) await this._load();
  }

  /** Seeks to sample 0 of the CURRENT track. Deliberately not routed through
   *  seek(): an album-absolute target of 0 resolves to queue index 0, which
   *  would restart the whole queue instead of this song. */
  private async restartCurrentTrack(): Promise<void> {
    if (!this.backend || !this.model.get().current) return;
    ++this.transportSeq;
    const seq = ++this.loadingSeq;
    const base = this.offsets()[this.queue.get().index] ?? 0;
    this.resetOffset = base;
    this.setState(this.pauseIntent ? 'paused' : 'buffering');
    this.model.update((mm) => ({ ...mm, positionSeconds: base / this.rate() }));
    await this.backend.seek(0);
    if (seq !== this.loadingSeq) return;
    if (!this.pauseIntent) this.backend.startPumping();
  }

  // ---- load / playback -----------------------------------------------------

  private async _load(): Promise<boolean> {
    const item = this.queue.current();
    if (!item) return false;
    const seq = ++this.loadingSeq;
    ++this.transportSeq;
    this.pendingEnded = false;
    // Lengths are keyed by track id, so a replaced queue cannot leak
    // another album's entries (ids are unique per server track row).
    this.model.update((m) => ({ ...m, state: 'loading', error: undefined }));
    try {
      const kind = this.chooseBackend(item.track);
      await this.ensureBackend(kind);
      if (seq !== this.loadingSeq) return false;
      const info = await this.backend!.open(item.track.audio.url, item.track.audio.size);
      if (seq !== this.loadingSeq) return false;
      const qi = this.queue.get().index;
      this.lengths.set(item.track.id, info.lengthSamples);
      // Publish track geometry NOW: the seek control reads these fields, and
      // waiting for the first rendered tick leaves a window where its hidden
      // input has max=0 (clicks clamp to seek(0) = wrong song).
      const offs = this.offsets();
      this.model.update((m) => ({
        ...m,
        currentTrackStartSeconds: (offs[qi] ?? 0) / this.rate(),
        currentTrackDurationSeconds: this.lengthOf(qi) / this.rate(),
      }));
      // prepare the next track ahead of time (gapless)
      const next = this.queue.at(qi + 1);
      if (next) {
        const ni = await this.backend!.prepareNext(next.track.audio.url, next.track.audio.size);
        if (seq !== this.loadingSeq) return false;
        if (ni) this.lengths.set(next.track.id, ni.lengthSamples);
      }
      this.resetOffset = this.offsets()[qi] ?? 0;
      this.applyGain(item);
      this.setMediaFor(item);
      this.model.update((m) => ({
        ...m,
        current: item,
        state: this.pauseIntent ? 'paused' : 'buffering',
        positionSeconds: this.resetOffset / this.rate(),
        durationSeconds: this.totalLength() / this.rate(),
      }));
      await this.backend!.seek(0);
      if (seq !== this.loadingSeq) return false;
      if (!this.pauseIntent) this.backend!.startPumping();
      return true;
    } catch (e) {
      if (seq === this.loadingSeq) this.fail(e instanceof Error ? e.message : String(e));
      return false;
    }
  }

  private async onEos(): Promise<void> {
    if (!this.backend) return;
    const seq = this.loadingSeq;
    const info = await this.backend.advance();
    if (seq !== this.loadingSeq) return;
    this.mutating = true;
    const item = this.queue.next();
    this.mutating = false;
    if (!info || !item) {
      // Last track decoded; let the ring drain, then end.
      this.pendingEnded = true;
      this.backend.pausePumping();
      this.tick();
      return;
    }
    const qi = this.queue.get().index;
    this.lengths.set(item.track.id, info.lengthSamples);
    // Same as _load(): publish geometry synchronously so the seek control
    // never sees the previous track's start/duration after a handoff.
    const handoffOffs = this.offsets();
    this.model.update((m) => ({
      ...m,
      currentTrackStartSeconds: (handoffOffs[qi] ?? 0) / this.rate(),
      currentTrackDurationSeconds: this.lengthOf(qi) / this.rate(),
    }));
    this.applyGain(item);
    this.setMediaFor(item);
    const next = this.queue.at(qi + 1);
    if (next) {
      const ni = await this.backend.prepareNext(next.track.audio.url, next.track.audio.size);
      if (seq !== this.loadingSeq) return;
      if (ni) this.lengths.set(next.track.id, ni.lengthSamples);
    }
    this.model.update((m) => ({
      ...m,
      current: item,
      state: this.pauseIntent || m.state === 'paused' ? 'paused' : 'buffering',
      durationSeconds: this.totalLength() / this.rate(),
    }));
    if (!this.pauseIntent) {
      const backend = this.backend;
      const transportSeq = this.transportSeq;
      backend.startPumping();
      try {
        await backend.play();
        if (
          seq === this.loadingSeq &&
          transportSeq === this.transportSeq &&
          !this.pauseIntent &&
          this.backend === backend
        ) {
          this.setState('playing');
        }
      } catch {
        if (
          seq === this.loadingSeq &&
          transportSeq === this.transportSeq &&
          this.backend === backend
        ) {
          this.setState('paused');
        }
      }
    }
  }

  private onPrimed(): void {
    const m = this.model.get();
    if (m.state === 'loading' || m.state === 'buffering') {
      const seq = this.loadingSeq;
      const transportSeq = this.transportSeq;
      const backend = this.backend;
      backend
        ?.play()
        .then(() => {
          const state = this.model.get().state;
          if (
            seq === this.loadingSeq &&
            transportSeq === this.transportSeq &&
            !this.pauseIntent &&
            this.backend === backend &&
            (state === 'loading' || state === 'buffering')
          ) {
            this.setState('playing');
          }
        })
        .catch(() => undefined);
    }
  }

  private onBuffering(): void {
    const state = this.model.get().state;
    if (state === 'loading' || state === 'buffering' || state === 'playing') {
      this.setState('buffering');
    }
  }

  private tick(): void {
    if (!this.backend) return;
    const pos = this.albumPosition();
    const dur = this.totalLength();
    const qi = this.queue.get().index;
    const idx = this.currentIndexAt(pos);
    // onEos() owns forward advancement. Only a LATER index may be adopted
    // here (never a regression): the worklet ring renders ahead, so at a
    // gapless handoff the rendered position can still describe the previous
    // track for a moment while the cursor has already advanced.
    if (idx > qi && !this.pendingEnded) {
      this.mutating = true;
      this.queue.moveTo(idx);
      this.mutating = false;
    }
    const offs = this.offsets();
    const trackStart = offs[idx] ?? 0;
    const trackDur = this.lengthOf(idx);
    const rate = this.rate();
    const ended = this.pendingEnded && pos >= dur - END_TOLERANCE_SAMPLES;
    this.model.update((m) => ({
      ...m,
      positionSeconds: pos / rate,
      durationSeconds: dur / rate,
      currentTrackStartSeconds: trackStart / rate,
      currentTrackDurationSeconds: trackDur / rate,
      state: ended ? 'ended' : m.state,
    }));
    // Media Session position state is TRACK-relative per spec: the OS
    // scrubber shows/requests positions inside the current song, not the
    // album. tick() therefore reports within-track values and converts
    // incoming seekto times back to album-absolute in init().
    setMediaPosition(trackDur / rate, Math.max(0, pos - trackStart) / rate);
    if (ended) {
      this.pendingEnded = false;
      this.backend.pause();
      this.backend.pausePumping();
    }
  }

  private currentIndexAt(pos: number): number {
    const offs = this.offsets();
    let idx = 0;
    for (let i = 0; i < offs.length; i++) {
      if (pos >= (offs[i] ?? 0)) idx = i;
    }
    return idx;
  }

  // ---- transport ------------------------------------------------------------

  async togglePlay(): Promise<void> {
    const m = this.model.get();
    if (m.state === 'playing') {
      await this.pause();
    } else if (m.state === 'paused') {
      await this.resume();
    } else if (m.state === 'ended' || m.state === 'idle' || m.state === 'error') {
      if (m.current) {
        this.pauseIntent = false;
        await this._load();
      }
    } else {
      await this.resume();
    }
  }

  async pause(): Promise<void> {
    ++this.transportSeq;
    this.pauseIntent = true;
    this.setState('paused');
    this.backend?.pausePumping();
    await this.backend?.pause();
  }

  async resume(): Promise<void> {
    if (!this.backend) return;
    const seq = ++this.transportSeq;
    const backend = this.backend;
    this.pauseIntent = false;
    backend.startPumping();
    await backend.play();
    if (seq !== this.transportSeq || this.pauseIntent || this.backend !== backend) return;
    this.setState('playing');
  }

  async seek(seconds: number): Promise<void> {
    if (!this.backend || !this.model.get().current) return;
    ++this.transportSeq;
    let seq = ++this.loadingSeq;
    const posSamples = Math.max(0, Math.floor(seconds * this.rate()));
    const items = this.queue.get().items;
    if (items.length === 0) return;
    // Resolve the album-absolute target to a (track, within-track offset) and
    // seek there directly; a target in another track must switch to it rather
    // than clamping inside the current track.
    const offs = this.offsets();
    let qi = 0;
    for (let i = 0; i < items.length; i++) {
      if (posSamples >= (offs[i] ?? 0)) qi = i;
    }
    const base = offs[qi] ?? 0;
    const trackLen = Math.max(0, this.lengthOf(qi) - 1);
    const within = Math.min(Math.max(0, posSamples - base), trackLen);

    if (qi !== this.queue.get().index) {
      this.mutating = true;
      this.queue.moveTo(qi);
      this.mutating = false;
      if (!(await this._load())) return;
      seq = this.loadingSeq;
      // _load() opened the target track, seeked to 0 and started pumping;
      // now move inside it (and re-align the offset bookkeeping) so the
      // reported position matches where the audio actually decodes from.
      if (qi === this.queue.get().index) {
        this.resetOffset = base + within;
        this.setState(this.pauseIntent ? 'paused' : 'buffering');
        this.model.update((m) => ({ ...m, positionSeconds: this.resetOffset / this.rate() }));
        await this.backend!.seek(within);
        if (seq !== this.loadingSeq) return;
        if (!this.pauseIntent) this.backend!.startPumping();
      }
      return;
    }

    this.resetOffset = base + within;
    this.setState(this.pauseIntent ? 'paused' : 'buffering');
    this.model.update((m) => ({ ...m, positionSeconds: this.resetOffset / this.rate() }));
    await this.backend.seek(within);
    if (seq !== this.loadingSeq) return;
    if (!this.pauseIntent) this.backend.startPumping();
  }

  async stop(): Promise<void> {
    const seq = ++this.loadingSeq;
    ++this.transportSeq;
    const backend = this.backend;
    this.pauseIntent = true;
    backend?.pausePumping();
    this.pendingEnded = false;
    this.model.update((m) => ({ ...m, state: 'idle', positionSeconds: 0 }));
    await backend?.pause();
    if (seq !== this.loadingSeq || this.backend !== backend) return;
    await backend?.seek(0);
  }

  /** Stops playback and disposes the backend + Media Session state. Used on
   *  sign-out / session expiry so audio never leaks across auth boundaries.
   *  The controller stays usable: the next play rebuilds its backend. */
  async teardown(): Promise<void> {
    ++this.loadingSeq;
    ++this.transportSeq;
    this.pauseIntent = false;
    this.backend?.pausePumping();
    this.pendingEnded = false;
    this.lengths.clear();
    this.resetOffset = 0;
    await this.backend?.close();
    this.backend = null;
    this.backendKind = null;
    setMediaMetadata(null);
    this.model.update((m) => ({
      ...m,
      state: 'idle',
      current: null,
      positionSeconds: 0,
      durationSeconds: 0,
      error: undefined,
    }));
  }

  // ---- settings --------------------------------------------------------------

  setVolume(v: number): void {
    const vol = Math.max(0, Math.min(1, v));
    this.model.update((m) => ({ ...m, volume: vol }));
    this.applyGain(this.model.get().current);
  }

  setNormalizeMode(mode: NormalizationMode): void {
    this.model.update((m) => ({ ...m, normalizeMode: mode }));
    this.applyGain(this.model.get().current);
  }

  private applyGain(item: QueueItem | null): void {
    const m = this.model.get();
    const normDb = normalizationGainDb(m.normalizeMode, item?.track.loudness, item?.albumLoudness);
    const gain = combinedGain(m.volume, normDb);
    this.backend?.setGain(gain);
    this.model.update((mm) => ({ ...mm, normDb }));
  }

  // ---- backend selection -----------------------------------------------------

  private chooseBackend(track: Track): 'musepack' | 'native' {
    const codec = track.codec?.codec ?? '';
    if (codec === 'musepack' || codec === 'musepack-sv7' || codec === 'musepack-sv8') {
      return 'musepack';
    }
    const mime = track.codec?.mimeType ?? '';
    if (mime && typeof document !== 'undefined') {
      const probe = document.createElement('audio');
      if (probe.canPlayType(mime)) return 'native';
    }
    throw new Error('This format is not supported by this browser.');
  }

  private async ensureBackend(kind: 'musepack' | 'native'): Promise<void> {
    if (this.backend && this.backendKind === kind) return;
    await this.backend?.close();
    this.backend = null;
    this.backendKind = kind;
    const events: BackendEvents = {
      onPrimed: () => this.onPrimed(),
      onBuffering: () => this.onBuffering(),
      onEos: () => void this.onEos(),
      onError: (msg) => this.fail(msg),
      onPosition: () => this.tick(),
    };
    this.backend = this.backendFactory
      ? this.backendFactory(kind, events)
      : createBackend(kind, events);
    await this.backend.init(this.token());
  }

  private setMediaFor(item: QueueItem): void {
    setMediaMetadata(item, item.artworkUrl);
  }

  private fail(msg: string): void {
    this.backend?.pausePumping();
    this.model.update((m) => ({ ...m, state: 'error', error: msg }));
  }

  getBackendKind(): 'musepack' | 'native' | null {
    return this.backendKind;
  }

  /** Compressed bytes fetched by the demand reader (dev/perf instrumentation). */
  getServedBytes(): number {
    if (this.backend && this.backendKind === 'musepack') {
      return this.backend.getServedBytes?.() ?? 0;
    }
    return 0;
  }
}
