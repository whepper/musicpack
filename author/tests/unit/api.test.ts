import { describe, it, expect, vi } from 'vitest';
import { AuthorApi, type InvokeFn } from '../../app/src/lib/api';
import type { Draft } from '../../app/src/lib/types';

function draft(): Draft {
  return {
    schema: 'musicpack-draft',
    version: 1,
    sourceRoot: '/music',
    album: { title: 'A', artists: [{ name: 'B' }] },
    media: [{ disc: 1, tracks: [{ track: 1, title: 'T', audioPath: '1.mpc' }] }],
    artwork: [],
    booklet: [],
    lyrics: [],
    extras: [],
  };
}

function makeApi(calls: unknown[]) {
  const invoke: InvokeFn = async (cmd, args) => {
    calls.push({ cmd, args });
    return {};
  };
  const plugins = {
    pickDirectory: vi.fn(async () => '/picked'),
    pickImageFile: vi.fn(async () => null),
    pickOutputDirectory: vi.fn(async () => null),
    revealInFinder: vi.fn(async () => undefined),
  };
  return { api: new AuthorApi(invoke, plugins), plugins };
}

describe('AuthorApi command surface', () => {
  it('inspects an album by path', async () => {
    const calls: unknown[] = [];
    const { api } = makeApi(calls);
    await api.inspectAlbum('/album');
    expect(calls[0]).toEqual({ cmd: 'inspect_album', args: { path: '/album' } });
  });

  it('validates a draft as serialized JSON', async () => {
    const calls: unknown[] = [];
    const { api } = makeApi(calls);
    const d = draft();
    await api.validateDraft(d);
    expect(calls[0]).toEqual({
      cmd: 'validate_draft',
      args: { draftJson: JSON.stringify(d) },
    });
  });

  it('identifies by mbid (nulls for unused options)', async () => {
    const calls: unknown[] = [];
    const { api } = makeApi(calls);
    await api.identifyDraft(draft(), { mbid: 'aaaa' });
    expect(calls[0]).toEqual({
      cmd: 'identify_draft',
      args: { draftJson: expect.any(String), mbid: 'aaaa', barcode: null, mbJson: null },
    });
  });

  it('passes mbJson through for an offline candidate apply', async () => {
    const calls: unknown[] = [];
    const { api } = makeApi(calls);
    await api.identifyDraft(draft(), { mbJson: '{"id":"x"}' });
    expect(calls[0]).toMatchObject({ cmd: 'identify_draft', args: { mbJson: '{"id":"x"}' } });
  });

  it('creates a package at the chosen output', async () => {
    const calls: unknown[] = [];
    const { api } = makeApi(calls);
    await api.createPackage(draft(), '/out.mpack');
    expect(calls[0]).toMatchObject({
      cmd: 'create_package',
      args: { outputDir: '/out.mpack' },
    });
  });

  it('verifies a package and reads an image', async () => {
    const calls: unknown[] = [];
    const { api } = makeApi(calls);
    await api.verifyPackage('/out.mpack');
    await api.readImage('/art.png');
    expect(calls[0]).toEqual({ cmd: 'verify_package', args: { path: '/out.mpack' } });
    expect(calls[1]).toEqual({ cmd: 'read_image', args: { path: '/art.png' } });
  });

  it('delegates dialogs and reveal to the plugin facade', async () => {
    const calls: unknown[] = [];
    const { api, plugins } = makeApi(calls);
    expect(await api.pickDirectory()).toBe('/picked');
    expect(plugins.pickDirectory).toHaveBeenCalledOnce();
    await api.revealInFinder('/x');
    expect(plugins.revealInFinder).toHaveBeenCalledWith('/x');
  });
});
