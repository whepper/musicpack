import { describe, it, expect, vi, beforeEach } from 'vitest';
import {
  PlayerController,
  type Backend,
  type BackendEvents,
} from '../../app/src/lib/playback/controller';
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

function release(id: number, titles: string[], seconds = 10): ReleaseDetail {
  return {
    id,
    edition: '2016 Remaster',
    loudness: { algorithm: 'ITU-R BS.1770-5', albumLufs: -7.28, albumTruePeakDb: -4.19 },
    album: { id: 1, title: 'Test Album', artists: [] },
    media: [{ disc: 1, tracks: titles.map((t, i) => track(i + 1, t, seconds)) }],
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
  advanceGate: Promise<{ rate: number; channels: number; version: number; lengthSamples: number } | null> | null = null;
  plays = 0;
  seeks: number[] = [];

  constructor(_kind: 'musepack' | 'native', events: BackendEvents) {
    this.events = events;
  }

  async init() {}
  lengthFor(url: string): number {
    return this.lengthsByUrl.get(url) ?? LENGTH;
  }
  async open(url: string) {
    this.opened.push(url);
    this.rendered = 0;
    return { rate: RATE, channels: 2, version: 8, lengthSamples: this.lengthFor(url) };
  }
  async prepareNext(url: string) {
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
    this.events.onPrimed();
  }
  emitEos() {
    this.events.onEos();
  }
  emitBuffering() {
    this.events.onBuffering();
  }
  emitPosition() {
    this.events.onPosition();
  }
  emitError(msg: string) {
    this.events.onError(msg);
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

/** Flushes promise chains (state transitions land on .then microtasks). */
async function flush(times = 3): Promise<void> {
  for (let i = 0; i < times; i++) await Promise.resolve();
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

  it('clears stale per-index lengths when the queue is replaced', async () => {
    const { player, getBackend, lengthsByUrl } = make();
    // album A: 20 s tracks
    lengthsByUrl.set('/api/v1/tracks/1/audio', 20 * RATE);
    lengthsByUrl.set('/api/v1/tracks/2/audio', 20 * RATE);
    await player.playAlbum(release(9, ['A1', 'A2'], 20), 'Album A', 'Artist');
    const b = getBackend();
    b.emitPrimed();
    expect(player.model.get().durationSeconds).toBeCloseTo(40, 1);

    // album B: same track numbers, 10 s each, started at index 1 so track 1
    // is never opened (its length must come from B's metadata, not A's cache)
    lengthsByUrl.set('/api/v1/tracks/1/audio', 10 * RATE);
    lengthsByUrl.set('/api/v1/tracks/2/audio', 10 * RATE);
    await player.playAlbum(release(10, ['B1', 'B2'], 10), 'Album B', 'Artist', 1);
    b.emitPrimed();
    await flush();
    // 10 + 10, not 20 (A) + 10
    expect(player.model.get().durationSeconds).toBeCloseTo(20, 1);
    // a seek across B's track boundary uses B's track lengths
    await player.seek(15);
    await flush();
    expect(player.model.get().current?.track.title).toBe('B2');
    expect(player.model.get().positionSeconds).toBeCloseTo(15, 1);
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
});
