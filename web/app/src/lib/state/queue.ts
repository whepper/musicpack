// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

// Web queue store (M3): a thin adapter over the platform-independent
// QueueModel (player-core/src/queue.ts). All semantics live in the core;
// this file adds only the web item construction from API types.
//
// QueueItem = core PlaybackItem plus the API `Track` verbatim and release
// identity. The core fields are the player-facing identity/source/metadata
// view; the `track` field remains until the orchestrator extraction (M4)
// moves all readers onto the core shape.

import { createQueueModel, type QueueModel } from '../../../../player-core/src/queue';import type { PlaybackItem } from '../../../../player-core/src/types';
import type { AlbumLoudness, ReleaseDetail, Track } from '../api/types';

export interface QueueItem extends PlaybackItem {
  track: Track;
  releaseId: number;
  albumId: number;
}

export interface QueueState {
  items: QueueItem[];
  /** Index of the current item (-1 when the queue is empty). */
  index: number;
}

/** Builds the play sequence for a release in media/disc-major, track order. */
export function tracksOfRelease(release: ReleaseDetail): Track[] {
  const out: Track[] = [];
  for (const disc of [...release.media].sort((a, b) => a.disc - b.disc)) {
    for (const track of [...disc.tracks].sort((a, b) => a.number - b.number)) {
      out.push(track);
    }
  }
  return out;
}

/** Builds a single web QueueItem for one track of a release. Shared by the
 *  album builders and the per-track add-to-queue action so item construction
 *  (the future representation-selection boundary) lives in exactly one place. */
export function itemForTrack(
  release: ReleaseDetail,
  track: Track,
  title: string,
  artist: string,
): QueueItem {
  const artworkUrl = release.artwork[0]?.url;
  return {
    // core PlaybackItem fields (identity, source, policy metadata)
    id: `t${track.id}`,
    trackId: track.id,
    source: {
      kind: 'http-range' as const,
      url: track.audio.url,
      byteSize: track.audio.size,
    },
    durationHintSeconds: track.duration,
    title: track.title,
    artist,
    albumTitle: title,
    edition: release.edition,
    artworkUrl,
    loudness: track.loudness,
    albumLoudness: release.loudness,
    codec: track.codec.codec,
    mimeType: track.codec.mimeType,
    // web-specific fields
    track,
    releaseId: release.id,
    albumId: release.album.id,
  };
}

function itemsFor(release: ReleaseDetail, title: string, artist: string): QueueItem[] {
  return tracksOfRelease(release).map((track) =>
    itemForTrack(release, track, title, artist),
  );
}

/** Pure builder (M4): constructs web QueueItems for a release without
 *  installing them — used by the playback facade's playAlbum. */
export function itemsForRelease(
  release: ReleaseDetail,
  title: string,
  artist: string,
): QueueItem[] {
  return itemsFor(release, title, artist);
}

/** The web queue store: QueueModel methods (delegated verbatim) plus the
 *  web-shaped playAlbum builder. NOTE: `{...model}` would snapshot the
 *  model's getters as static values (repeat/shuffle would freeze at their
 *  initial state), so the policy surface is forwarded explicitly. */
export function createQueueStore() {
  // Hosts own randomness (core purity law 2): the web passes Math.random.
  const model = createQueueModel<QueueItem>({ rng: Math.random });
  return {
    ...model,
    get repeat() {
      return model.repeat;
    },
    setRepeat(mode: 'off' | 'one' | 'all'): void {
      model.setRepeat(mode);
    },
    get shuffle() {
      return model.shuffle;
    },
    setShuffle(on: boolean): void {
      model.setShuffle(on);
    },
    getPresentationOrder(): number[] {
      return model.getPresentationOrder();
    },
    /** Play the given release from `startIndex` (default first track). */
    playAlbum(
      release: ReleaseDetail,
      title: string,
      artist: string,
      startIndex = 0,
    ): QueueItem {
      const items = itemsFor(release, title, artist);
      if (items.length === 0) throw new Error('This release has no playable tracks.');
      return model.playSequence(items, startIndex);
    },
    /** Append a single item to the end of the current queue. */
    addItem(item: QueueItem): void {
      model.enqueue(item);
    },
    /** Append all tracks of a release to the end of the current queue. */
    addAlbum(
      release: ReleaseDetail,
      title: string,
      artist: string,
    ): void {
      model.enqueueMany(itemsFor(release, title, artist));
    },
  };
}

export type QueueStore = ReturnType<typeof createQueueStore>;
