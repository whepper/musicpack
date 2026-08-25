import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { ApiClient } from '../../app/src/lib/api/client';
import { ApiError, NetworkError } from '../../app/src/lib/api/errors';

function jsonResponse(status: number, body: unknown): Response {
  return new Response(body === null ? null : JSON.stringify(body), {
    status,
    headers: { 'Content-Type': 'application/json' },
  });
}

describe('ApiClient', () => {
  beforeEach(() => {
    vi.stubGlobal('fetch', vi.fn());
  });
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it('parses album pages', async () => {
    const fetchMock = vi.fn().mockResolvedValue(
      jsonResponse(200, { albums: [{ id: 1, title: 'X', artists: [], releaseCount: 1 }], total: 1, limit: 50, offset: 0 }),
    );
    vi.stubGlobal('fetch', fetchMock);
    const api = new ApiClient();
    const page = await api.albums({ limit: 50 });
    expect(page.total).toBe(1);
    expect(fetchMock.mock.calls[0]?.[0]).toBe('/api/v1/albums?limit=50');
  });

  it('maps typed server error codes to friendly ApiErrors', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn().mockResolvedValue(jsonResponse(404, { error: { code: 'not_found', message: 'Track not found' } })),
    );
    const api = new ApiClient();
    await expect(api.album('999')).rejects.toMatchObject({
      code: 'not_found',
      status: 404,
      message: expect.stringContaining('no longer in the collection'),
    });
  });

  it('falls back to internal for a non-JSON error body', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(new Response('boom', { status: 500 })));
    const api = new ApiClient();
    await expect(api.albums()).rejects.toBeInstanceOf(ApiError);
  });

  it('throws NetworkError when fetch rejects', async () => {
    vi.stubGlobal('fetch', vi.fn().mockRejectedValue(new TypeError('fetch failed')));
    const api = new ApiClient();
    await expect(api.albums()).rejects.toBeInstanceOf(NetworkError);
  });

  it('sends the bearer token when a token provider is configured', async () => {
    const fetchMock = vi.fn().mockResolvedValue(jsonResponse(200, { albums: [], total: 0, limit: 50, offset: 0 }));
    vi.stubGlobal('fetch', fetchMock);
    const api = new ApiClient({ token: () => 'mpk_test' });
    await api.albums();
    const headers = (fetchMock.mock.calls[0]?.[1] as RequestInit).headers as Headers;
    expect(headers.get('Authorization')).toBe('Bearer mpk_test');
  });

  it('notifies onUnauthorized on 401', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn().mockResolvedValue(jsonResponse(401, { error: { code: 'unauthorized', message: 'x' } })),
    );
    const api = new ApiClient();
    const spy = vi.fn();
    api.onUnauthorized = spy;
    await expect(api.albums()).rejects.toBeInstanceOf(ApiError);
    expect(spy).toHaveBeenCalledOnce();
  });

  it('session exchange posts the token and uses cookies by default', async () => {
    const fetchMock = vi.fn().mockResolvedValue(jsonResponse(200, { status: 'authenticated' }));
    vi.stubGlobal('fetch', fetchMock);
    const api = new ApiClient();
    await api.createSession('mpk_secret');
    const [url, init] = fetchMock.mock.calls[0] as [string, RequestInit];
    expect(url).toBe('/api/v1/session');
    expect(init.method).toBe('POST');
    expect(JSON.parse(init.body as string)).toEqual({ token: 'mpk_secret' });
  });

  it('surfaces content hashes on representations, waveforms and assets (offline integrity)', async () => {
    const sha = 'a'.repeat(64);
    const fetchMock = vi.fn().mockResolvedValue(
      jsonResponse(200, {
        id: 7,
        album: { id: 1, title: 'X', artists: [] },
        media: [
          {
            disc: 1,
            tracks: [
              {
                id: 55,
                number: 1,
                title: 'T',
                artists: [],
                codec: { codec: 'musepack-sv8', mimeType: 'audio/musepack' },
                audio: { id: 90, size: 10, sha256: sha, url: '/api/v1/tracks/55/audio' },
                representations: [
                  {
                    id: 91,
                    size: 20,
                    sha256: sha,
                    url: '/api/v1/tracks/55/representations/91/audio',
                    codec: { codec: 'flac', mimeType: 'audio/flac' },
                  },
                ],
                waveform: {
                  version: 1,
                  intervalMs: 100,
                  encoding: 'peak-rms-u8',
                  floorDb: -60,
                  points: 4,
                  sha256: sha,
                  url: '/api/v1/tracks/55/waveform',
                },
              },
            ],
          },
        ],
        artwork: [{ id: 7, kind: 'artwork', mimeType: 'image/jpeg', sha256: sha, url: '/api/v1/assets/7' }],
        assets: [],
      }),
    );
    vi.stubGlobal('fetch', fetchMock);
    const api = new ApiClient();
    const rel = await api.release(7);
    const track = rel.media[0]!.tracks[0]!;
    expect(track.representations?.[0]?.sha256).toBe(sha);
    expect(track.waveform?.sha256).toBe(sha);
    expect(rel.artwork[0]?.sha256).toBe(sha);
  });
});
