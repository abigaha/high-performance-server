import { beforeEach, describe, expect, it, vi } from 'vitest';

const mocks = vi.hoisted(() => ({
  getLibrary: vi.fn(),
  getUserPlaylists: vi.fn(),
  createPlaylist: vi.fn(),
  addToPlaylist: vi.fn(),
  removeFromPlaylist: vi.fn(),
  reorderPlaylist: vi.fn(),
  renamePlaylist: vi.fn(),
  deletePlaylist: vi.fn(),
}));

vi.mock('../../src/api/music', () => mocks);

import { useMusicStore } from '../../src/stores/music';
import { usePlayerStore } from '../../src/stores/player';
import type { MusicMeta, PaginatedResponse, Playlist } from '../../src/types/api';

interface Deferred<T> {
  promise: Promise<T>;
  resolve: (value: T) => void;
  reject: (reason: unknown) => void;
}

function deferred<T>(): Deferred<T> {
  let resolvePromise!: (value: T) => void;
  let rejectPromise!: (reason: unknown) => void;
  const promise = new Promise<T>((resolve, reject) => {
    resolvePromise = resolve;
    rejectPromise = reject;
  });
  return { promise, resolve: resolvePromise, reject: rejectPromise };
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
    usePlayerStore.getState().reset();
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

  it('updates playlist state immutably only after rename succeeds', async () => {
    const pending = deferred<{ id: number; user_id: number; name: string; description: string; item_count: number; created_at: string }>();
    mocks.renamePlaylist.mockReturnValueOnce(pending.promise);
    useMusicStore.setState({ userPlaylists: [{ id: 1, name: 'Old', description: 'Original description', itemCount: 2 }] });
    const before = useMusicStore.getState().userPlaylists;

    const operation = useMusicStore.getState().renamePlaylist(1, 'New', 'Desc');
    expect(useMusicStore.getState().userPlaylists).toBe(before);
    pending.resolve({ id: 1, user_id: 7, name: 'New', description: 'Desc', item_count: 9, created_at: 'now' });
    await operation;

    expect(useMusicStore.getState().userPlaylists).not.toBe(before);
    expect(useMusicStore.getState().userPlaylists).toEqual([{
      id: 1,
      user_id: 7,
      name: 'New',
      description: 'Desc',
      item_count: 9,
      itemCount: 9,
      created_at: 'now',
    }]);
  });

  it('preserves complete server playlist records when fetching summaries', async () => {
    mocks.getUserPlaylists.mockResolvedValueOnce([{
      id: 7,
      user_id: 4,
      name: 'Described playlist',
      description: 'Keep this server description',
      item_count: 2,
      created_at: 'now',
    }]);

    await useMusicStore.getState().fetchPlaylists(4);

    expect(useMusicStore.getState().userPlaylists).toEqual([{
      id: 7,
      user_id: 4,
      name: 'Described playlist',
      description: 'Keep this server description',
      item_count: 2,
      itemCount: 2,
      created_at: 'now',
    }]);
  });

  it('preserves an empty server description without replacing it with the list index', async () => {
    mocks.getUserPlaylists.mockResolvedValueOnce([{
      id: 8,
      user_id: 4,
      name: 'Empty description playlist',
      description: '',
      item_count: 0,
      created_at: 'now',
    }]);

    await useMusicStore.getState().fetchPlaylists(4);

    expect(useMusicStore.getState().userPlaylists).toEqual([{
      id: 8,
      user_id: 4,
      name: 'Empty description playlist',
      description: '',
      item_count: 0,
      itemCount: 0,
      created_at: 'now',
    }]);
  });

  it('keeps an explicit empty server description through a successful rename', async () => {
    const original: Playlist = {
      id: 7,
      user_id: 4,
      name: 'Described playlist',
      description: 'Keep this server description',
      item_count: 2,
      created_at: 'created-at',
    };
    const renamed: Playlist = {
      ...original,
      name: 'Renamed playlist',
      description: '',
      item_count: 3,
      created_at: 'updated-at',
    };
    mocks.getUserPlaylists.mockResolvedValueOnce([original]);
    mocks.renamePlaylist.mockResolvedValueOnce(renamed);

    await useMusicStore.getState().fetchPlaylists(original.user_id);
    await useMusicStore.getState().renamePlaylist(original.id, renamed.name, original.description);

    expect(useMusicStore.getState().userPlaylists).toEqual([{
      ...renamed,
      description: '',
      itemCount: renamed.item_count,
    }]);
  });

  it('does not clear a nonempty description when rename fails', async () => {
    const original = {
      id: 7,
      user_id: 4,
      name: 'Described playlist',
      description: 'Keep this server description',
      item_count: 2,
      itemCount: 2,
      created_at: 'created-at',
    };
    useMusicStore.setState({ userPlaylists: [original] });
    mocks.renamePlaylist.mockRejectedValueOnce(new Error('rename failed'));

    await expect(useMusicStore.getState().renamePlaylist(original.id, 'Rejected rename', original.description))
      .rejects.toThrow('rename failed');

    expect(useMusicStore.getState().userPlaylists).toEqual([original]);
  });

  it('does not let a session-stale rename replace a new nonempty description', async () => {
    const oldRename = deferred<Playlist>();
    const oldPlaylist = {
      id: 7,
      user_id: 4,
      name: 'Old session',
      description: 'Old nonempty description',
      item_count: 1,
      itemCount: 1,
      created_at: 'old-created-at',
    };
    const newPlaylist = {
      id: 7,
      user_id: 9,
      name: 'New session',
      description: 'New nonempty description',
      item_count: 4,
      itemCount: 4,
      created_at: 'new-created-at',
    };
    mocks.renamePlaylist.mockReturnValueOnce(oldRename.promise);
    useMusicStore.setState({ userPlaylists: [oldPlaylist] });

    const renaming = useMusicStore.getState().renamePlaylist(7, 'Stale rename', oldPlaylist.description);
    useMusicStore.getState().reset();
    useMusicStore.setState({ userPlaylists: [newPlaylist] });
    oldRename.resolve({
      id: 7,
      user_id: 4,
      name: 'Stale rename',
      description: oldPlaylist.description,
      item_count: 1,
      created_at: oldPlaylist.created_at,
    });
    await renaming;

    expect(useMusicStore.getState().userPlaylists).toEqual([newPlaylist]);
  });

  it('creates a playlist locally after the API succeeds without refetching it', async () => {
    const pending = deferred<Playlist>();
    mocks.createPlaylist.mockReturnValueOnce(pending.promise);
    useMusicStore.setState({ userPlaylists: [{ id: 1, name: 'Existing', description: 'Existing description', itemCount: 2 }] });
    const before = useMusicStore.getState().userPlaylists;

    const operation = useMusicStore.getState().createPlaylist(4, 'New list');
    expect(useMusicStore.getState().userPlaylists).toBe(before);
    pending.resolve({ id: 9, user_id: 4, name: 'New list', description: 'Created description', item_count: 0, created_at: 'now' });
    await operation;

    expect(useMusicStore.getState().userPlaylists).toEqual([
      { id: 1, name: 'Existing', description: 'Existing description', itemCount: 2 },
      {
        id: 9,
        user_id: 4,
        name: 'New list',
        description: 'Created description',
        item_count: 0,
        itemCount: 0,
        created_at: 'now',
      },
    ]);
    expect(mocks.getUserPlaylists).not.toHaveBeenCalled();
  });

  it('does not change playlist state when deletion fails', async () => {
    const playlists = [{ id: 1, name: 'Keep', itemCount: 2 }];
    useMusicStore.setState({ userPlaylists: playlists });
    mocks.deletePlaylist.mockRejectedValueOnce(new Error('delete failed'));

    await expect(useMusicStore.getState().deletePlaylist(1)).rejects.toThrow('delete failed');

    expect(useMusicStore.getState().userPlaylists).toBe(playlists);
  });

  it('deleting a playlist detaches every matching player queue source after success', async () => {
    mocks.deletePlaylist.mockResolvedValueOnce(undefined);
    const selected = response('new').items[0];
    useMusicStore.setState({ userPlaylists: [{ id: 7, name: 'Delete', itemCount: 2 }] });
    usePlayerStore.getState().play(selected, [
      { track: response('old').items[0], source: { kind: 'PLAYLIST', id: 7 } },
      { track: selected, source: { kind: 'PLAYLIST', id: 7 } },
      { track: { ...selected, music_id: 3 }, source: { kind: 'PLAYLIST', id: 8 } },
    ]);
    usePlayerStore.getState().setCurrentTime(14);

    await useMusicStore.getState().deletePlaylist(7);

    expect(usePlayerStore.getState().queue.map((entry) => entry.source)).toEqual([
      { kind: 'SINGLE', id: null },
      { kind: 'SINGLE', id: null },
      { kind: 'PLAYLIST', id: 8 },
    ]);
    expect(usePlayerStore.getState()).toMatchObject({ playing: true, currentTime: 14, queueIndex: 1 });
  });

  it('removes an item immutably only after the API succeeds', async () => {
    const pending = deferred<void>();
    mocks.removeFromPlaylist.mockReturnValueOnce(pending.promise);
    const items = [response('old').items[0], response('new').items[0]];
    useMusicStore.setState({ currentPlaylist: { id: 7, name: 'List', items } });

    const operation = useMusicStore.getState().removeFromPlaylist(7, 1);
    expect(useMusicStore.getState().currentPlaylist?.items).toBe(items);
    pending.resolve(undefined);
    await operation;

    expect(useMusicStore.getState().currentPlaylist?.items).not.toBe(items);
    expect(useMusicStore.getState().currentPlaylist?.items.map((item) => item.music_id)).toEqual([2]);
  });

  it('increments the target playlist count after adding a song succeeds', async () => {
    mocks.addToPlaylist.mockResolvedValueOnce(undefined);
    useMusicStore.setState({ userPlaylists: [{ id: 7, name: 'List', itemCount: 2 }] });

    await useMusicStore.getState().addToPlaylist(7, 3);

    expect(useMusicStore.getState().userPlaylists).toEqual([{ id: 7, name: 'List', itemCount: 3 }]);
    expect(mocks.getUserPlaylists).not.toHaveBeenCalled();
  });

  it('decrements the target playlist count after removing a song succeeds', async () => {
    mocks.removeFromPlaylist.mockResolvedValueOnce(undefined);
    useMusicStore.setState({ userPlaylists: [{ id: 7, name: 'List', itemCount: 2 }] });

    await useMusicStore.getState().removeFromPlaylist(7, 1);

    expect(useMusicStore.getState().userPlaylists).toEqual([{ id: 7, name: 'List', itemCount: 1 }]);
    expect(mocks.getUserPlaylists).not.toHaveBeenCalled();
  });

  it('removing a playlist item updates only matching current and pending queue entries', async () => {
    mocks.removeFromPlaylist.mockResolvedValueOnce(undefined);
    const selected = response('old').items[0];
    usePlayerStore.getState().play(selected, [
      { track: selected, source: { kind: 'PLAYLIST', id: 7 } },
      { track: { ...selected, title: 'pending' }, source: { kind: 'PLAYLIST', id: 7 } },
      { track: { ...selected, title: 'other source' }, source: { kind: 'LIBRARY', id: null } },
    ]);

    await useMusicStore.getState().removeFromPlaylist(7, selected.music_id);

    expect(usePlayerStore.getState().queue.map((entry) => [entry.track.title, entry.source])).toEqual([
      ['old', { kind: 'SINGLE', id: null }],
      ['other source', { kind: 'LIBRARY', id: null }],
    ]);
    expect(usePlayerStore.getState().playing).toBe(true);
  });

  it('removing an item invalidates a pending playlist play even when no queue exists', async () => {
    mocks.removeFromPlaylist.mockResolvedValueOnce(undefined);
    const revision = usePlayerStore.getState().stateRevision;

    await useMusicStore.getState().removeFromPlaylist(7, 1);

    expect(usePlayerStore.getState().stateRevision).toBe(revision + 1);
  });

  it('deleting a playlist invalidates a pending playlist play even when no queue exists', async () => {
    mocks.deletePlaylist.mockResolvedValueOnce(undefined);
    const revision = usePlayerStore.getState().stateRevision;

    await useMusicStore.getState().deletePlaylist(7);

    expect(usePlayerStore.getState().stateRevision).toBe(revision + 1);
  });

  it('reordering a playlist invalidates a pending playlist play even when no queue exists', async () => {
    mocks.reorderPlaylist.mockResolvedValueOnce(undefined);
    const revision = usePlayerStore.getState().stateRevision;

    await useMusicStore.getState().reorderPlaylist(7, [2, 1]);

    expect(usePlayerStore.getState().stateRevision).toBe(revision + 1);
  });

  it('reset invalidates an older library request so it cannot overwrite a new session', async () => {
    const oldRequest = deferred<PaginatedResponse<MusicMeta>>();
    mocks.getLibrary.mockReturnValueOnce(oldRequest.promise);
    const loading = useMusicStore.getState().fetchLibrary(0, 20, 'old');

    useMusicStore.getState().reset();
    useMusicStore.setState({ library: response('new').items, libraryTotal: 1 });
    oldRequest.resolve(response('old'));
    await loading;

    expect(useMusicStore.getState().library[0].title).toBe('new');
    expect(useMusicStore.getState().libraryLoading).toBe(false);
  });

  it('reset invalidates an older rename response so it cannot mutate a new session', async () => {
    const oldRename = deferred<{ id: number; user_id: number; name: string; description: string; item_count: number; created_at: string }>();
    mocks.renamePlaylist.mockReturnValueOnce(oldRename.promise);
    useMusicStore.setState({ userPlaylists: [{ id: 7, name: 'Old session', itemCount: 1 }] });
    const renaming = useMusicStore.getState().renamePlaylist(7, 'Stale rename', '');

    useMusicStore.getState().reset();
    useMusicStore.setState({ userPlaylists: [{ id: 7, name: 'New session', itemCount: 4 }] });
    oldRename.resolve({ id: 7, user_id: 1, name: 'Stale rename', description: '', item_count: 1, created_at: '' });
    await renaming;

    expect(useMusicStore.getState().userPlaylists).toEqual([{ id: 7, name: 'New session', itemCount: 4 }]);
  });

  it.each(['resolve', 'reject'] as const)('reset makes an old create operation %s inert', async (settlement) => {
    const pending = deferred<Playlist>();
    mocks.createPlaylist.mockReturnValueOnce(pending.promise);
    useMusicStore.setState({ userPlaylists: [{ id: 7, name: 'Old session', itemCount: 1 }] });
    const operation = useMusicStore.getState().createPlaylist(4, 'Stale list');

    useMusicStore.getState().reset();
    useMusicStore.setState({ userPlaylists: [{ id: 7, name: 'New session', itemCount: 4 }] });
    if (settlement === 'resolve') {
      pending.resolve({ id: 8, user_id: 4, name: 'Stale list', description: '', item_count: 0, created_at: '' });
    } else {
      pending.reject(new Error('old create failed'));
    }
    await operation.catch(() => undefined);

    expect(useMusicStore.getState().userPlaylists).toEqual([{ id: 7, name: 'New session', itemCount: 4 }]);
  });

  it.each(['resolve', 'reject'] as const)('reset makes an old add operation %s inert', async (settlement) => {
    const pending = deferred<void>();
    mocks.addToPlaylist.mockReturnValueOnce(pending.promise);
    const operation = useMusicStore.getState().addToPlaylist(7, 3);
    useMusicStore.getState().reset();
    if (settlement === 'resolve') pending.resolve(undefined);
    else pending.reject(new Error('old add failed'));
    await operation.catch(() => undefined);

    expect(useMusicStore.getState().userPlaylists).toEqual([]);
    expect(usePlayerStore.getState().queue).toEqual([]);
  });

  it('reorders current items immutably only after the API succeeds', async () => {
    const pending = deferred<void>();
    mocks.reorderPlaylist.mockReturnValueOnce(pending.promise);
    const items = [response('old').items[0], response('new').items[0]];
    useMusicStore.setState({ currentPlaylist: { id: 7, name: 'List', items } });

    const operation = useMusicStore.getState().reorderPlaylist(7, [2, 1]);
    expect(useMusicStore.getState().currentPlaylist?.items).toBe(items);
    pending.resolve(undefined);
    await operation;

    expect(useMusicStore.getState().currentPlaylist?.items).not.toBe(items);
    expect(useMusicStore.getState().currentPlaylist?.items.map((item) => item.music_id)).toEqual([2, 1]);
  });

  it.each([
    ['create', () => useMusicStore.getState().createPlaylist(4, 'New'), mocks.createPlaylist],
    ['rename', () => useMusicStore.getState().renamePlaylist(7, 'New', 'Desc'), mocks.renamePlaylist],
    ['delete', () => useMusicStore.getState().deletePlaylist(7), mocks.deletePlaylist],
    ['add', () => useMusicStore.getState().addToPlaylist(7, 3), mocks.addToPlaylist],
    ['remove', () => useMusicStore.getState().removeFromPlaylist(7, 1), mocks.removeFromPlaylist],
    ['reorder', () => useMusicStore.getState().reorderPlaylist(7, [2, 1]), mocks.reorderPlaylist],
  ] as const)('keeps every state reference and value when %s rejects', async (_name, invoke, apiMock) => {
    const playlists = [{ id: 7, name: 'Keep', itemCount: 2 }];
    const items = [response('old').items[0], response('new').items[0]];
    const currentPlaylist = { id: 7, name: 'Keep', items };
    useMusicStore.setState({ userPlaylists: playlists, currentPlaylist });
    const before = useMusicStore.getState();
    apiMock.mockRejectedValueOnce(new Error('request failed'));

    await expect(invoke()).rejects.toThrow('request failed');

    const after = useMusicStore.getState();
    expect(after.userPlaylists).toBe(before.userPlaylists);
    expect(after.currentPlaylist).toBe(before.currentPlaylist);
    expect(after).toMatchObject({ userPlaylists: playlists, currentPlaylist });
  });
});
