import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { MemoryRouter, Route, Routes } from 'react-router-dom';
import { StrictMode } from 'react';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import UserPlaylistPage from '../../src/pages/UserPlaylistPage';
import { clearUserSession } from '../../src/session/clearUserSession';
import { useAuthStore } from '../../src/stores/auth';
import { useMusicStore } from '../../src/stores/music';
import { usePlayerStore } from '../../src/stores/player';
import { useToastStore } from '../../src/stores/toast';

const mocks = vi.hoisted(() => ({
  getUserPlaylists: vi.fn(),
  getPlaylistItems: vi.fn(),
  getMusicDetail: vi.fn(),
  createPlaylist: vi.fn(),
  renamePlaylist: vi.fn(),
  deletePlaylist: vi.fn(),
  removeFromPlaylist: vi.fn(),
}));

vi.mock('../../src/api/music', () => mocks);

interface Deferred<T> {
  promise: Promise<T>;
  resolve: (value: T) => void;
  reject: (reason: unknown) => void;
}

function deferred<T>(): Deferred<T> {
  let resolve!: (value: T) => void;
  let reject!: (reason: unknown) => void;
  const promise = new Promise<T>((resolvePromise, rejectPromise) => {
    resolve = resolvePromise;
    reject = rejectPromise;
  });
  return { promise, resolve, reject };
}

function playable(musicId: number) {
  return {
    music_id: musicId,
    file_id: musicId + 100,
    title: `Playlist ${musicId}`,
    artist: musicId === 1 ? 'A' : 'B',
    album: '',
    genre: '',
    duration_sec: 40,
    file_hash: `h${musicId}`,
    file_size: 20,
    content_type: 'audio/mpeg',
  };
}

function renderPage() {
  return render(
    <StrictMode>
      <MemoryRouter initialEntries={['/my/music']}>
        <Routes>
          <Route path="/my/music" element={<UserPlaylistPage />} />
          <Route path="/player/:id" element={<div>player route</div>} />
        </Routes>
      </MemoryRouter>
    </StrictMode>,
  );
}

describe('UserPlaylistPage queue source', () => {
  beforeEach(() => {
    vi.resetAllMocks();
    useAuthStore.setState({
      token: 'token',
      user: {
        user_id: 4,
        username: 'owner',
        email: 'owner@example.com',
        role: 'NORMAL',
        vip_status: 'NONE',
        vip_expires_at: null,
        capabilities: ['USE_AUTHENTICATED_FEATURES'],
        created_at: '2026-01-01T00:00:00Z',
      },
      loading: false,
      restored: true,
    });
    useMusicStore.setState({
      library: [],
      libraryTotal: 0,
      libraryLoading: false,
      libraryError: null,
      currentPlaylist: null,
      userPlaylists: [],
    });
    usePlayerStore.getState().reset();
    useToastStore.getState().reset();
    mocks.getUserPlaylists.mockResolvedValue([{
      id: 7,
      user_id: 4,
      name: 'Queue list',
      description: '',
      item_count: 2,
      created_at: '',
    }]);
    mocks.getPlaylistItems.mockResolvedValue([
      { id: 70, playlist_id: 7, music_id: 1, title: 'Playlist 1', artist: 'A', file_hash: 'h1', sort_order: 0, added_at: '' },
      { id: 71, playlist_id: 7, music_id: 2, title: 'Playlist 2', artist: 'B', file_hash: 'h2', sort_order: 1, added_at: '' },
    ]);
    mocks.getMusicDetail.mockResolvedValue(playable(2));
    mocks.createPlaylist.mockImplementation(async (_userId: number, name: string) => ({
      id: 8, user_id: 4, name, description: '', item_count: 0, created_at: '',
    }));
    mocks.deletePlaylist.mockResolvedValue(undefined);
    mocks.removeFromPlaylist.mockResolvedValue(undefined);
    mocks.renamePlaylist.mockImplementation(async (id: number, name: string) => ({
      id, user_id: 4, name, description: '', item_count: 2, created_at: '',
    }));
  });

  it('playing from a playlist snapshots every item with that PLAYLIST id', async () => {
    const user = userEvent.setup();
    renderPage();

    await user.click(await screen.findByRole('button', { name: /Queue list/ }));
    await user.click(await screen.findByRole('button', { name: '播放 Playlist 2' }));
    expect(await screen.findByText('player route')).toBeInTheDocument();
    await waitFor(() => expect(usePlayerStore.getState().queue).toHaveLength(2));
    expect(usePlayerStore.getState().queue.map((entry) => entry.source)).toEqual([
      { kind: 'PLAYLIST', id: 7 },
      { kind: 'PLAYLIST', id: 7 },
    ]);
    expect(usePlayerStore.getState().queueIndex).toBe(1);
  });

  it('does not let an external player revision overwrite a pending playlist play', async () => {
    const request = deferred<ReturnType<typeof playable>>();
    mocks.getMusicDetail.mockReturnValueOnce(request.promise);
    const user = userEvent.setup();
    renderPage();

    await user.click(await screen.findByRole('button', { name: /Queue list/ }));
    await user.click(await screen.findByRole('button', { name: '播放 Playlist 1' }));
    await waitFor(() => expect(mocks.getMusicDetail).toHaveBeenCalledWith(1));

    const replacement = playable(99);
    act(() => usePlayerStore.getState().play(replacement, [{
      track: replacement,
      source: { kind: 'LIBRARY', id: null },
    }]));
    await act(async () => request.resolve(playable(1)));

    expect(usePlayerStore.getState().currentTrack?.music_id).toBe(99);
    expect(usePlayerStore.getState().queue[0].source).toEqual({ kind: 'LIBRARY', id: null });
    expect(screen.queryByText('player route')).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: '播放 Playlist 1' })).not.toBeDisabled();
  });

  it('removing an item invalidates a pending playlist play before it can rebuild the queue', async () => {
    const request = deferred<ReturnType<typeof playable>>();
    mocks.getMusicDetail.mockReturnValueOnce(request.promise);
    vi.spyOn(window, 'confirm').mockReturnValueOnce(true);
    const user = userEvent.setup();
    renderPage();

    await user.click(await screen.findByRole('button', { name: /Queue list/ }));
    await user.click(await screen.findByRole('button', { name: '播放 Playlist 1' }));
    await waitFor(() => expect(mocks.getMusicDetail).toHaveBeenCalledWith(1));
    await user.click(screen.getByRole('button', { name: '从歌单移除 Playlist 2' }));
    await waitFor(() => expect(mocks.removeFromPlaylist).toHaveBeenCalledWith(7, 2));

    await act(async () => { request.resolve(playable(1)); });

    expect(usePlayerStore.getState().queue).toEqual([]);
    expect(screen.queryByText('player route')).not.toBeInTheDocument();
  });

  it('deleting a playlist invalidates a pending playlist play before it can rebuild the queue', async () => {
    const request = deferred<ReturnType<typeof playable>>();
    mocks.getMusicDetail.mockReturnValueOnce(request.promise);
    vi.spyOn(window, 'confirm').mockReturnValueOnce(true);
    const user = userEvent.setup();
    renderPage();

    await user.click(await screen.findByRole('button', { name: /Queue list/ }));
    await user.click(await screen.findByRole('button', { name: '播放 Playlist 1' }));
    await waitFor(() => expect(mocks.getMusicDetail).toHaveBeenCalledWith(1));
    await user.click(screen.getByRole('button', { name: '删除歌单 Queue list' }));
    await waitFor(() => expect(mocks.deletePlaylist).toHaveBeenCalledWith(7));

    await act(async () => { request.resolve(playable(1)); });

    expect(usePlayerStore.getState().queue).toEqual([]);
    expect(screen.queryByText('player route')).not.toBeInTheDocument();
  });

  it('creates a playlist through the Store without refetching the list', async () => {
    const user = userEvent.setup();
    renderPage();

    await screen.findByRole('button', { name: /Queue list/ });
    const fetchCalls = mocks.getUserPlaylists.mock.calls.length;
    await user.click(screen.getByRole('button', { name: '新建' }));
    await user.type(screen.getByLabelText('歌单名称'), 'Created locally');
    await user.click(screen.getByRole('button', { name: '创建' }));

    expect(await screen.findByRole('button', { name: /Created locally/ })).toBeInTheDocument();
    expect(mocks.getUserPlaylists).toHaveBeenCalledTimes(fetchCalls);
  });

  it('renames a playlist through the Store without refetching the list', async () => {
    const user = userEvent.setup();
    renderPage();

    await user.click(await screen.findByRole('button', { name: /Queue list/ }));
    const fetchCalls = mocks.getUserPlaylists.mock.calls.length;
    await user.click(screen.getByRole('button', { name: '重命名歌单 Queue list' }));
    const rename = screen.getByLabelText('歌单新名称');
    fireEvent.change(rename, { target: { value: 'Renamed locally' } });
    await user.click(screen.getByRole('button', { name: '保存' }));

    expect(await screen.findByRole('button', { name: '删除歌单 Renamed locally' })).toBeInTheDocument();
    expect(mocks.getUserPlaylists).toHaveBeenCalledTimes(fetchCalls);
  });

  it('round-trips a nonempty playlist description during rename', async () => {
    const description = 'Keep this server description';
    mocks.getUserPlaylists.mockResolvedValue([{
      id: 7,
      user_id: 4,
      name: 'Described list',
      description,
      item_count: 2,
      created_at: '',
    }]);
    mocks.renamePlaylist.mockImplementation(async (id: number, name: string, sentDescription: string) => ({
      id,
      user_id: 4,
      name,
      description: sentDescription,
      item_count: 2,
      created_at: '',
    }));
    const user = userEvent.setup();
    renderPage();

    await user.click(await screen.findByRole('button', { name: /Described list/ }));
    await user.click(screen.getByRole('button', { name: '重命名歌单 Described list' }));
    fireEvent.change(screen.getByLabelText('歌单新名称'), { target: { value: 'Renamed description' } });
    await user.click(screen.getByRole('button', { name: '保存' }));

    await waitFor(() => expect(mocks.renamePlaylist).toHaveBeenCalledWith(7, 'Renamed description', description));
    expect(useMusicStore.getState().userPlaylists).toEqual([{
      id: 7,
      user_id: 4,
      name: 'Renamed description',
      description,
      item_count: 2,
      itemCount: 2,
      created_at: '',
    }]);
  });

  it.each(['resolve', 'reject'] as const)(
    'session reset makes an old description rename %s inert',
    async (settlement) => {
      const description = 'Protected description';
      const request = deferred<{
        id: number;
        user_id: number;
        name: string;
        description: string;
        item_count: number;
        created_at: string;
      }>();
      mocks.getUserPlaylists.mockResolvedValue([{
        id: 7,
        user_id: 4,
        name: 'Protected list',
        description,
        item_count: 2,
        created_at: '',
      }]);
      mocks.renamePlaylist.mockReturnValueOnce(request.promise);
      const user = userEvent.setup();
      renderPage();

      await user.click(await screen.findByRole('button', { name: /Protected list/ }));
      await user.click(screen.getByRole('button', { name: '重命名歌单 Protected list' }));
      fireEvent.change(screen.getByLabelText('歌单新名称'), { target: { value: 'Old session rename' } });
      await user.click(screen.getByRole('button', { name: '保存' }));
      await waitFor(() => expect(mocks.renamePlaylist).toHaveBeenCalledWith(7, 'Old session rename', description));

      act(() => clearUserSession());
      await act(async () => {
        if (settlement === 'resolve') {
          request.resolve({
            id: 7,
            user_id: 4,
            name: 'Old session rename',
            description,
            item_count: 2,
            created_at: '',
          });
        } else {
          request.reject(new Error('old rename failed'));
        }
        await request.promise.catch(() => undefined);
      });

      expect(useMusicStore.getState().userPlaylists).toEqual([]);
      expect(useToastStore.getState().messages).toEqual([]);
      expect(screen.queryByRole('alert')).not.toBeInTheDocument();
    },
  );

  it('deleting the current source playlist keeps playback and detaches every queue entry', async () => {
    const user = userEvent.setup();
    vi.spyOn(window, 'confirm').mockReturnValueOnce(true);
    usePlayerStore.getState().play(playable(1), [
      { track: playable(1), source: { kind: 'PLAYLIST', id: 7 } },
      { track: playable(2), source: { kind: 'PLAYLIST', id: 7 } },
    ]);
    renderPage();

    await user.click(await screen.findByRole('button', { name: /Queue list/ }));
    const fetchCalls = mocks.getUserPlaylists.mock.calls.length;
    await user.click(screen.getByRole('button', { name: '删除歌单 Queue list' }));

    await waitFor(() => expect(mocks.deletePlaylist).toHaveBeenCalledWith(7));
    expect(usePlayerStore.getState().currentTrack?.music_id).toBe(1);
    expect(usePlayerStore.getState().queue.map((entry) => entry.source)).toEqual([
      { kind: 'SINGLE', id: null },
      { kind: 'SINGLE', id: null },
    ]);
    expect(screen.queryByRole('button', { name: /Queue list/ })).not.toBeInTheDocument();
    expect(mocks.getUserPlaylists).toHaveBeenCalledTimes(fetchCalls);
  });

  it('does not let an old delete clear a newly selected playlist', async () => {
    const request = deferred<void>();
    mocks.getUserPlaylists.mockResolvedValue([
      { id: 7, user_id: 4, name: 'First list', description: '', item_count: 1, created_at: '' },
      { id: 8, user_id: 4, name: 'Second list', description: '', item_count: 1, created_at: '' },
    ]);
    mocks.getPlaylistItems.mockImplementation(async (playlistId: number) => [{
      id: playlistId * 10,
      playlist_id: playlistId,
      music_id: playlistId,
      title: playlistId === 7 ? 'First track' : 'Second track',
      artist: 'Artist',
      file_hash: `hash-${playlistId}`,
      sort_order: 0,
      added_at: '',
    }]);
    mocks.deletePlaylist.mockReturnValueOnce(request.promise);
    vi.spyOn(window, 'confirm').mockReturnValueOnce(true);
    const user = userEvent.setup();
    renderPage();

    await user.click(await screen.findByRole('button', { name: /First list/ }));
    await screen.findByText('First track');
    await user.click(screen.getByRole('button', { name: '删除歌单 First list' }));
    await user.click(screen.getByRole('button', { name: /Second list/ }));
    expect(await screen.findByText('Second track')).toBeInTheDocument();

    await act(async () => request.resolve(undefined));

    expect(screen.getByText('Second track')).toBeInTheDocument();
    expect(useToastStore.getState().messages).toEqual([]);
  });

  it('removes a song through the Store without refetching or restoring the old count', async () => {
    const user = userEvent.setup();
    vi.spyOn(window, 'confirm').mockReturnValueOnce(true);
    renderPage();

    await user.click(await screen.findByRole('button', { name: /Queue list/ }));
    const fetchCalls = mocks.getUserPlaylists.mock.calls.length;
    await user.click(screen.getByRole('button', { name: '从歌单移除 Playlist 1' }));

    await waitFor(() => expect(screen.queryByText('Playlist 1')).not.toBeInTheDocument());
    expect(screen.getAllByRole('button', { name: /Queue list/ }).find((button) => button.hasAttribute('aria-pressed')))
      .toHaveTextContent('1 首');
    expect(mocks.getUserPlaylists).toHaveBeenCalledTimes(fetchCalls);
  });

  it('renders every populated playlist action as a 44px control', async () => {
    const user = userEvent.setup();
    renderPage();

    await user.click(await screen.findByRole('button', { name: /Queue list/ }));
    await user.click(screen.getByRole('button', { name: '新建' }));
    const playlistButton = screen.getAllByRole('button', { name: /Queue list/ })
      .find((button) => button.hasAttribute('aria-pressed'));

    for (const button of [
      screen.getByRole('button', { name: '取消' }),
      screen.getByRole('button', { name: '创建' }),
      screen.getByRole('button', { name: '重命名歌单 Queue list' }),
      screen.getByRole('button', { name: '删除歌单 Queue list' }),
      screen.getByRole('button', { name: '播放 Playlist 1' }),
      screen.getByRole('button', { name: '从歌单移除 Playlist 1' }),
    ]) {
      expect(button).toHaveClass('min-h-11', 'min-w-11');
    }
    expect(playlistButton).toHaveClass('min-h-11', 'min-w-44');
    expect(screen.getByLabelText('歌单名称')).toHaveClass('min-h-11', 'min-w-11');
  });

  it('renders the playlist list retry action as a 44px control', async () => {
    mocks.getUserPlaylists.mockRejectedValue(new Error('playlist list unavailable'));
    renderPage();

    expect(await screen.findByRole('button', { name: '重试' })).toHaveClass('min-h-11');
  });

  it('renders the playlist item retry action as a 44px control', async () => {
    mocks.getPlaylistItems.mockRejectedValue(new Error('playlist items unavailable'));
    const user = userEvent.setup();
    renderPage();

    await user.click(await screen.findByRole('button', { name: /Queue list/ }));
    expect(await screen.findByRole('button', { name: '重试' })).toHaveClass('min-h-11');
  });

  it('empty playlists expose accessible rename and delete actions', async () => {
    mocks.getUserPlaylists.mockResolvedValue([{
      id: 7, user_id: 4, name: 'Empty list', description: '', item_count: 0, created_at: '',
    }]);
    mocks.getPlaylistItems.mockResolvedValue([]);
    vi.spyOn(window, 'confirm').mockReturnValueOnce(true);
    const user = userEvent.setup();
    renderPage();

    await user.click(await screen.findByRole('button', { name: /Empty list/ }));
    const renameButton = screen.getByRole('button', { name: '重命名歌单 Empty list' });
    const deleteButton = screen.getByRole('button', { name: '删除歌单 Empty list' });
    expect(renameButton).toHaveClass('min-h-11', 'min-w-11');
    expect(deleteButton).toHaveClass('min-h-11', 'min-w-11');
    await user.click(renameButton);
    const rename = screen.getByLabelText('歌单新名称');
    fireEvent.change(rename, { target: { value: 'Renamed list' } });
    await user.click(screen.getByRole('button', { name: '保存' }));
    await waitFor(() => expect(mocks.renamePlaylist).toHaveBeenCalledWith(7, 'Renamed list', ''));
    await user.click(screen.getByRole('button', { name: '删除歌单 Renamed list' }));
    await waitFor(() => expect(mocks.deletePlaylist).toHaveBeenCalledWith(7));
  });

  it.each(['resolve', 'reject'] as const)(
    'clearing the session makes an old playlist detail %s inert',
    async (settlement) => {
      const request = deferred<ReturnType<typeof playable>>();
      mocks.getMusicDetail.mockReturnValueOnce(request.promise);
      const user = userEvent.setup();
      renderPage();
      await user.click(await screen.findByRole('button', { name: /Queue list/ }));
      await user.click(await screen.findByRole('button', { name: '播放 Playlist 1' }));
      await waitFor(() => expect(mocks.getMusicDetail).toHaveBeenCalledWith(1));

      act(() => clearUserSession());
      await act(async () => {
        if (settlement === 'resolve') request.resolve(playable(1));
        else request.reject(new Error('old request failed'));
        await request.promise.catch(() => undefined);
      });

      expect(usePlayerStore.getState().queue).toEqual([]);
      expect(useToastStore.getState().messages).toEqual([]);
      expect(screen.queryByText('player route')).not.toBeInTheDocument();
      expect(screen.queryByRole('alert')).not.toBeInTheDocument();
    },
  );

  it('a later playlist play click supersedes an older detail response', async () => {
    const first = deferred<ReturnType<typeof playable>>();
    const second = deferred<ReturnType<typeof playable>>();
    mocks.getMusicDetail
      .mockReturnValueOnce(first.promise)
      .mockReturnValueOnce(second.promise);
    const user = userEvent.setup();
    renderPage();
    await user.click(await screen.findByRole('button', { name: /Queue list/ }));

    await user.click(await screen.findByRole('button', { name: '播放 Playlist 1' }));
    await user.click(screen.getByRole('button', { name: '播放 Playlist 2' }));
    await act(async () => second.resolve(playable(2)));
    expect(await screen.findByText('player route')).toBeInTheDocument();
    await act(async () => first.resolve(playable(1)));

    expect(usePlayerStore.getState().currentTrack?.music_id).toBe(2);
    expect(usePlayerStore.getState().queueIndex).toBe(1);
  });

  it('unmount makes an old playlist detail rejection inert', async () => {
    const request = deferred<ReturnType<typeof playable>>();
    mocks.getMusicDetail.mockReturnValueOnce(request.promise);
    const user = userEvent.setup();
    const { unmount } = renderPage();
    await user.click(await screen.findByRole('button', { name: /Queue list/ }));
    await user.click(await screen.findByRole('button', { name: '播放 Playlist 1' }));
    unmount();

    await act(async () => {
      request.reject(new Error('old request failed'));
      await request.promise.catch(() => undefined);
    });

    expect(usePlayerStore.getState().queue).toEqual([]);
    expect(useToastStore.getState().messages).toEqual([]);
  });
});
