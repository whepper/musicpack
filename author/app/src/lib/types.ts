// Types for the MusicPack Author draft model and the Tauri command surface.
//
// The draft is application state (not a MusicPack format): it mirrors the
// .mpack v1 logical hierarchy but references files under `sourceRoot` and
// keeps release, source and identity separate, exactly as the spec requires.
// Everything here maps 1:1 onto the `musicpack` CLI JSON modes.

export interface Artist {
  name: string;
  role?: string;
}

export interface AlbumGroup {
  title: string;
  artists: Artist[];
  releaseType?: string;
  originalReleaseDate?: string;
  genres?: string[];
}

export interface ReleaseEdition {
  releaseDate?: string;
  edition?: string;
  country?: string;
  label?: string;
  catalogueNumber?: string;
  notes?: string;
}

export interface Identifiers {
  musicbrainzReleaseGroupId?: string;
  musicbrainzReleaseId?: string;
  barcode?: string;
}

export type IdentitySource = 'musicbrainz' | 'store' | 'local';
export type Confidence = 'exact' | 'confirmed' | 'probable' | 'none';

export interface Identity {
  source?: IdentitySource;
  confidence?: Confidence;
}

export interface SourceInfo {
  type?: string;
  store?: string;
  sourceId?: string;
}

export interface TrackSource {
  store?: string;
  trackId?: string;
}

export interface SourceAudio {
  codec?: string;
  md5?: string;
}

export interface TrackIdentifiers {
  isrc?: string;
  musicbrainzTrackId?: string;
  musicbrainzRecordingId?: string;
}

export interface Track {
  track: number;
  title: string;
  artists?: Artist[];
  identifiers?: TrackIdentifiers;
  source?: TrackSource;
  sourceAudio?: SourceAudio;
  duration?: number;
  codec?: string;
  streamVersion?: number;
  sampleRate?: number;
  channels?: number;
  /** source file path relative to the draft's sourceRoot */
  audioPath: string;
}

export interface Medium {
  disc: number;
  format?: string;
  title?: string;
  tracks: Track[];
}

export interface ArtworkEntry {
  role: string;
  path?: string;
  embedded?: boolean;
  sourceAudio?: string;
  mime?: string;
}

export interface AssetEntry {
  path: string;
}

/** Sonic analysis state carried by the authoring draft (application state,
 * not part of the .mpack manifest). The completed document lives outside the
 * package until build; `path` points at it for create_package to attach. */
export interface SonicAnalysis {
  status: 'not_analysed' | 'pending' | 'ready' | 'ready-with-warnings' | 'error';
  profile?: string;
  path?: string;
  tracksAnalysed?: number;
  tracksTotal?: number;
  warnings?: string[];
  error?: string;
}

export interface Draft {
  schema: 'musicpack-draft';
  version: 1;
  sourceRoot: string;
  album: AlbumGroup;
  release?: ReleaseEdition;
  identifiers?: Identifiers;
  identity?: Identity;
  source?: SourceInfo;
  media: Medium[];
  artwork: ArtworkEntry[];
  booklet: AssetEntry[];
  lyrics: AssetEntry[];
  extras: AssetEntry[];
  sonicAnalysis?: SonicAnalysis;
}

export const RELEASE_TYPES = [
  'album',
  'ep',
  'single',
  'maxi-single',
  'compilation',
  'soundtrack',
  'live-album',
  'remix-album',
  'box-set',
  'other',
] as const;

export const MEDIUM_FORMATS = [
  'CD',
  'SACD',
  'Vinyl',
  'Cassette',
  'Digital',
  'Blu-ray Audio',
  'DVD-Audio',
  'Other',
] as const;

export const ARTWORK_ROLES = [
  'front',
  'back',
  'medium',
  'booklet-page',
  'other',
] as const;

export const SOURCE_TYPES = [
  'cd-rip',
  'digital-download',
  'vinyl-rip',
  'tape-rip',
  'other',
] as const;

// ---- command results ------------------------------------------------------

export interface ValidationResult {
  ok: boolean;
  errors: string[];
  warnings: string[];
}

export interface IdentifyCandidate {
  releaseId?: string;
  releaseGroupId?: string;
  title?: string;
  artist?: string;
  date?: string;
  country?: string;
  barcode?: string;
  confidence: Confidence;
}

export type IdentifyResult =
  | { kind: 'candidates'; candidates: IdentifyCandidate[] }
  | { kind: 'applied'; draft: Draft; confidence: Confidence; applied: boolean };

export interface CreateResult {
  ok: boolean;
  outputPath?: string;
  verify?: { errors: number; warnings: number };
  error?: { code?: string; message?: string };
}

export interface ReadImageResult {
  mime: string;
  dataBase64: string;
}

/** Result of a sonic analysis run. `cancelled` is distinct from failure. */
export interface SonicResult {
  ok: boolean;
  cancelled?: boolean;
  profile?: string;
  outputPath?: string;
  sha256?: string;
  tracks?: number;
  contributing?: number;
}

/** A progress event emitted during sonic analysis. */
export interface SonicProgress {
  event: 'model' | 'track' | 'album' | 'done' | 'error' | 'cancelled';
  state?: string;
  path?: string;
  done?: number;
  total?: number;
  disc?: number;
  track?: number;
  status?: 'ok' | 'no-embedding' | 'error';
  message?: string;
  contributing?: number;
  sha256?: string;
}

export interface BackendInfo {
  musicpackVersion: string;
  authorApi: number;
  location: 'bundled' | 'development';
}

export interface IdentifyOptions {
  mbid?: string;
  barcode?: string;
  mbJson?: string;
}
