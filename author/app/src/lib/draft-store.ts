// The authoring draft store: in-memory editable state for the album being
// authored. Plain TS + the minimal store primitive, so the editing logic is
// unit-testable without the Svelte runtime (web-client convention). The GUI
// never mutates a half-created .mpack directory; a package is only produced
// from a validated draft at "Create MusicPack".

import { writable, type Writable } from './store';
import type {
  ArtworkEntry,
  AssetEntry,
  Draft,
  Identifiers,
  Identity,
  Medium,
  ReleaseEdition,
  SourceInfo,
  Track,
} from './types';

export type ChipStatus = 'ok' | 'warn' | 'idle';

export interface ChipState {
  audio: ChipStatus;
  metadata: ChipStatus;
  artwork: ChipStatus;
  identity: ChipStatus;
}

export interface DraftStore {
  draft: Writable<Draft | null>;
  busy: Writable<boolean>;
  error: Writable<string | null>;

  setDraft(draft: Draft): void;
  clear(): void;
  setBusy(b: boolean): void;
  setError(e: string | null): void;

  updateAlbum(fn: (album: Draft['album']) => void): void;
  updateRelease(fn: (release: ReleaseEdition) => void): void;
  updateIdentifiers(fn: (ids: Identifiers) => void): void;
  updateIdentity(fn: (id: Identity) => void): void;
  updateSource(fn: (src: SourceInfo) => void): void;
  updateMedium(discIndex: number, fn: (medium: Medium) => void): void;
  updateTrack(discIndex: number, trackIndex: number, patch: Partial<Track>): void;
  setArtwork(entries: ArtworkEntry[]): void;
  setAssets(kind: 'booklet' | 'lyrics' | 'extras', entries: AssetEntry[]): void;
}

function mutate(draft: Draft, fn: (d: Draft) => void): Draft {
  const next: Draft = structuredClone(draft);
  fn(next);
  return next;
}

export function createDraftStore(): DraftStore {
  const draft = writable<Draft | null>(null);
  const busy = writable<boolean>(false);
  const error = writable<string | null>(null);

  const withDraft = (fn: (d: Draft) => void): void => {
    const current = draft.get();
    if (current) draft.set(mutate(current, fn));
  };

  return {
    draft,
    busy,
    error,
    setDraft(d: Draft) {
      draft.set(structuredClone(d));
    },
    clear() {
      draft.set(null);
      error.set(null);
    },
    setBusy(b: boolean) {
      busy.set(b);
    },
    setError(e: string | null) {
      error.set(e);
    },

    updateAlbum(fn) {
      withDraft((d) => fn(d.album));
    },
    updateRelease(fn) {
      withDraft((d) => {
        if (!d.release) d.release = {};
        fn(d.release);
      });
    },
    updateIdentifiers(fn) {
      withDraft((d) => {
        if (!d.identifiers) d.identifiers = {};
        fn(d.identifiers);
      });
    },
    updateIdentity(fn) {
      withDraft((d) => {
        if (!d.identity) d.identity = {};
        fn(d.identity);
      });
    },
    updateSource(fn) {
      withDraft((d) => {
        if (!d.source) d.source = {};
        fn(d.source);
      });
    },
    updateMedium(discIndex: number, fn: (medium: Medium) => void) {
      withDraft((d) => {
        const medium = d.media[discIndex];
        if (medium) fn(medium);
      });
    },
    updateTrack(discIndex: number, trackIndex: number, patch: Partial<Track>) {
      withDraft((d) => {
        const medium = d.media[discIndex];
        const track = medium?.tracks[trackIndex];
        if (track) Object.assign(track, patch);
      });
    },
    setArtwork(entries: ArtworkEntry[]) {
      withDraft((d) => {
        d.artwork = structuredClone(entries);
      });
    },
    setAssets(kind: 'booklet' | 'lyrics' | 'extras', entries: AssetEntry[]) {
      withDraft((d) => {
        d[kind] = structuredClone(entries);
      });
    },
  };
}

// Footer chip status derived from the draft (instant, local). The
// authoritative validation still comes from `validate-draft`.
export function chipState(d: Draft): ChipState {
  const tracks = d.media.reduce((n, m) => n + m.tracks.length, 0);
  const metadataOk = d.album.title.trim().length > 0 && d.album.artists.length > 0;
  const conf = d.identity?.confidence;
  return {
    audio: tracks > 0 ? 'ok' : 'warn',
    metadata: metadataOk ? 'ok' : 'warn',
    artwork: d.artwork.length > 0 ? 'ok' : 'warn',
    identity:
      conf === 'exact' || conf === 'confirmed'
        ? 'ok'
        : conf === 'probable'
          ? 'warn'
          : 'idle',
  };
}
