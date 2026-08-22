// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

// Native-codec backend: plays browser-supported formats (FLAC, Vorbis, WAV,
// ...) through HTMLMediaElement routed into the shared gain pipeline.
//
// Gapless: the next track is preloaded into a second element ahead of time
// and swapped in on 'ended'. Browsers do not expose a sample-perfect gapless
// API for <audio>, so the boundary may carry a small gap — unlike the
// Musepack backend, which is exact. This is documented in web/README.md.
import type { EngineStreamInfo } from './musepack-engine';
import type {
  Engine,
  EngineCapabilities,
  EngineEventName,
} from '../../../../player-core/src/engine';
import type { PlaybackItem } from '../../../../player-core/src/types';

const NATIVE_CAPABILITIES: EngineCapabilities = {
  preloadNext: true,
  // Browsers expose no sample-perfect gapless API for <audio>: the standby
  // element swap may carry a small boundary gap. Declared, not hidden.
  sampleAccurateGapless: false,
  decodeGate: false,
};

interface ElementSlot {
  el: HTMLAudioElement;
  src: MediaElementAudioSourceNode;
  url: string;
}

export class NativeBackend {
  readonly kind = 'native' as const;
  readonly capabilities = NATIVE_CAPABILITIES;
  private ctx: AudioContext | null = null;
  private gain: GainNode | null = null;
  private current: ElementSlot | null = null;
  private standby: ElementSlot | null = null;
  rate = 44100;
  private channels = 2;
  private token: string | null = null;
  private playRequest = 0;
  private shouldPlay = false;

  // events wired by the controller
  onEos: (() => void) | null = null;
  onError: ((msg: string) => void) | null = null;
  onPosition: (() => void) | null = null;
  onPrimed: (() => void) | null = null;
  onBuffering: (() => void) | null = null;

  /** Port-facing event registration (M2). Listeners fire IN ADDITION to the
   *  legacy assignable fields (which the web controller still uses until
   *  M4); either side may be absent. */
  private listeners = new Map<EngineEventName, Set<(sender: Engine) => void>>();
  on(name: EngineEventName, cb: (sender: Engine) => void): () => void {
    let set = this.listeners.get(name);
    if (!set) this.listeners.set(name, (set = new Set()));
    set.add(cb);
    return () => set!.delete(cb);
  }
  private emitNamed(name: EngineEventName): void {
    for (const cb of this.listeners.get(name) ?? []) cb(this);
  }
  private emitLegacyAndPort(
    field: 'onEos' | 'onPosition' | 'onPrimed' | 'onBuffering',
    portName: EngineEventName,
  ): void {
    this[field]?.();
    this.emitNamed(portName);
  }

  async init(token: string | null): Promise<void> {
    this.token = token;
    this.ctx = new AudioContext();
    this.gain = this.ctx.createGain();
    this.gain.connect(this.ctx.destination);
  }

  private makeSlot(): ElementSlot {
    if (!this.ctx || !this.gain) throw new Error('native backend not initialized');
    const el = new Audio();
    el.crossOrigin = 'anonymous';
    el.preload = 'auto';
    const src = this.ctx.createMediaElementSource(el);
    src.connect(this.gain);
    el.addEventListener('ended', () => this.emitLegacyAndPort('onEos', 'eos'));
    el.addEventListener('waiting', () => this.emitLegacyAndPort('onBuffering', 'buffering'));
    el.addEventListener('error', () => {
      this.onError?.('This format cannot be played in your browser.');
      this.emitNamed('error');
    });
    el.addEventListener('timeupdate', () => this.emitLegacyAndPort('onPosition', 'tick'));
    return { el, src, url: '' };
  }

  private loadInto(slot: ElementSlot, url: string): void {
    slot.url = url;
    // Same-origin audio (session cookie authenticates). crossOrigin anonymous
    // keeps the response CORS-clean so createMediaElementSource can route it.
    slot.el.src = url;
    slot.el.load();
  }

  /** Port signature (M4): open by resolved item. */
  async open(item: PlaybackItem): Promise<EngineStreamInfo> {
    return this.openSource(item.source.url);
  }

  async openSource(url: string, size = 0): Promise<EngineStreamInfo> {
    void size;
    this.playRequest++;
    this.shouldPlay = false;
    const slot = this.makeSlot();
    this.disposeCurrent();
    this.current = slot;
    this.loadInto(slot, url);
    const info = await this.metadata(slot);
    this.rate = info.rate;
    return info;
  }

  /** Port signature (M4). */
  async prepareNext(item: PlaybackItem): Promise<EngineStreamInfo | null> {
    return this.prepareNextSource(item.source.url);
  }

  async prepareNextSource(url: string, size = 0): Promise<EngineStreamInfo | null> {
    void size;
    this.disposeStandby();
    const slot = this.makeSlot();
    this.standby = slot;
    this.loadInto(slot, url);
    try {
      return await this.metadata(slot);
    } catch {
      this.disposeStandby();
      return null;
    }
  }

  private metadata(slot: ElementSlot): Promise<EngineStreamInfo> {
    return new Promise((resolve, reject) => {
      if (slot.el.readyState >= 1 && slot.el.duration > 0 && !Number.isNaN(slot.el.duration)) {
        resolve(this.infoOf(slot));
        return;
      }
      const onMeta = () => {
        cleanup();
        resolve(this.infoOf(slot));
      };
      const onErr = () => {
        cleanup();
        reject(new Error('unplayable'));
      };
      const cleanup = () => {
        slot.el.removeEventListener('loadedmetadata', onMeta);
        slot.el.removeEventListener('error', onErr);
      };
      slot.el.addEventListener('loadedmetadata', onMeta);
      slot.el.addEventListener('error', onErr);
    });
  }

  private infoOf(slot: ElementSlot): EngineStreamInfo {
    const duration = slot.el.duration > 0 ? slot.el.duration : 0;
    return {
      rate: this.rate,
      channels: this.channels,
      version: 0,
      lengthSamples: Math.floor(duration * this.rate),
    };
  }

  async advance(): Promise<EngineStreamInfo | null> {
    if (!this.standby) return null;
    const promoted = this.standby;
    this.playRequest++;
    this.shouldPlay = false;
    this.standby = null;
    this.disposeCurrent();
    this.current = promoted;
    return this.infoOf(promoted);
  }

  startPumping(): void {
    // Browser-managed decoding needs no producer pump. The controller owns
    // the single audible play() request through the shared Backend contract.
  }

  pausePumping(): void {
    // Native decode is browser-managed; no pump to pause.
  }

  async play(): Promise<void> {
    const slot = this.current;
    if (!slot) return;
    this.shouldPlay = true;
    const request = ++this.playRequest;
    if (this.ctx && this.ctx.state !== 'running') await this.ctx.resume();
    if (request !== this.playRequest || !this.shouldPlay || this.current !== slot) return;
    await slot.el.play();
    if (!this.shouldPlay || this.current !== slot) slot.el.pause();
  }

  async pause(): Promise<void> {
    this.shouldPlay = false;
    this.playRequest++;
    if (this.current) this.current.el.pause();
  }

  /** Port signature (M4). */
  async seekSample(samples: number): Promise<void> {
    return this.seek(samples);
  }

  async seek(sample: number): Promise<void> {
    if (!this.current) return;
    this.shouldPlay = false;
    this.playRequest++;
    this.current.el.currentTime = sample / this.rate;
    this.onPrimed?.();
    this.emitNamed('primed');
  }

  setGain(linear: number): void {
    if (this.gain) this.gain.gain.value = linear;
  }

  getPositionSamples(): number {
    const el = this.current?.el;
    if (!el || Number.isNaN(el.currentTime)) return 0;
    return Math.max(0, Math.floor(el.currentTime * this.rate));
  }

  renderedSamples(): number {
    return this.getPositionSamples();
  }

  getInfo(): EngineStreamInfo | null {
    return this.current ? this.infoOf(this.current) : null;
  }

  get lengthSamples(): number {
    const el = this.current?.el;
    if (!el || Number.isNaN(el.duration)) return 0;
    return Math.floor(el.duration * this.rate);
  }

  private disposeCurrent(): void {
    if (this.current) {
      this.current.el.pause();
      this.current.el.src = '';
      this.current = null;
    }
  }

  private disposeStandby(): void {
    if (this.standby) {
      this.standby.el.pause();
      this.standby.el.src = '';
      this.standby = null;
    }
  }

  async close(): Promise<void> {
    this.shouldPlay = false;
    this.playRequest++;
    this.disposeCurrent();
    this.disposeStandby();
    if (this.ctx) {
      await this.ctx.close();
      this.ctx = null;
      this.gain = null;
    }
  }
}
