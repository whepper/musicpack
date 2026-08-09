import { describe, it, expect } from 'vitest';
import { createDraftStore, chipState } from '../../app/src/lib/draft-store';
import type { Draft } from '../../app/src/lib/types';

function sampleDraft(overrides: Partial<Draft> = {}): Draft {
  return {
    schema: 'musicpack-draft',
    version: 1,
    sourceRoot: '/music/Example Album',
    album: {
      title: 'Example Album',
      artists: [{ name: 'Example Artist', role: 'main' }],
      releaseType: 'album',
    },
    release: { releaseDate: '1987-01-01', edition: '1987 CD', catalogueNumber: 'EXA 1987' },
    identifiers: { musicbrainzReleaseId: '11111111-2222-3333-4444-555555555555' },
    identity: { source: 'local', confidence: 'none' },
    source: { type: 'cd-rip' },
    media: [
      {
        disc: 1,
        format: 'CD',
        tracks: [
          {
            track: 1,
            title: 'First Track',
            audioPath: '01 - First Track.mpc',
            codec: 'musepack-sv8',
            duration: 252.3,
          },
        ],
      },
    ],
    artwork: [{ role: 'front', path: 'cover.jpg' }],
    booklet: [],
    lyrics: [],
    extras: [],
    ...overrides,
  };
}

describe('draft store', () => {
  it('ingests an inspect result (deep-cloned, immutable)', () => {
    const store = createDraftStore();
    const d = sampleDraft();
    store.setDraft(d);
    expect(store.draft.get()).not.toBe(d);
    expect(store.draft.get()?.album.title).toBe('Example Album');
    d.album.title = 'mutated';
    expect(store.draft.get()?.album.title).toBe('Example Album');
  });

  it('edits release-group and release fields separately', () => {
    const store = createDraftStore();
    store.setDraft(sampleDraft());
    store.updateAlbum((a) => (a.title = 'Renamed'));
    store.updateRelease((r) => (r.edition = '2001 Remaster'));
    const d = store.draft.get()!;
    expect(d.album.title).toBe('Renamed');
    expect(d.release?.edition).toBe('2001 Remaster');
    expect(d.release?.catalogueNumber).toBe('EXA 1987');
    expect(d.source?.type).toBe('cd-rip');
  });

  it('updates a track on a given disc without touching the source path', () => {
    const store = createDraftStore();
    store.setDraft(sampleDraft());
    store.updateTrack(0, 0, { title: 'Edited Track' });
    const t = store.draft.get()!.media[0]!.tracks[0]!;
    expect(t.title).toBe('Edited Track');
    expect(t.audioPath).toBe('01 - First Track.mpc');
  });

  it('updates medium format and title', () => {
    const store = createDraftStore();
    store.setDraft(sampleDraft());
    store.updateMedium(0, (m) => {
      m.format = 'Digital';
      m.title = 'Digital edition';
    });
    const m = store.draft.get()!.media[0]!;
    expect(m.format).toBe('Digital');
    expect(m.title).toBe('Digital edition');
  });

  it('replaces artwork and asset lists', () => {
    const store = createDraftStore();
    store.setDraft(sampleDraft());
    store.setArtwork([
      { role: 'front', path: 'front.jpg' },
      { role: 'back', path: 'back.jpg' },
    ]);
    store.setAssets('lyrics', [{ path: 'song.lrc' }]);
    const d = store.draft.get()!;
    expect(d.artwork.map((a) => a.role)).toEqual(['front', 'back']);
    expect(d.lyrics).toEqual([{ path: 'song.lrc' }]);
  });

  it('applies an identified draft wholesale', () => {
    const store = createDraftStore();
    store.setDraft(sampleDraft());
    const identified = sampleDraft();
    identified.identity = { source: 'musicbrainz', confidence: 'exact' };
    identified.album.releaseType = 'compilation';
    store.setDraft(identified);
    const d = store.draft.get()!;
    expect(d.identity?.confidence).toBe('exact');
    expect(d.album.releaseType).toBe('compilation');
  });
});

describe('chipState', () => {
  it('reports ok for a complete draft', () => {
    const chips = chipState(sampleDraft());
    expect(chips.audio).toBe('ok');
    expect(chips.metadata).toBe('ok');
    expect(chips.artwork).toBe('ok');
    expect(chips.identity).toBe('idle'); // local/none
  });

  it('flags missing title/artist', () => {
    const chips = chipState(sampleDraft({ album: { title: '', artists: [] } }));
    expect(chips.metadata).toBe('warn');
  });

  it('reports identity ok only for exact/confirmed', () => {
    expect(
      chipState(sampleDraft({ identity: { source: 'musicbrainz', confidence: 'exact' } })).identity,
    ).toBe('ok');
    expect(
      chipState(sampleDraft({ identity: { source: 'musicbrainz', confidence: 'confirmed' } })).identity,
    ).toBe('ok');
    expect(
      chipState(sampleDraft({ identity: { source: 'musicbrainz', confidence: 'probable' } })).identity,
    ).toBe('warn');
  });
});
