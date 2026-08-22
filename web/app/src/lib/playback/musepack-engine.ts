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
import { RING_SECONDS, type WorkletReport } from './worklet-protocol';
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
  // M8 Phase B: overlap-add mixing in the worklet's crossfade lane.
  crossfade: true,
};

interface WorkerHandle {
  worker: Worker;
  info: EngineStreamInfo | null;
  sourceInfo: EngineStreamInfo | null;
  eos: boolean;
  nextUrl: string | null;
  cancelOpen: (() => void) | null;
  /** Crossfade: the lane decode finished before promotion; the engine must
   *  re-emit the core's eos once this handle becomes current. */
  syntheticEosPending?: boolean;
}

const CALLBACK_MARGIN_FRAMES = 2048;

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
  /** Decoder done + output dry: the current track's audible end (used by
   *  the core as a drain-complete signal after crossfade clock shifts). */
  private outputDrained = false;
  private standbyRequest = 0;
  private trackEndResolve: (() => void) | null = null;
  private contextShouldRun = false;
  // Crossfade state (M8 Phase B).
  private xfadeToken = 0;
  private xlaneReadyResolve: ((ok: boolean) => void) | null = null;
  private xfadeSwapResolve: (() => void) | null = null;
  /** Outgoing-lane eos arrived while a fade was in flight (suppressed). */
  private xfadeSuppressEos = false;
  private pendingFadeSeconds = 0;
  /** Output-rate frames the lane must hold before mixing may start. */
  private xlaneNeeded = 0;
  private xlaneFilled = 0;
  /** The standby worker currently routed into the crossfade lane. */
  private xfadeArmedFor: WorkerHandle | null = null;
  /** Token under which the lane was armed (stamped on xsamples/xend). */
  private xfadeArmedToken = 0;
  /** Lane feed queue + credit state (engine-side pacing). */
  private xq: Float32Array[] = [];
  private xAwaitingAccepted = false;
  private xlaneDone = false;
  private xlaneEndSent = false;

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
        } else if (h === this.standby && this.xfadeArmedFor === h) {
          // Crossfade lane: queue the standby's decode and forward it under
          // one-outstanding-credit pacing so the lane ring never overflows.
          this.xq.push(msg.samples as Float32Array);
          this.pumpXlaneQueue();
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
          if (h === this.xfadeArmedFor) {
            // The lane decode finished: once the final credit returns, tell
            // the worklet to flush the resampler tail (xend).
            this.xlaneDone = true;
            this.sendXlaneEndIfDrained();
          }
          if (h === this.current) {
            if (h.syntheticEosPending) {
              // The incoming track finished decoding inside the crossfade
              // lane BEFORE promotion; its one-shot eos is spent. The core
              // still needs an eos at the audible end so the normal
              // boundary chain keeps working — re-emit it now.
              h.syntheticEosPending = false;
              this.h.eos();
              this.emitNamed('eos');
            } else if (this.xfadeSuppressEos) {
              // The outgoing track is draining under a crossfade; the core
              // already advanced its cursor. Swallow this stale eos.
              this.xfadeSuppressEos = false;
            } else {
              this.h.eos();
              this.emitNamed('eos');
            }
          }
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
        // A crossfade-promoted track whose decode finished inside the lane
        // has a spent one-shot worker eos; re-emit the core's eos once its
        // audible content has been consumed.
        const cur = this.current;
        if (
          cur?.syntheticEosPending &&
          cur.info &&
          this.streamSamples - this.resetBase >= cur.info.lengthSamples
        ) {
          cur.syntheticEosPending = false;
          this.h.eos();
          this.emitNamed('eos');
        }
        this.h.tick(this.streamSamples);
        this.emitNamed('tick');
        break;
      case 'primed':
        this.outputDrained = false;
        this.h.primed();
        this.emitNamed('primed');
        break;
      case 'need':
        this.outputDrained = false;
        this.backpressured = false;
        this.resumePumpingIfRequested();
        break;
      case 'full':
        this.backpressured = true;
        this.current?.worker.postMessage({ type: 'pause', generation: this.generation });
        break;
      case 'underrun':
        // Decoder exhausted + output dry = the track's audible end. The core
        // uses this (via isOutputDrained) as an alternative drain-complete
        // signal, because a crossfade compresses the album clock and the
        // positional threshold may never be reached.
        if (this.current?.eos) this.outputDrained = true;
        this.h.buffering();
        this.emitNamed('buffering');
        this.backpressured = false;
        this.resumePumpingIfRequested();
        break;
      case 'accepted':
        if (msg.lane === 2) {
          this.onXlaneAccepted(msg.available);
        } else {
          this.pullInFlight = false;
          this.resumePumpingIfRequested();
        }
        break;
      case 'trackEnded': {
        const resolve = this.trackEndResolve;
        this.trackEndResolve = null;
        resolve?.();
        break;
      }
      case 'xfadeReady': {
        // Whole incoming track queued and flushed: readiness is guaranteed.
        const resolve = this.xlaneReadyResolve;
        this.xlaneReadyResolve = null;
        this.xfadeArmedFor?.worker.postMessage({
          type: 'pause',
          generation: this.generation,
        });
        resolve?.(true);
        break;
      }
      case 'xfaded': {
        const swapResolve = this.xfadeSwapResolve;
        this.xfadeSwapResolve = null;
        swapResolve?.();
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
    this.outputDrained = false;
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

  /**
   * Crossfade (M8 Phase B): overlap-add the standby track over the current
   * one inside the worklet. The standby worker is pumped into the worklet's
   * crossfade lane under its own credit loop; once the lane holds at least
   * the fade window of audio, mixing starts at the end of the outgoing
   * track. Resolves with the incoming track's info AFTER the worklet swap,
   * with the standby promoted exactly like advance() — or null (caller
   * falls back to normal EOS) when there is no standby/context, the decode
   * fails, or a competing transport action superseded the attempt.
   */
  async beginCrossfade(
    next: PlaybackItem,
    fadeSeconds: number,
  ): Promise<EngineStreamInfo | null> {
    const generation = this.generation;
    const outgoing = this.current;
    const incoming = this.standby;
    if (!this.node || !outgoing || !incoming || !incoming.info || !incoming.sourceInfo) {
      return null;
    }
    void next; // the standby is already opened on the prepared item
    this.pendingFadeSeconds = Math.max(0.25, Math.min(15, fadeSeconds));
    const token = ++this.xfadeToken;
    this.xfadeArmedToken = token;

    // Arm the worklet lane for the incoming format.
    this.node.port.postMessage({
      type: 'xfade',
      sourceRate: incoming.sourceInfo.rate,
      sourceChannels: incoming.sourceInfo.channels,
      fadeFrames: Math.max(1, Math.round(this.pendingFadeSeconds * this.outputRate)),
      token,
      generation,
    });

    // Route the standby's decode into the lane. Its decode-eos must NOT
    // reach the core: the incoming track is only beginning. The arm stays
    // set until promotion: between ready and swap the worker keeps
    // decoding into the lane (post-swap stragglers are absorbed by the
    // worklet's token gate).
    this.xfadeArmedFor = incoming;
    incoming.eos = false;
    try {
      const ok = await this.pumpLaneUntilReady(incoming, token, generation);
      if (!ok || generation !== this.generation || this.standby !== incoming) {
        this.abortXfadeAttempt();
        return null;
      }
    } catch {
      this.abortXfadeAttempt();
      return null;
    }

    // Start mixing; the render callback owns the transition timing from
    // here. The outgoing worker may still be decoding — its late eos is
    // swallowed so the core keeps waiting for the swap.
    this.node.port.postMessage({ type: 'xfade-go', token, generation });
    this.xfadeSuppressEos = true;

    // Wait for the swap report (real-time: the fade window elapses in the
    // audio callback), then finish the bookkeeping like a gapless advance.
    await new Promise<void>((resolve) => {
      this.xfadeSwapResolve = resolve;
      // Never hang the fallback forever (e.g. suspended context).
      setTimeout(() => {
        if (this.xfadeSwapResolve === resolve) {
          this.xfadeSwapResolve = null;
          resolve();
        }
      }, 60000);
    });
    if (generation !== this.generation) return null;

    ++this.standbyRequest;
    this.standby = null;
    this.current = incoming;
    this.xfadeArmedFor = null;
    // Any still-queued lane chunks belong to the now-current track; the
    // worklet absorbed the lane resampler at the swap, so they continue
    // seamlessly through the normal path.
    this.xAwaitingAccepted = false;
    for (const samples of this.xq) {
      this.node.port.postMessage(
        { type: 'samples', buffer: samples.buffer, generation: this.generation },
        [samples.buffer as ArrayBuffer],
      );
    }
    this.xq = [];
    this.xlaneDone = false;
    this.xlaneEndSent = false;
    // If the incoming decode already completed inside the lane, its one-shot
    // worker eos is spent: synthesize the core's eos at the audible end so
    // the normal boundary chain (advance/next preload) keeps working.
    if (incoming.eos) {
      incoming.eos = false;
      incoming.syntheticEosPending = true;
    }
    await this.closeWorker(outgoing);
    return incoming.info;
  }

  /** Tears down any in-flight crossfade attempt (competing transport won). */
  private abortXfadeAttempt(): void {
    this.xfadeArmedFor = null;
    this.xfadeArmedToken = -1;
    this.xq = [];
    this.xAwaitingAccepted = false;
    this.xlaneDone = false;
    this.xlaneEndSent = false;
    this.node?.port.postMessage({ type: 'xfade-cancel', generation: this.generation });
    const ready = this.xlaneReadyResolve;
    this.xlaneReadyResolve = null;
    ready?.(false);
    const swap = this.xfadeSwapResolve;
    this.xfadeSwapResolve = null;
    swap?.();
    this.xfadeSuppressEos = false;
  }

  /** Demand-paces the standby worker into the crossfade lane until the lane
   *  holds enough audio to start the mix (the whole fade window when it
   *  fits, else most of the ring — decode keeps feeding the lane in real
   *  time during the fade), or the whole track when it is shorter.
   *  Resolves false on supersede/error. */
  private pumpLaneUntilReady(
    incoming: WorkerHandle,
    token: number,
    generation: number,
  ): Promise<boolean> {
    const fadeFrames = Math.max(1, Math.round(this.pendingFadeSeconds * this.outputRate));
    const ringCapacity = Math.round(this.outputRate * RING_SECONDS);
    this.xlaneNeeded = Math.min(
      incoming.info?.lengthSamples ?? Number.POSITIVE_INFINITY,
      fadeFrames + CALLBACK_MARGIN_FRAMES,
      Math.round(ringCapacity * 0.75),
    );
    this.xlaneFilled = 0;
    this.xlaneDone = false;
    this.xlaneEndSent = false;
    this.xAwaitingAccepted = false;
    this.xq = [];
    return new Promise<boolean>((resolve) => {
      this.xlaneReadyResolve = resolve;
      const timer = setTimeout(() => {
        if (this.xlaneReadyResolve === resolve) {
          this.xlaneReadyResolve = null;
          resolve(false);
        }
      }, 30000);
      void timer;
      incoming.worker.postMessage({ type: 'play', generation });
    });
  }

  /** Forwards one queued lane chunk when the previous credit returned. */
  private pumpXlaneQueue(): void {
    if (this.xAwaitingAccepted || this.xq.length === 0 || !this.node) return;
    const samples = this.xq.shift()!;
    this.xAwaitingAccepted = true;
    this.node.port.postMessage(
      {
        type: 'xsamples',
        buffer: samples.buffer,
        token: this.xfadeArmedToken,
        generation: this.generation,
      },
      [samples.buffer as ArrayBuffer],
    );
  }

  /** Lane bookkeeping after each accepted crossfade credit. */
  private onXlaneAccepted(available: number | undefined): void {
    this.xAwaitingAccepted = false;
    if (available !== undefined) this.xlaneFilled = available;
    this.sendXlaneEndIfDrained();
    this.pumpXlaneQueue();
    // The decoder worker is pull-based (one chunk per 'play'): ask for the
    // next lane chunk until its decode is done.
    if (!this.xlaneDone && !this.xAwaitingAccepted && this.xq.length === 0) {
      this.xfadeArmedFor?.worker.postMessage({ type: 'play', generation: this.generation });
    }
    this.settleXlaneReadiness();
  }

  /** Sends xend once the lane decode is complete and every queued chunk
   *  has been credited; the worklet answers with xfadeReady. */
  private sendXlaneEndIfDrained(): void {
    if (!this.xlaneDone || this.xAwaitingAccepted || this.xq.length > 0) return;
    if (this.xlaneEndSent) return;
    this.xlaneEndSent = true;
    this.node?.port.postMessage({
      type: 'xend',
      token: this.xfadeArmedToken,
      generation: this.generation,
    });
  }

  /** Readiness = the lane holds the needed prime audio, or the whole track
   *  is decoded, credited, and flushed (xfadeReady arrives separately). */
  private settleXlaneReadiness(): void {
    if (!this.xlaneReadyResolve) return;
    if (
      this.xlaneFilled >= this.xlaneNeeded ||
      (this.xlaneDone && !this.xAwaitingAccepted && this.xq.length === 0)
    ) {
      const resolve = this.xlaneReadyResolve;
      this.xlaneReadyResolve = null;
      // Pause the standby decode while waiting for the go moment.
      this.xfadeArmedFor?.worker.postMessage({ type: 'pause', generation: this.generation });
      resolve(true);
    }
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
    // A seek repositions the clock: a pending crossfade synthetic eos must
    // not fire from stale frame accounting.
    this.current.syntheticEosPending = false;
    this.seekResolve?.();
    this.seekResolve = null;
    this.trackEndResolve?.();
    this.trackEndResolve = null;
    this.current.eos = false;
    this.node?.port.postMessage({ type: 'reset', generation });
    this.resetBase = sample;
    this.streamSamples = sample;
    this.outputDrained = false;
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

  /** True when the decoder is exhausted and the output ring has run dry —
   *  the current track has no more audible content. */
  isOutputDrained(): boolean {
    return this.outputDrained;
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
