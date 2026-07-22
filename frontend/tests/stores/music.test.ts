import { beforeEach, describe, expect, it, vi } from 'vitest';

const mocks = vi.hoisted(() => ({
  getLibrary: vi.fn(),
  getUserPlaylists: vi.fn(),
  addToPlaylist: vi.fn(),
  removeFromPlaylist: vi.fn(),
  reorderPlaylist: vi.fn(),
}));

vi.mock('../../src/api/music', () => mocks);

import { useMusicStore } from '../../src/stores/music';
import type { MusicMeta, PaginatedResponse } from '../../src/types/api';

interface Deferred<T> {
  promise: Promise<T>;
  resolve: (value: T) => void;
}

function deferred<T>(): Deferred<T> {
  let resolvePromise!: (value: T) => void;
  const promise = new Promise<T>((resolve) => {
    resolvePromise = resolve;
  });
  return { promise, resolve: resolvePromise };
}

function response(title: string): PaginatedResponse<MusicMeta> {
  return {
    items: [{
      music_id: title === 'new' ? 2 : 1,
      title,
      artist: '',
      album: '',
      genre: '',
      duration_sec: 0,
      file_hash: '',
      file_size: 0,
      content_type: 'audio/mpeg',
    }],
    total: 1,
    offset: 0,
    limit: 20,
  };
}

describe('music store', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    useMusicStore.setState({
      library: [],
      libraryTotal: 0,
      libraryLoading: false,
      libraryError: null,
      currentPlaylist: null,
      userPlaylists: [],
    });
  });

  it('keeps the newest library response when an older request finishes last', async () => {
    const oldRequest = deferred<PaginatedResponse<MusicMeta>>();
    const newRequest = deferred<PaginatedResponse<MusicMeta>>();
    mocks.getLibrary
      .mockReturnValueOnce(oldRequest.promise)
      .mockReturnValueOnce(newRequest.promise);

    const first = useMusicStore.getState().fetchLibrary(0, 20, 'old');
    const second = useMusicStore.getState().fetchLibrary(0, 20, 'new');
    newRequest.resolve(response('new'));
    await second;
    oldRequest.resolve(response('old'));
    await first;

    expect(useMusicStore.getState().library[0].title).toBe('new');
    expect(useMusicStore.getState().libraryLoading).toBe(false);
  });

  it('preserves the detailed API error in library state', async () => {
    mocks.getLibrary.mockRejectedValueOnce(new Error('数据库查询超时'));

    await useMusicStore.getState().fetchLibrary(0, 20);

    expect(useMusicStore.getState().libraryError).toBe('数据库查询超时');
    expect(useMusicStore.getState().libraryLoading).toBe(false);
  });
});
