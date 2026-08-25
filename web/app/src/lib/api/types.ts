// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

// Typed views of the MusicPack HTTP API v1 resources (specs/musicpack-api-v1.md).

export interface ArtistRef {
  id: number;
  name: string;
  role?: string;
}

export interface ArtworkRef {
  id: number;
  url: string;
  mimeType?: string;
  kind?: string;
  role?: string;
}

export interface AlbumSummary {
  id: number;
  title: string;
  releaseType?: string;
  originalReleaseDate?: string;
  genres?: string[];
  artists: ArtistRef[];
  releaseCount: number;
  artwork?: ArtworkRef;
}

export interface AlbumPage {
  albums: AlbumSummary[];
  limit: number;
  offset: number;
  total: number;
}

export interface ReleaseSummary {
  id: number;
  edition?: string;
  releaseDate?: string;
  country?: string;
  label?: string;
  catalogueNumber?: string;
  barcode?: string;
  mbid?: string;
  identitySource?: string;
  identityConfidence?: string;
  trackCount: number;
  media: string[];
  artwork?: ArtworkRef;
  packageStatus?: string;
  verifyStatus?: string;
}

export interface AlbumDetail {
  album: {
    id: number;
    title: string;
    releaseType?: string;
    originalReleaseDate?: string;
    mbid?: string;
    genres?: string[];
    artists: ArtistRef[];
  };
  releases: ReleaseSummary[];
}

export interface Loudness {
  lufs: number;
  truePeakDb: number;
}

export interface AlbumLoudness {
  algorithm?: string;
  albumLufs: number;
  albumTruePeakDb: number;
}

export interface CodecInfo {
  codec: string;
  mimeType: string;
  streamVersion?: number;
  sampleRate?: number;
  channels?: number;
}

export interface AudioRef {
  id: number;
  size: number;
  sha256?: string;
  url: string;
}

export interface WaveformRef {
  version: number;
  intervalMs: number;
  encoding: string;
  floorDb: number;
  points: number;
  url: string;
}

export interface Track {
  id: number;
  number: number;
  title: string;
  artists: ArtistRef[];
  isrc?: string;
  duration?: number;
  loudness?: Loudness;
  codec: CodecInfo;
  audio: AudioRef;
  /// Optional waveform envelope (see specs/musicpack-waveform-v1.md).
  /// `null` (or undefined) means the track has no waveform — the player
  /// falls back to the linear `<input type="range">` seek control.
  waveform?: WaveformRef | null;
  /// Optional alternate audio representations (Phase 3). The default
  /// remains `audio`; the client currently plays only the default, so
  /// this field is display/selection metadata. Omitted when empty.
  representations?: RepresentationRef[];
}

export interface RepresentationRef {
  id: number;
  size: number;
  url: string;
  codec: CodecInfo;
  label?: string;
}

export interface MediaDisc {
  disc: number;
  format?: string;
  title?: string;
  tracks: Track[];
}

export interface AssetRef {
  id: number;
  kind: string;
  role?: string;
  mimeType: string;
  url: string;
}

export interface ReleaseDetail {
  id: number;
  edition?: string;
  releaseDate?: string;
  country?: string;
  label?: string;
  catalogueNumber?: string;
  barcode?: string;
  mbid?: string;
  identitySource?: string;
  identityConfidence?: string;
  sourceType?: string;
  sourceStore?: string;
  sourceId?: string;
  provenanceTool?: string;
  provenanceToolVersion?: string;
  notes?: string;
  packageStatus?: string;
  verifyStatus?: string;
  loudness?: AlbumLoudness;
  album: {
    id: number;
    title: string;
    releaseType?: string;
    originalReleaseDate?: string;
    mbid?: string;
    artists: ArtistRef[];
  };
  media: MediaDisc[];
  artwork: ArtworkRef[];
  assets: AssetRef[];
}

export interface ArtistSummary {
  id: number;
  name: string;
  albumCount: number;
}

export interface ArtistPage {
  artists: ArtistSummary[];
  limit: number;
  offset: number;
  total: number;
}

export interface ArtistDetail {
  id: number;
  name: string;
  albums: Array<{
    id: number;
    title: string;
    releaseType?: string;
    originalReleaseDate?: string;
  }>;
}

export interface SessionInfo {
  id?: number;
  createdAt?: string;
  expiresAt?: string;
}

export interface LibraryStatus {
  scan: {
    running: number;
    startedAt?: string;
    finishedAt?: string;
    packagesScanned?: number;
    added?: number;
    updated?: number;
    removed?: number;
    invalid?: number;
  };
  verify: {
    running: number;
    startedAt?: string;
    finishedAt?: string;
    packagesVerified?: number;
    passed?: number;
    warnings?: number;
    failed?: number;
  };
}
