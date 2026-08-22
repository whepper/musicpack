// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

// The Musepack playback engine: owns the AudioContext, the PCM AudioWorklet
// (with its bounded ring), and TWO decoder workers so the next track is
// already opened when the current one ends (gapless). The playback controller
// drives it; the UI never touches it directly.
// The PCM AudioWorklet. In dev Vite serves the TS source transformed; in the
// production build it is bundled to /assets/worklet.js (see vite.config.ts).
const WORKLET_URL = import.meta.env.PROD
  ? '/assets/worklet.js'
  : '/src/lib/playback/audio-worklet.ts';
import type { WorkletReport } from './worklet-protocol';
import type {
  Engine,
  EngineCapabilities,
  EngineEvents,
  EngineEventName,
} from '../../../../player-core/src/engine';
import type { PlaybackItem } from '../../../../player-core/src/types';

export type EngineStreamInfo = import('../../../../player-core/src/types').StreamInfo & {
  /** Kept as a type alias for web-internal call sites; structurally the
   *  player-core StreamInfo. */
};

/** Constructor handlers (M4): the core EngineEvents naming. */
export interface EngineHandlers {
  primed: () => void;
  buffering: () => void;
  eos: () => void;
  error: (message: string) => void;
  tick: (streamSamples: number) => void;
}

const MUSEPACK_CAPABILITIES: EngineCapabilities = {
  preloadNext: true,
  sampleAccurateGapless: true,
  decodeGate: true,
  // Phase B: overlap-add mixing in the worklet. Until then the musepack
  // engine reports no crossfade and the core falls back to normal EOS.
  crossfade: false,
};

interface WorkerHandle {
  worker: Worker;
  info: EngineStreamInfo | null;
  sourceInfo: EngineStreamInfo | null;
  eos: boolean;
  nextUrl: string | null;
  cancelOpen: (() => void) | null;
}

const DECODER_WORKER_URL = '/decoder.worker.js';

export class MusepackEngine implements Engine {
  readonly kind = 'musepack' as const;
  readonly capabilities = MUSEPACK_CAPABILITIES;
  private ctx: AudioContext | null = null;
  private gain: GainNode | null = null;
  private node: AudioWorkletNode | null = null;
  private current: WorkerHandle | null = null;
  private standby: WorkerHandle | null = null;
  private h: EngineHandlers;
  private streamSamples = 0;
  /** Stream position at the last ring reset (seek/open); rendered is added. */
  private resetBase = 0;
  private token: string | null = null;
  private servedBytes = 0;
  private ready: Promise<void> | null = null;
  private pumpingRequested = false;
  private backpressured = false;
  private pullInFlight = false;
  private generation = 0;
  private seekResolve: (() => void) | null = null;
  private transitioning = false;
  private standbyRequest = 0;
  private trackEndResolve: (() => void) | null = null;
  private contextShouldRun = false;

  constructor(h: EngineHandlers) {
    this.h = h;
  }

  /** Port-facing event registration (M2). Listeners fire IN ADDITION to the
   *  legacy constructor callbacks (which the web controller still uses until
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

  /** Lazily creates the AudioContext + worklet. Must run after a user gesture. */
  private async ensureContext(): Promise<void> {
    if (this.ctx) return;
    this.ctx = new AudioContext();
    this.gain = this.ctx.createGain();
    this.gain.connect(this.ctx.destination);
    await this.ctx.audioWorklet.addModule(WORKLET_URL);
    this.node = new AudioWorkletNode(this.ctx, 'musicpack-pcm', {
      numberOfOutputs: 1,
      outputChannelCount: [2],
      channelCount: 2,
      channelCountMode: 'explicit',
    });
    this.node.connect(this.gain);
    this.node.port.onmessage = (ev: MessageEvent<WorkletReport>) => {
      this.onWorkletMessage(ev.data);
    };
  }

  async init(token: string | null): Promise<void> {
    this.token = token;
    await this.ensureContext();
  }

  private makeWorker(): WorkerHandle {
    const worker = new Worker(DECODER_WORKER_URL, { type: 'classic' });
    const h: WorkerHandle = {
      worker,
      info: null,
      sourceInfo: null,
      eos: false,
      nextUrl: null,
      cancelOpen: null,
    };
    worker.onmessage = (ev: MessageEvent) => {
      this.onWorkerMessage(h, ev.data);
    };
    worker.onerror = (ev: ErrorEvent) => {
      this.h.error('Decoder worker failed.');
      this.emitNamed('error');
      void ev;
    };
    return h;
  }

  private onWorkerMessage(h: WorkerHandle, msg: Record<string, unknown>): void {
    const messageGeneration = msg.generation;
    const generationScoped =
      msg.type === 'pcm' ||
      msg.type === 'seeked' ||
      msg.type === 'stats' ||
      msg.type === 'eos' ||
      msg.type === 'error';
    if (generationScoped && messageGeneration !== this.generation) {
      return;
    }
    switch (msg.type) {
      case 'info':
        h.sourceInfo = {
          rate: msg.rate as number,
          channels: msg.channels as number,
          version: msg.version as number,
          lengthSamples: msg.lengthSamples as number,
        };
        h.info = this.normalizedInfo(h.sourceInfo);
        break;
      case 'pcm':
        if (h === this.current) {
          const samples = msg.samples as Float32Array;
          this.node?.port.postMessage(
            { type: 'samples', buffer: samples.buffer, generation: this.generation },
            [samples.buffer as ArrayBuffer],
          );
        }
        break;
      case 'seeked': {
        this.transitioning = false;
        const resolve = this.seekResolve;
        this.seekResolve = null;
        resolve?.();
        break;
      }
      case 'stats':
        this.servedBytes = msg.served as number;
        break;
      case 'eos':
        this.pullInFlight = false;
        if (!h.eos) {
          h.eos = true;
          if (h === this.current) { this.h.eos(); this.emitNamed('eos'); }
        }
        break;
      case 'error':
        this.pullInFlight = false;
        this.h.error((msg.message as string) ?? 'Decoder error.');
        this.emitNamed('error');
        break;
    }
  }

  private onWorkletMessage(msg: WorkletReport): void {
    if (msg.generation !== this.generation) return;
    switch (msg.type) {
      case 'rendered':
        this.streamSamples = this.resetBase + msg.frames;
        this.h.tick(this.streamSamples);
        this.emitNamed('tick');
        break;
      case 'primed':
        this.h.primed();
        this.emitNamed('primed');
        break;
      case 'need':
        this.backpressured = false;
        this.resumePumpingIfRequested();
        break;
      case 'full':
        this.backpressured = true;
        this.current?.worker.postMessage({ type: 'pause', generation: this.generation });
        break;
      case 'underrun':
        this.h.buffering();
        this.emitNamed('buffering');
        this.backpressured = false;
        this.resumePumpingIfRequested();
        break;
      case 'accepted':
        this.pullInFlight = false;
        this.resumePumpingIfRequested();
        break;
      case 'trackEnded': {
        const resolve = this.trackEndResolve;
        this.trackEndResolve = null;
        resolve?.();
        break;
      }
      case 'error':
        this.pullInFlight = false;
        this.h.error(msg.message ?? 'Audio worklet protocol error.');
        this.emitNamed('error');
        break;
    }
  }

  /** Port signature (M4): open by resolved item. */
  async open(item: PlaybackItem): Promise<EngineStreamInfo> {
    return this.openSource(item.source.url, item.source.byteSize ?? -1);
  }

  /** Opens the first track in the current slot (fetches header + seek table). */
  async openSource(url: string, size: number): Promise<EngineStreamInfo> {
    await this.ensureContext();
    const generation = ++this.generation;
    this.pumpingRequested = false;
    this.transitioning = true;
    this.pullInFlight = false;
    this.seekResolve?.();
    this.seekResolve = null;
    this.trackEndResolve?.();
    this.trackEndResolve = null;
    this.node?.port.postMessage({ type: 'reset', generation });
    ++this.standbyRequest;
    await this.closeCurrent();
    await this.closeStandby();
    if (generation !== this.generation) throw new Error('Playback open was superseded.');
    const h = this.makeWorker();
    h.nextUrl = url;
    this.current = h;
    this.backpressured = false;
    this.resetBase = 0;
    this.streamSamples = 0;
    try {
      const info = await this.openInWorker(h, url, size, generation);
      if (generation !== this.generation || this.current !== h) {
        if (this.current === h) this.current = null;
        await this.closeWorker(h);
        throw new Error('Playback open was superseded.');
      }
      this.configureWorklet(h.sourceInfo ?? info, generation);
      this.transitioning = false;
      return info;
    } catch (error) {
      if (this.current === h) {
        this.current = null;
        await this.closeWorker(h);
      }
      throw error;
    }
  }

  /** Port signature (M4): standby-open by resolved item. */
  async prepareNext(item: PlaybackItem): Promise<EngineStreamInfo | null> {
    return this.prepareNextSource(item.source.url, item.source.byteSize ?? -1);
  }

  /** Opens the next track in the standby slot, ahead of the current one. */
  async prepareNextSource(url: string, size: number): Promise<EngineStreamInfo | null> {
    const request = ++this.standbyRequest;
    const previous = this.standby;
    if (previous) this.standby = null;
    if (previous) await this.closeWorker(previous);
    if (request !== this.standbyRequest) return null;
    const h = this.makeWorker();
    h.nextUrl = url;
    this.standby = h;
    try {
      const info = await this.openInWorker(h, url, size, this.generation);
      if (request !== this.standbyRequest || this.standby !== h) {
        if (this.standby === h) {
          this.standby = null;
          await this.closeWorker(h);
        }
        return null;
      }
      return info;
    } catch {
      if (this.standby === h) {
        this.standby = null;
        await this.closeWorker(h);
      }
      return null;
    }
  }

  private async openInWorker(
    h: WorkerHandle,
    url: string,
    size: number,
    generation: number,
  ): Promise<EngineStreamInfo> {
    const infoPromise = new Promise<EngineStreamInfo>((resolve, reject) => {
      const orig = h.worker.onmessage;
      const finish = () => {
        h.cancelOpen = null;
        h.worker.onmessage = orig;
      };
      h.cancelOpen = () => {
        finish();
        reject(new Error('Playback open was superseded.'));
      };
      h.worker.onmessage = (ev: MessageEvent) => {
        const msg = ev.data;
        if (msg.type === 'info') {
          h.sourceInfo = {
            rate: msg.rate,
            channels: msg.channels,
            version: msg.version,
            lengthSamples: msg.lengthSamples,
          };
          if (h.sourceInfo.channels < 1 || h.sourceInfo.channels > 2) {
            finish();
            reject(new Error(`Unsupported Musepack channel count: ${h.sourceInfo.channels}`));
            return;
          }
          h.info = this.normalizedInfo(h.sourceInfo);
          finish();
          resolve(h.info);
        } else if (msg.type === 'error') {
          finish();
          reject(new Error(msg.message));
        }
      };
    });
    h.worker.postMessage({ type: 'open', url, size, token: this.token, generation });
    return infoPromise;
  }

  private configureWorklet(sourceInfo: EngineStreamInfo, generation: number): void {
    this.node?.port.postMessage({
      type: 'config',
      sourceRate: sourceInfo.rate,
      sourceChannels: sourceInfo.channels,
      outputRate: this.outputRate,
      outputChannels: 2,
      generation,
    });
    this.node?.port.postMessage({ type: 'reset', generation });
    this.streamSamples = 0;
  }

  /** Promotes the standby worker to current (gapless handoff at EOS). */
  async advance(): Promise<EngineStreamInfo | null> {
    const generation = this.generation;
    await this.finishTrack(generation);
    if (generation !== this.generation) return null;
    const standby = this.standby;
    if (!standby || !standby.info) return null;
    const promotedSourceInfo = standby.sourceInfo;
    if (!promotedSourceInfo) return null;
    ++this.standbyRequest;
    const promoted = standby;
    this.standby = null;
    const previous = this.current;
    if (previous) this.current = null;
    if (previous) await this.closeWorker(previous);
    if (generation !== this.generation || this.current !== null) {
      await this.closeWorker(promoted);
      return null;
    }
    this.current = promoted;
    this.node?.port.postMessage({
      type: 'track',
      sourceRate: promotedSourceInfo.rate,
      sourceChannels: promotedSourceInfo.channels,
      generation,
    });
    return promoted.info;
  }

  private finishTrack(generation: number): Promise<void> {
    if (!this.node) return Promise.resolve();
    this.trackEndResolve?.();
    return new Promise((resolve) => {
      this.trackEndResolve = resolve;
      this.node?.port.postMessage({ type: 'end', generation });
    });
  }

  startPumping(): void {
    this.pumpingRequested = true;
    this.resumePumpingIfRequested();
  }

  pausePumping(): void {
    this.pumpingRequested = false;
    this.current?.worker.postMessage({ type: 'pause', generation: this.generation });
  }

  private resumePumpingIfRequested(): void {
    if (
      this.pumpingRequested &&
      !this.backpressured &&
      !this.pullInFlight &&
      !this.transitioning &&
      this.current
    ) {
      this.pullInFlight = true;
      this.current.worker.postMessage({ type: 'play', generation: this.generation });
    }
  }

  /** Starts the audio clock (call after a user gesture or after priming). */
  async play(): Promise<void> {
    const ctx = this.ctx;
    if (!ctx) return;
    this.contextShouldRun = true;
    if (ctx.state !== 'running') await ctx.resume();
    if (!this.contextShouldRun && ctx.state === 'running') await ctx.suspend();
  }

  async pause(): Promise<void> {
    this.contextShouldRun = false;
    if (this.ctx && this.ctx.state !== 'closed') await this.ctx.suspend();
  }

  /** Port signature (M4). */
  async seekSample(samples: number): Promise<void> {
    return this.seek(samples);
  }

  async seek(sample: number): Promise<void> {
    if (!this.current) return;
    const generation = ++this.generation;
    this.transitioning = true;
    this.backpressured = false;
    this.pullInFlight = false;
    this.seekResolve?.();
    this.seekResolve = null;
    this.trackEndResolve?.();
    this.trackEndResolve = null;
    this.current.eos = false;
    this.node?.port.postMessage({ type: 'reset', generation });
    this.resetBase = sample;
    this.streamSamples = sample;
    const sourceSample = this.outputToSourceSample(sample, this.current.sourceInfo);
    await this.postSeek(sourceSample, generation);
    if (generation !== this.generation) return;
    if (this.pumpingRequested) this.startPumping();
  }

  private postSeek(sample: number, generation: number): Promise<void> {
    return new Promise((resolve) => {
      const h = this.current;
      if (!h) return resolve();
      this.seekResolve = resolve;
      h.worker.postMessage({ type: 'seek', sample, generation });
    });
  }

  setGain(linear: number): void {
    if (this.gain) this.gain.gain.value = linear;
  }

  getPositionSamples(): number {
    return this.streamSamples;
  }

  /** AudioContext-rate frames rendered since the last reset (port name). */
  renderedSamples(): number {
    return this.streamSamples - this.resetBase;
  }

  /** Legacy alias (tests/web call sites until M4 cleanup). */
  getRenderedSamples(): number {
    return this.renderedSamples();
  }

  getServedBytes(): number {
    return this.servedBytes;
  }

  getInfo(): EngineStreamInfo | null {
    return this.current?.info ?? null;
  }

  get lengthSamples(): number {
    return this.current?.info?.lengthSamples ?? 0;
  }

  get rate(): number {
    return this.current?.info?.rate ?? 44100;
  }

  private get outputRate(): number {
    return this.ctx?.sampleRate ?? 44100;
  }

  private normalizedInfo(sourceInfo: EngineStreamInfo): EngineStreamInfo {
    return {
      rate: this.outputRate,
      channels: 2,
      version: sourceInfo.version,
      lengthSamples: Math.ceil((sourceInfo.lengthSamples * this.outputRate) / sourceInfo.rate),
    };
  }

  private outputToSourceSample(sample: number, sourceInfo: EngineStreamInfo | null): number {
    if (!sourceInfo) return sample;
    return Math.min(
      sourceInfo.lengthSamples,
      Math.max(0, Math.floor((sample * sourceInfo.rate) / this.outputRate)),
    );
  }

  /** Terminates a decoder worker, but only after its nested demand-reader/
   *  network worker has been closed (the decoder acks `closed` after running
   *  its teardown). A bounded timeout guards against a stuck worker.
   *  Terminating the outer worker first would orphan the nested networker. */
  private closeWorker(h: WorkerHandle): Promise<void> {
    return new Promise((resolve) => {
      const worker = h.worker;
      h.cancelOpen?.();
      const orig = worker.onmessage;
      const done = () => {
        clearTimeout(timer);
        worker.onmessage = orig;
        worker.terminate();
        resolve();
      };
      const timer = setTimeout(() => {
        worker.onmessage = orig;
        worker.terminate();
        resolve();
      }, 500);
      worker.onmessage = (ev: MessageEvent) => {
        const msg = ev.data;
        if (msg.type === 'closed') done();
        else this.onWorkerMessage(h, msg);
      };
      worker.postMessage({ type: 'close' });
    });
  }

  private async closeCurrent(): Promise<void> {
    if (this.current) {
      const h = this.current;
      this.current = null;
      await this.closeWorker(h);
    }
  }

  private async closeStandby(): Promise<void> {
    if (this.standby) {
      const h = this.standby;
      this.standby = null;
      await this.closeWorker(h);
    }
  }

  async close(): Promise<void> {
    this.contextShouldRun = false;
    this.pumpingRequested = false;
    this.backpressured = false;
    this.pullInFlight = false;
    this.transitioning = false;
    this.seekResolve?.();
    this.seekResolve = null;
    this.trackEndResolve?.();
    this.trackEndResolve = null;
    ++this.standbyRequest;
    await this.closeCurrent();
    await this.closeStandby();
    if (this.ctx) {
      await this.ctx.close();
      this.ctx = null;
      this.gain = null;
      this.node = null;
    }
  }
}
