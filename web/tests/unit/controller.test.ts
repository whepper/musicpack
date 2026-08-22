import { describe, it, expect, vi, beforeEach } from 'vitest';
import { PlayerController } from '../../app/src/lib/playback/controller';
import type { Engine } from '../../player-core/src/engine';
type Backend = Engine;
type BackendEvents = {
  primed(): void;
  buffering(): void;
  eos(): void;
  error(message: string): void;
  tick(): void;
};
import type { EngineStreamInfo } from '../../app/src/lib/playback/musepack-engine';
import { createQueueStore } from '../../app/src/lib/state/queue';
import type { ReleaseDetail, Track } from '../../app/src/lib/api/types';

const RATE = 44100;
const LENGTH = RATE * 10; // 10 s tracks

function track(id: number, title: string, seconds = 10): Track {
  return {
    id,
    number: id,
    title,
    artists: [],
    duration: seconds,
    loudness: { lufs: -7.19, truePeakDb: -4.19 },
    codec: { codec: 'musepack-sv8', mimeType: 'audio/musepack', sampleRate: RATE, channels: 2 },
    audio: { id: id + 100, size: 1000, url: `/api/v1/tracks/${id}/audio` },
  };
}

function release(id: number, titles: string[], seconds = 10, idBase = 0): ReleaseDetail {
  return {
    id,
    edition: '2016 Remaster',
    loudness: { algorithm: 'ITU-R BS.1770-5', albumLufs: -7.28, albumTruePeakDb: -4.19 },
    album: { id: 1, title: 'Test Album', artists: [] },
    media: [{ disc: 1, tracks: titles.map((t, i) => track(idBase + i + 1, t, seconds)) }],
    artwork: [],
    assets: [],
  };
}

class FakeBackend implements Backend {
  readonly kind = 'musepack';
  readonly rate = RATE;
  lengthSamples = LENGTH;
  /** Per-URL stream length override (tests). Unknown URLs use LENGTH. */
  lengthsByUrl = new Map<string, number>();
  private rendered = 0; // worklet rendered counter (resets on seek/open)
  private resetBase = 0;
  private standbyInfo: { rate: number; channels: number; version: number; lengthSamples: number } | null = null;
  opened: string[] = [];
  prepared: string[] = [];
  gains: number[] = [];
  events: BackendEvents;
  paused = false;
  closed = false;
  playGate: Promise<void> | null = null;
  seekGate: Promise<void> | null = null;
  pauseGate: Promise<void> | null = null;
  /** Holds open() so tests can park a load mid-flight. */
  openGate: Promise<void> | null = null;
  advanceGate: Promise<{ rate: number; channels: number; version: number; lengthSamples: number } | null> | null = null;
  plays = 0;
  seeks: number[] = [];

  constructor(_kind: 'musepack' | 'native', events: BackendEvents) {
    this.events = events;
  }

  readonly capabilities = {
    preloadNext: true,
    sampleAccurateGapless: true,
    decodeGate: true,
    crossfade: false,
  };
  private listeners = new Map<string, Set<(sender: Engine) => void>>();
  on(name: string, cb: (sender: Engine) => void): () => void {
    let set = this.listeners.get(name);
    if (!set) this.listeners.set(name, (set = new Set()));
    set.add(cb);
    return () => set!.delete(cb);
  }

  async init() {}
  lengthFor(url: string): number {
    return this.lengthsByUrl.get(url) ?? LENGTH;
  }
  async open(item: import('../../player-core/src/types').PlaybackItem) {
    const url = item.source.url;
    this.opened.push(url);
    this.rendered = 0;
    if (this.openGate) await this.openGate;
    return { rate: RATE, channels: 2, version: 8, lengthSamples: this.lengthFor(url) };
  }
  async prepareNext(item: import('../../player-core/src/types').PlaybackItem) {
    const url = item.source.url;
    this.prepared.push(url);
    const info = { rate: RATE, channels: 2, version: 8, lengthSamples: this.lengthFor(url) };
    this.standbyInfo = info;
    return info;
  }
  async advance() {
    if (this.advanceGate) return this.advanceGate;
    return this.standbyInfo;
  }
  startPumping() {
    this.paused = false;
  }
  pausePumping() {
    this.paused = true;
  }
  async play() {
    this.plays++;
    await this.playGate;
  }
  async pause() {
    await this.pauseGate;
  }
  seekSample(samples: number): Promise<void> {
    return this.seek(samples);
  }
  renderedSamples(): number {
    return this.getRenderedSamples();
  }
  async seek(sample: number) {
    this.seeks.push(sample);
    this.resetBase = sample;
    this.rendered = 0; // ring reset: rendered counter restarts
    await this.seekGate;
  }
  setGain(g: number) {
    this.gains.push(g);
  }
  getRenderedSamples() {
    return this.rendered;
  }
  getInfo() {
    return { rate: RATE, channels: 2, version: 8, lengthSamples: LENGTH };
  }
  getServedBytes() {
    return 0;
  }
  async close() {
    this.closed = true;
  }

  // test drivers
  setRendered(frames: number) {
    this.rendered = frames;
  }
  emitPrimed() {
    this.events.primed();
  }
  emitEos() {
    this.events.eos();
  }
  emitBuffering() {
    this.events.buffering();
  }
  emitPosition() {
    this.events.tick();
  }
  emitError(msg: string) {
    this.events.error(msg);
  }
}

function make() {
  const queue = createQueueStore();
  const lengthsByUrl = new Map<string, number>();
  let backend: FakeBackend | null = null;
  const player = new PlayerController(queue, {
    backendFactory: (kind, events) => {
      const b = new FakeBackend(kind, events);
      b.lengthsByUrl = lengthsByUrl; // shared map: tests mutate before opens
      backend = b;
      return b;
    },
  });
  player.init();
  return { player, queue, getBackend: () => backend!, lengthsByUrl };
}

function makeWithStorage(storage: { get: () => string | null; set: (v: string | null) => void }) {
  const queue = createQueueStore();
  const lengthsByUrl = new Map<string, number>();
  let backend: FakeBackend | null = null;
  const player = new PlayerController(queue, {
    backendFactory: (kind, events) => {
      const b = new FakeBackend(kind, events);
      b.lengthsByUrl = lengthsByUrl;
      backend = b;
      return b;
    },
    storage,
  });
  player.init();
  return { player, queue, getBackend: () => backend!, lengthsByUrl };
}

/** Flushes promise chains (state transitions land on .then microtasks). */
async function flush(times = 3): Promise<void> {
  for (let i = 0; i < times; i++) await Promise.resolve();
}

/** Like make(), but the open gate can be armed/released by the test even
 *  before the backend exists (the factory runs inside the first load). */
function makeWithOpenGate() {
  const queue = createQueueStore();
  const lengthsByUrl = new Map<string, number>();
  let backend: FakeBackend | null = null;
  const holder: { gate: Promise<void> | null } = { gate: null };
  const player = new PlayerController(queue, {
    backendFactory: (kind, events) => {
      const b = new FakeBackend(kind, events);
      b.lengthsByUrl = lengthsByUrl;
      b.openGate = holder.gate;
      backend = b;
      return b;
    },
  });
  player.init();
  return {
    player,
    queue,
    getBackend: () => backend!,
    lengthsByUrl,
    setGate(gate: Promise<void> | null): void {
      holder.gate = gate;
      if (backend) backend.openGate = gate;
    },
  };
}

describe('PlayerController', () => {
  beforeEach(() => {
    vi.restoreAllMocks();
  });

  it('playAlbum transitions loading -> buffering -> playing on prime', async () => {
    const { player, getBackend } = make();
    const r = release(9, ['T1', 'T2']);
    const pending = player.playAlbum(r, 'Test Album', 'Artist');
    expect(player.model.get().state).toBe('loading');
    await pending;
    expect(player.model.get().state).toBe('buffering');
    expect(player.model.get().current?.track.title).toBe('T1');
    expect(getBackend().prepared).toContain('/api/v1/tracks/2/audio'); // gapless prepare
    getBackend().emitPrimed();
    await flush();
    expect(player.model.get().state).toBe('playing');
  });

  it('applies album normalization gain by default and tracks it separately from volume', async () => {
    const { player, getBackend } = make();
    await player.playAlbum(release(9, ['T1']), 'Test Album', 'Artist');
    getBackend().emitPrimed();
    const gain0 = getBackend().gains.at(-1) ?? 0;
    expect(gain0).toBeGreaterThan(0);
    expect(gain0).toBeLessThan(1);
    expect(player.model.get().normDb).toBeLessThan(0);
    player.setVolume(1.0);
    const gain1 = getBackend().gains.at(-1) ?? 0;
    expect(gain1).toBeGreaterThan(gain0); // volume up, norm unchanged
    expect(player.model.get().normDb).toBe(player.model.get().normDb); // norm intact
    player.setNormalizeMode('off');
    expect(getBackend().gains.at(-1)).toBeCloseTo(1.0, 6); // volume only
  });

  it('position advances with rendered samples', async () => {
    const { player, getBackend } = make();
    await player.playAlbum(release(9, ['T1']), 'Test Album', 'Artist');
    getBackend().emitPrimed();
    getBackend().setRendered(5000);
    getBackend().emitPosition();
    expect(player.model.get().positionSeconds).toBeCloseTo(5000 / RATE, 6);
    expect(player.model.get().durationSeconds).toBeCloseTo(20 / 2, 6); // single track 10s
  });

  it('seek maps album position into the current track', async () => {
    const { player, getBackend } = make();
    await player.playAlbum(release(9, ['T1', 'T2']), 'Test Album', 'Artist');
    getBackend().emitPrimed();
    await flush();
    await player.seek(5);
    await flush();
    expect(player.model.get().state).toBe('buffering');
    getBackend().emitPrimed();
    await flush();
    expect(player.model.get().positionSeconds).toBeCloseTo(5, 2);
    getBackend().setRendered(2 * RATE); // played 2 more seconds past the seek
    getBackend().emitPosition();
    expect(player.model.get().positionSeconds).toBeCloseTo(7, 2);
  });

  it('advances gaplessly at EOS and ends after the last track', async () => {
    const { player, queue, getBackend } = make();
    await player.playAlbum(release(9, ['T1', 'T2']), 'Test Album', 'Artist');
    getBackend().emitPrimed();
    expect(queue.get().index).toBe(0);

    getBackend().setRendered(LENGTH); // track 1 finished
    getBackend().emitEos();
    // async advance -> next()
    await flush();
    expect(queue.get().index).toBe(1);
    expect(player.model.get().current?.track.title).toBe('T2');
    expect(player.model.get().state).toBe('playing');

    // last track finished -> no next -> pendingEnded -> drain -> ended
    getBackend().setRendered(2 * LENGTH);
    getBackend().emitEos();
    await flush();
    getBackend().setRendered(2 * LENGTH);
    getBackend().emitPosition();
    await flush();
    expect(player.model.get().state).toBe('ended');
  });

  it('ends immediately when final EOS arrives after the native-style position reached duration', async () => {
    const { player, getBackend } = make();
    await player.playAlbum(release(9, ['T1']), 'Test Album', 'Artist');
    const backend = getBackend();
    backend.setRendered(LENGTH);
    backend.emitEos();
    await flush();
    expect(player.model.get().state).toBe('ended');
  });

  it('does not report playing when promoted-track play is rejected', async () => {
    const { player, getBackend } = make();
    await player.playAlbum(release(9, ['T1', 'T2']), 'Test Album', 'Artist');
    const backend = getBackend();
    backend.emitPrimed();
    await flush();
    backend.playGate = Promise.reject(new Error('play rejected'));

    backend.emitEos();
    await flush(6);
    expect(player.model.get().current?.track.title).toBe('T2');
    expect(player.model.get().state).toBe('paused');
  });

  it('pause stops pumping and keeps the position', async () => {
    const { player, getBackend } = make();
    await player.playAlbum(release(9, ['T1']), 'Test Album', 'Artist');
    getBackend().emitPrimed();
    getBackend().setRendered(4000);
    getBackend().emitPosition();
    await player.pause();
    await flush();
    expect(player.model.get().state).toBe('paused');
    expect(player.model.get().positionSeconds).toBeCloseTo(4000 / RATE, 6);
    await player.togglePlay();
    expect(player.model.get().state).toBe('playing');
  });

  it('recovers from a PCM underrun after the backend primes again', async () => {
    const { player, getBackend } = make();
    await player.playAlbum(release(9, ['T1']), 'Test Album', 'Artist');
    const backend = getBackend();
    backend.emitPrimed();
    await flush();
    expect(player.model.get().state).toBe('playing');

    backend.emitBuffering();
    expect(player.model.get().state).toBe('buffering');
    backend.emitPrimed();
    await flush();
    expect(player.model.get().state).toBe('playing');
  });

  it('does not let queued underrun and primed events override an explicit pause', async () => {
    const { player, getBackend } = make();
    await player.playAlbum(release(9, ['T1']), 'Test Album', 'Artist');
    const backend = getBackend();
    let finishPlay = () => {};
    backend.playGate = new Promise<void>((resolve) => {
      finishPlay = resolve;
    });
    backend.emitPrimed();
    await flush();
    await player.pause();

    backend.emitBuffering();
    backend.emitPrimed();
    finishPlay();
    await flush();
    expect(player.model.get().state).toBe('paused');
    expect(backend.paused).toBe(true);
  });

  it('ignores a stale EOS continuation after the user moves to the next track', async () => {
    const { player, queue, getBackend } = make();
    await player.playAlbum(release(9, ['T1', 'T2', 'T3']), 'Test Album', 'Artist');
    const backend = getBackend();
    let finishAdvance = (_value: EngineStreamInfo | null) => {};
    backend.advanceGate = new Promise((resolve) => {
      finishAdvance = resolve;
    });

    backend.emitEos();
    await flush();
    await player.next();
    expect(queue.get().index).toBe(1);
    expect(player.model.get().current?.track.title).toBe('T2');

    finishAdvance({ rate: RATE, channels: 2, version: 8, lengthSamples: LENGTH });
    await flush();
    expect(queue.get().index).toBe(1);
    expect(player.model.get().current?.track.title).toBe('T2');
  });

  it('ignores a stale EOS continuation after a seek resets the stream', async () => {
    const { player, queue, getBackend } = make();
    await player.playAlbum(release(9, ['T1', 'T2']), 'Test Album', 'Artist');
    const backend = getBackend();
    let finishAdvance = (_value: EngineStreamInfo | null) => {};
    backend.advanceGate = new Promise((resolve) => {
      finishAdvance = resolve;
    });

    backend.emitEos();
    await flush();
    await player.seek(2);
    finishAdvance(null);
    await flush();

    expect(queue.get().index).toBe(0);
    expect(player.model.get().current?.track.title).toBe('T1');
    backend.setRendered(2 * LENGTH);
    backend.emitPosition();
    expect(player.model.get().state).not.toBe('ended');
  });

  it('keeps the controller and audio context paused while a seek reprimes', async () => {
    const { player, getBackend } = make();
    await player.playAlbum(release(9, ['T1']), 'Test Album', 'Artist');
    const backend = getBackend();
    backend.emitPrimed();
    await flush();
    await player.pause();
    const playsBeforeSeek = backend.plays;
    let finishSeek = () => {};
    backend.seekGate = new Promise<void>((resolve) => {
      finishSeek = resolve;
    });

    const seeking = player.seek(5);
    await flush();
    backend.emitPrimed();
    await flush();
    expect(player.model.get().state).toBe('paused');
    expect(backend.plays).toBe(playsBeforeSeek);

    finishSeek();
    await seeking;
    backend.emitPrimed();
    await flush();
    expect(player.model.get().state).toBe('paused');
    expect(backend.plays).toBe(playsBeforeSeek);
  });

  it('does not let a superseded seek resume pumping after stop', async () => {
    const { player, getBackend } = make();
    await player.playAlbum(release(9, ['T1']), 'Test Album', 'Artist');
    const backend = getBackend();
    backend.emitPrimed();
    await flush();
    let finishSeek = () => {};
    backend.seekGate = new Promise<void>((resolve) => {
      finishSeek = resolve;
    });

    const seeking = player.seek(5);
    await flush();
    const stopping = player.stop();
    await flush();
    expect(player.model.get().state).toBe('idle');
    finishSeek();
    await Promise.all([seeking, stopping]);
    expect(player.model.get().state).toBe('idle');
    expect(backend.paused).toBe(true);
  });

  it('does not let a stale resume completion override a later pause', async () => {
    const { player, getBackend } = make();
    await player.playAlbum(release(9, ['T1']), 'Test Album', 'Artist');
    const backend = getBackend();
    backend.emitPrimed();
    await flush();
    await player.pause();
    let finishPlay = () => {};
    backend.playGate = new Promise<void>((resolve) => {
      finishPlay = resolve;
    });

    const resuming = player.resume();
    await flush();
    const pausing = player.pause();
    finishPlay();
    await Promise.all([resuming, pausing]);
    expect(player.model.get().state).toBe('paused');
    expect(backend.paused).toBe(true);
  });

  it('does not let a stale stop seek a newly loaded track', async () => {
    const { player, getBackend } = make();
    await player.playAlbum(release(9, ['T1']), 'First', 'Artist');
    const backend = getBackend();
    let finishPause = () => {};
    backend.pauseGate = new Promise<void>((resolve) => {
      finishPause = resolve;
    });

    const stopping = player.stop();
    await flush();
    const loading = player.playAlbum(release(10, ['T2']), 'Second', 'Artist');
    await loading;
    const seeksBeforeStopCompletes = backend.seeks.length;
    finishPause();
    await stopping;

    expect(player.model.get().current?.track.title).toBe('T2');
    expect(player.model.get().state).toBe('buffering');
    expect(backend.seeks).toHaveLength(seeksBeforeStopCompletes);
  });

  it('reports friendly errors on backend failure', async () => {
    const { player, getBackend } = make();
    await player.playAlbum(release(9, ['T1']), 'Test Album', 'Artist');
    getBackend().emitError('This format is not supported by this browser.');
    expect(player.model.get().state).toBe('error');
    expect(player.model.get().error).toContain('not supported');
  });

  it('seeks across a track boundary by switching to the target track', async () => {
    const { player, queue, getBackend } = make();
    await player.playAlbum(release(9, ['T1', 'T2']), 'Test Album', 'Artist');
    const b = getBackend();
    b.emitPrimed();
    // 15 s lands 5 s into track 2 (tracks are 10 s each)
    await player.seek(15);
    await flush();
    expect(queue.get().index).toBe(1);
    expect(player.model.get().current?.track.title).toBe('T2');
    // the reported position is the album-absolute target, not the track end
    expect(player.model.get().positionSeconds).toBeCloseTo(15, 1);
    // and it tracks the ACTUAL decoded position (1 s rendered in track 2)
    b.setRendered(RATE);
    b.emitPosition();
    await flush();
    expect(player.model.get().positionSeconds).toBeCloseTo(16, 1);
    expect(player.model.get().state).not.toBe('error');
  });

  it('never regresses the queue cursor during a gapless handoff', async () => {
    const { player, queue, getBackend } = make();
    await player.playAlbum(release(9, ['T1', 'T2']), 'Test Album', 'Artist');
    getBackend().emitPrimed();
    // decode of track 1 finishes while the ring still renders its tail
    getBackend().setRendered(LENGTH - 20000);
    getBackend().emitEos();
    await flush();
    expect(queue.get().index).toBe(1); // EOS owns the advance
    expect(player.model.get().current?.track.title).toBe('T2');
    // a tick whose rendered position still describes track 1 must not move
    // the cursor back to track 1
    getBackend().setRendered(LENGTH - 5000);
    getBackend().emitPosition();
    await flush();
    expect(queue.get().index).toBe(1);
    expect(player.model.get().current?.track.title).toBe('T2');
  });

  it('keys decoded lengths by track id so replaced queues cannot leak them', async () => {
    const { player, getBackend, lengthsByUrl } = make();
    // album A: 20 s tracks (ids 1, 2)
    lengthsByUrl.set('/api/v1/tracks/1/audio', 20 * RATE);
    lengthsByUrl.set('/api/v1/tracks/2/audio', 20 * RATE);
    await player.playAlbum(release(9, ['A1', 'A2'], 20), 'Album A', 'Artist');
    const b = getBackend();
    b.emitPrimed();
    expect(player.model.get().durationSeconds).toBeCloseTo(40, 1);

    // album B: DIFFERENT track rows (ids 101, 102), 10 s each, started at
    // index 1. Its lengths must come from B's own metadata, never from A.
    await player.playAlbum(release(10, ['B1', 'B2'], 10, 100), 'Album B', 'Artist', 1);
    b.emitPrimed();
    await flush();
    expect(player.model.get().durationSeconds).toBeCloseTo(20, 1);
    // a seek across B's track boundary uses B's track lengths
    await player.seek(15);
    await flush();
    expect(player.model.get().current?.track.title).toBe('B2');
    expect(player.model.get().positionSeconds).toBeCloseTo(15, 1);
  });

  it('keeps decoded lengths across loads so later tracks report exact starts', async () => {
    const { player, getBackend, lengthsByUrl } = make();
    // Manifest placeholder says 48 s per track; real decodes differ
    // (the Long Player fixture scenario).
    const r = release(9, ['T1', 'T2', 'T3'], 48);
    lengthsByUrl.set('/api/v1/tracks/1/audio', 39 * RATE);
    lengthsByUrl.set('/api/v1/tracks/2/audio', 5 * RATE);
    lengthsByUrl.set('/api/v1/tracks/3/audio', 7 * RATE);
    await player.playAlbum(r, 'Album', 'Artist');
    const b = getBackend();
    b.emitPrimed();
    b.setRendered(39 * RATE);
    b.emitEos(); // gapless advance into T2
    await flush();
    b.emitPosition();
    await flush();
    expect(player.model.get().currentTrackStartSeconds).toBeCloseTo(39, 1);

    // next() performs a fresh load at index > 0; the exact length of T1
    // must survive it, or every later offset drifts into placeholder land.
    await player.next();
    await flush();
    b.emitPrimed();
    b.emitPosition();
    await flush();
    expect(player.model.get().currentTrackStartSeconds).toBeCloseTo(44, 1); // 39+5
    expect(player.model.get().durationSeconds).toBeCloseTo(51, 1); // 39+5+7
  });

  it('previous() restarts the current song instead of the queue start', async () => {
    const { player, queue, getBackend } = make();
    await player.playAlbum(release(9, ['T1', 'T2', 'T3']), 'Test Album', 'Artist');
    const b = getBackend();
    b.emitPrimed();
    await flush();
    // gapless advance into T2
    b.setRendered(LENGTH);
    b.emitEos();
    await flush();
    expect(queue.get().index).toBe(1);
    // five seconds into T2 (rendered counts continuously across the handoff)
    b.setRendered(LENGTH + 5 * RATE);
    b.emitPosition();
    await flush();
    expect(player.model.get().positionSeconds).toBeCloseTo(15, 1);

    await player.previous();
    await flush();
    // stays on T2 and restarts it, rather than reloading T1 at queue start
    expect(queue.get().index).toBe(1);
    expect(player.model.get().current?.track.title).toBe('T2');
    expect(player.model.get().positionSeconds).toBeCloseTo(10, 1);
    expect(b.seeks[b.seeks.length - 1]).toBe(0); // within-track seek to sample 0
  });

  it('playQueueIndex moves within the queue without collapsing it', async () => {
    const { player, queue, getBackend } = make();
    await player.playAlbum(release(9, ['T1', 'T2', 'T3']), 'Test Album', 'Artist');
    const b = getBackend();
    b.emitPrimed();
    await flush();
    expect(queue.get().items.length).toBe(3);

    await player.playQueueIndex(2);
    await flush();
    // the queue is preserved, the cursor moved, and the target track opened
    expect(queue.get().items.length).toBe(3);
    expect(queue.get().index).toBe(2);
    expect(player.model.get().current?.track.title).toBe('T3');
    expect(b.opened).toContain('/api/v1/tracks/3/audio');

    // clicking the already-current item does not reload anything
    const opens = b.opened.length;
    await player.playQueueIndex(2);
    await flush();
    expect(b.opened.length).toBe(opens);
  });

  it('restores a persisted session as paused and resumes at the saved spot', async () => {
    const map = new Map<string, string>();
    const storage = {
      get: () => map.get('musicpack.player.v1') ?? null,
      set: (v: string | null) => {
        if (v === null) map.delete('musicpack.player.v1');
        else map.set('musicpack.player.v1', v);
      },
    };
    const a = makeWithStorage(storage);
    vi.useFakeTimers({ toFake: ['setTimeout', 'Date'] });
    try {
      await a.player.playAlbum(release(9, ['T1', 'T2', 'T3']), 'Album', 'Artist');
      const ba = a.getBackend();
      ba.emitPrimed();
      // Advance fake time past the persist throttle, then tick: the position
      // write must land (a trailing write alone would be async).
      vi.advanceTimersByTime(2001);
      ba.setRendered(5 * RATE);
      ba.emitPosition();
      expect(map.size).toBe(1);

      // A fresh controller (simulated page reload) restores the session.
      const b = makeWithStorage(storage);
      const m = b.player.model.get();
      expect(m.state).toBe('paused');
      expect(m.current?.track.title).toBe('T1');
      expect(m.positionSeconds).toBeCloseTo(5, 2);
      expect(b.queue.get().items.length).toBe(3);

      // Pressing play rebuilds the backend and seeks to the saved spot.
      await b.player.togglePlay();
      await flush();
      const bb = b.getBackend();
      expect(bb.opened).toContain('/api/v1/tracks/1/audio');
      expect(bb.seeks[bb.seeks.length - 1]).toBe(5 * RATE);
      bb.emitPrimed();
      await flush();
      expect(b.player.model.get().state).toBe('playing');
      expect(b.player.model.get().positionSeconds).toBeCloseTo(5, 2);
    } finally {
      vi.useRealTimers();
    }
  });

  it('persists and restores repeat/shuffle policy across a reload (v2)', async () => {
    const map = new Map<string, string>();
    const storage = {
      get: () => map.get('musicpack.player.v1') ?? null,
      set: (v: string | null) => {
        if (v === null) map.delete('musicpack.player.v1');
        else map.set('musicpack.player.v1', v);
      },
    };
    const a = makeWithStorage(storage);
    vi.useFakeTimers({ toFake: ['setTimeout', 'Date'] });
    try {
      await a.player.playAlbum(release(9, ['T1', 'T2', 'T3']), 'Album', 'Artist');
      const ba = a.getBackend();
      ba.emitPrimed();
      a.player.setRepeat('all');
      a.player.setShuffle(true);
      vi.advanceTimersByTime(2001);
      ba.setRendered(2 * RATE);
      ba.emitPosition();
      expect(map.size).toBe(1);

      // The persisted payload is v2 with the policy embedded.
      const raw = JSON.parse(map.get('musicpack.player.v1')!) as { v: number; repeat: string; shuffle: boolean };
      expect(raw.v).toBe(2);
      expect(raw.repeat).toBe('all');
      expect(raw.shuffle).toBe(true);

      // A fresh controller restores the policy.
      const b = makeWithStorage(storage);
      const m = b.player.model.get();
      expect(m.repeat).toBe('all');
      expect(m.shuffle).toBe(true);
      expect(b.queue.shuffle).toBe(true);
      expect(b.queue.repeat).toBe('all');
      // Shuffle order is current-first around the restored item.
      expect(b.queue.getPresentationOrder()[0]).toBe(b.queue.get().index);
    } finally {
      vi.useRealTimers();
    }
  });

  it('a legacy v1 snapshot restores with default policy', async () => {
    const map = new Map<string, string>();
    map.set(
      'musicpack.player.v1',
      JSON.stringify({
        v: 1,
        items: [
          { track: { id: 11, audio: { url: '/api/v1/tracks/11/audio' } }, releaseId: 9, albumId: 1, albumTitle: 'A', artist: 'X' },
        ],
        index: 0,
        positionSeconds: 4,
        volume: 0.7,
        normalizeMode: 'album',
      }),
    );
    const player = new PlayerController(createQueueStore(), {
      storage: { get: () => map.get('musicpack.player.v1') ?? null, set: () => undefined },
    });
    player.init();
    const m = player.model.get();
    expect(m.state).toBe('paused');
    expect(m.current?.track.id).toBe(11); // QueueItem fields survive verbatim
    expect(m.repeat).toBe('off'); // documented v1 defaults
    expect(m.shuffle).toBe(false);
  });

  it('clears persisted state on teardown', async () => {
    const map = new Map<string, string>();
    const storage = {
      get: () => map.get('k') ?? null,
      set: (v: string | null) => {
        if (v === null) map.delete('k');
        else map.set('k', v);
      },
    };
    const a = makeWithStorage(storage);
    vi.useFakeTimers({ toFake: ['setTimeout', 'Date'] });
    try {
      await a.player.playAlbum(release(9, ['T1']), 'Album', 'Artist');
      a.getBackend().emitPrimed();
      vi.advanceTimersByTime(2001); // past the persist throttle
      a.getBackend().emitPosition(); // persists
      expect(map.size).toBe(1);
      await a.player.teardown();
      expect(map.size).toBe(0);
    } finally {
      vi.useRealTimers();
    }
  });

  it('teardown stops playback, disposes the backend and clears player state', async () => {
    const { player, getBackend } = make();
    await player.playAlbum(release(9, ['T1']), 'Test Album', 'Artist');
    const b = getBackend();
    b.emitPrimed();
    await flush();
    expect(player.model.get().state).toBe('playing');
    await player.teardown();
    await flush();
    expect(b.closed).toBe(true);
    expect(player.model.get().state).toBe('idle');
    expect(player.model.get().current).toBeNull();
    expect(player.model.get().positionSeconds).toBe(0);
    expect(player.model.get().error).toBeUndefined();
  });

  // ---- M0 characterization: superseded loads & external queue mutation ----

  it('lets the newest command win when next fires during an unresolved load', async () => {
    const { player, queue, getBackend, setGate } = makeWithOpenGate();
    let releaseOpen = () => {};
    setGate(new Promise<void>((resolve) => { releaseOpen = resolve; }));

    const first = player.playAlbum(release(9, ['T1', 'T2', 'T3']), 'A', 'Artist');
    await flush();
    expect(player.model.get().state).toBe('loading');

    void player.next(); // supersedes; its own load parks on the same gate
    await flush();
    expect(queue.get().index).toBe(1);

    releaseOpen();
    await Promise.allSettled([first]);
    await flush(6);
    expect(queue.get().index).toBe(1);
    expect(player.model.get().current?.track.title).toBe('T2');
    expect(player.model.get().state).toBe('buffering');
    getBackend().emitPrimed();
    await flush();
    expect(player.model.get().state).toBe('playing');
  });

  it('lets a late previous() override a pending next() during loading', async () => {
    const { player, queue, getBackend, setGate } = makeWithOpenGate();
    let releaseOpen = () => {};
    setGate(new Promise<void>((resolve) => { releaseOpen = resolve; }));

    const first = player.playAlbum(release(9, ['T1', 'T2', 'T3']), 'A', 'Artist');
    await flush();
    void player.next();
    await flush();
    void player.previous(); // <3s into the song -> cursor back to index 0
    await flush();
    expect(queue.get().index).toBe(0);

    releaseOpen();
    await Promise.allSettled([first]);
    await flush(6);
    expect(queue.get().index).toBe(0);
    expect(player.model.get().current?.track.title).toBe('T1');
    expect(player.model.get().state).not.toBe('error');
  });

  it('discards a superseded album load when another playAlbum replaces it mid-open', async () => {
    const { player, queue, getBackend, setGate } = makeWithOpenGate();
    let releaseOpen = () => {};
    setGate(new Promise<void>((resolve) => { releaseOpen = resolve; }));

    const a = player.playAlbum(release(9, ['A1', 'A2'], 20), 'Album A', 'Artist');
    await flush();
    expect(getBackend().opened).toEqual(['/api/v1/tracks/1/audio']);

    const b = player.playAlbum(release(10, ['B1', 'B2'], 10, 100), 'Album B', 'Artist', 1);
    await flush();
    expect(getBackend().opened).toContain('/api/v1/tracks/102/audio'); // B start index 1

    releaseOpen();
    await Promise.allSettled([a, b]);
    await flush(6);
    expect(queue.get().items.map((i) => i.track.title)).toEqual(['B1', 'B2']);
    expect(queue.get().index).toBe(1);
    expect(player.model.get().current?.track.title).toBe('B2');
    expect(player.model.get().durationSeconds).toBeCloseTo(20, 1); // B's geometry
    expect(player.model.get().state).toBe('buffering');
  });

  it('stops playback when the queue is cleared externally while playing', async () => {
    const { player, queue, getBackend } = make();
    await player.playAlbum(release(9, ['T1', 'T2']), 'A', 'Artist');
    const b = getBackend();
    b.emitPrimed();
    await flush();
    expect(player.model.get().state).toBe('playing');

    queue.clear(); // external mutation (no mutating flag): the panel path
    await flush();
    expect(queue.get().items).toHaveLength(0);
    expect(player.model.get().state).toBe('idle');
    expect(b.paused).toBe(true); // pump stopped, audio cannot continue
  });

  it('advances to the following track when the playing item is removed externally', async () => {
    const { player, queue, getBackend } = make();
    await player.playAlbum(release(9, ['T1', 'T2', 'T3']), 'A', 'Artist');
    const b = getBackend();
    b.emitPrimed();
    await flush();

    queue.removeAt(0); // remove the CURRENT item (cursor clamps onto T2)
    await flush(6);
    expect(queue.get().items.map((i) => i.track.title)).toEqual(['T2', 'T3']);
    expect(queue.get().index).toBe(0);
    expect(player.model.get().current?.track.title).toBe('T2');
    expect(player.model.get().state).toBe('buffering');
    b.emitPrimed();
    await flush();
    expect(player.model.get().state).toBe('playing');
  });

  it('keeps playing untouched when a future item is removed externally', async () => {
    const { player, queue, getBackend } = make();
    await player.playAlbum(release(9, ['T1', 'T2', 'T3']), 'A', 'Artist');
    const b = getBackend();
    b.emitPrimed();
    await flush();
    const opens = b.opened.length;

    queue.removeAt(2); // a future item: current track id unchanged
    await flush(6);
    expect(queue.get().items).toHaveLength(2);
    expect(player.model.get().current?.track.title).toBe('T1');
    expect(player.model.get().state).toBe('playing');
    expect(b.opened.length).toBe(opens); // no reload was triggered
  });
});
