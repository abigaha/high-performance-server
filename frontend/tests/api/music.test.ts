import { describe, it, expect, vi, beforeEach } from 'vitest';

const mockFetch = vi.fn();
globalThis.fetch = mockFetch;

beforeEach(() => {
  mockFetch.mockReset();
  localStorage.setItem('token', 'test-token');
});

describe('music API', () => {
  it('getLibrary returns paginated music', async () => {
    const items = [
      { music_id: 1, title: 'Song A', artist: 'Artist 1', album: 'Album 1', genre: 'Pop', duration_sec: 200, file_hash: 'a1', file_size: 1000, content_type: 'audio/mpeg' },
    ];
    mockFetch.mockResolvedValueOnce({
      ok: true,
      json: async () => ({ items, total: 1, offset: 0, limit: 20 }),
    });

    const { getLibrary } = await import('../../src/api/music');
    const res = await getLibrary(0, 20);

    expect(mockFetch).toHaveBeenCalledWith(
      expect.stringContaining('/api/music/library'),
      expect.anything(),
    );
    expect(res.items).toHaveLength(1);
    expect(res.items[0].title).toBe('Song A');
  });

  it('getUserPlaylists returns playlists', async () => {
    const playlists = [{ id: 1, name: 'My Playlist', description: '', item_count: 3, created_at: '2024-01-01' }];
    mockFetch.mockResolvedValueOnce({
      ok: true,
      json: async () => ({ playlists }),
    });

    const { getUserPlaylists } = await import('../../src/api/music');
    const res = await getUserPlaylists(1);

    expect(res).toHaveLength(1);
    expect(res[0].name).toBe('My Playlist');
  });

  it('getPlaylistItems returns items from the response wrapper', async () => {
    const items = [{
      id: 10,
      music_id: 2,
      title: 'Song B',
      artist: 'Artist 2',
      file_hash: 'b2',
      sort_order: 0,
      added_at: '2024-01-01',
    }];
    mockFetch.mockResolvedValueOnce({
      ok: true,
      json: async () => ({ playlist_id: 1, items }),
    });

    const { getPlaylistItems } = await import('../../src/api/music');
    const res = await getPlaylistItems(1);

    expect(res).toEqual(items);
  });

  it('reorderPlaylist sends music_ids expected by the server', async () => {
    mockFetch.mockResolvedValueOnce({
      ok: true,
      json: async () => ({ message: '排序已更新' }),
    });

    const { reorderPlaylist } = await import('../../src/api/music');
    await reorderPlaylist(1, [3, 2, 1]);

    expect(mockFetch).toHaveBeenCalledWith(
      expect.stringContaining('/api/playlists/1/items/reorder'),
      expect.objectContaining({
        method: 'PUT',
        body: JSON.stringify({ music_ids: [3, 2, 1] }),
      }),
    );
  });
});
