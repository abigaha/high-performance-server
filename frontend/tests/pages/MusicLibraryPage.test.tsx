import { act, render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { MemoryRouter, Route, Routes } from 'react-router-dom';
import { StrictMode } from 'react';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import MusicLibraryPage from '../../src/pages/MusicLibraryPage';
import { clearUserSession } from '../../src/session/clearUserSession';
import { useAuthStore } from '../../src/stores/auth';
import { useMusicStore } from '../../src/stores/music';
import { usePlayerStore } from '../../src/stores/player';
import { useToastStore } from '../../src/stores/toast';
import type { MusicMeta } from '../../src/types/api';

const mocks = vi.hoisted(() => ({
  getLibrary: vi.fn(),
  getMusicDetail: vi.fn(),
  getUserPlaylists: vi.fn(),
  addToPlaylist: vi.fn(),
}));

vi.mock('../../src/api/music', () => mocks);

const music = (musicId: number): MusicMeta => ({
  music_id: musicId,
  title: `Library ${musicId}`,
  artist: 'Artist',
  album: '',
  genre: '',
  duration_sec: 30,
  file_hash: `hash-${musicId}`,
  file_size: 10,
  content_type: 'audio/mpeg',
});

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
  return { ...music(musicId), file_id: musicId + 100 };
}

function renderPage() {
  return render(
    <StrictMode>
      <MemoryRouter initialEntries={['/music/library']}>
        <Routes>
          <Route path="/music/library" element={<MusicLibraryPage />} />
          <Route path="/player/:id" element={<div>player route</div>} />
        </Routes>
      </MemoryRouter>
    </StrictMode>,
  );
}

describe('MusicLibraryPage queue source', () => {
  beforeEach(() => {
    vi.resetAllMocks();
    useAuthStore.setState({ token: null, user: null, loading: false, restored: true });
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
    mocks.getLibrary.mockResolvedValue({ items: [music(1), music(2)], total: 2, offset: 0, limit: 20 });
    mocks.getMusicDetail.mockImplementation(async (id: number) => playable(id));
  });

  it('playing from library snapshots every item as LIBRARY', async () => {
    const user = userEvent.setup();
    renderPage();

    await user.click(await screen.findByRole('button', { name: '播放 Library 2' }));
    expect(await screen.findByText('player route')).toBeInTheDocument();
    await waitFor(() => expect(usePlayerStore.getState().queue).toHaveLength(2));
    expect(usePlayerStore.getState().queue.map((entry) => entry.source)).toEqual([
      { kind: 'LIBRARY', id: null },
      { kind: 'LIBRARY', id: null },
    ]);
    expect(usePlayerStore.getState().queueIndex).toBe(1);
  });

  it('renders the music library retry action as a 44px control', async () => {
    mocks.getLibrary.mockRejectedValue(new Error('library unavailable'));
    renderPage();

    expect(await screen.findByRole('button', { name: '重试' })).toHaveClass('min-h-11');
  });

  it('renders the music search input as a 44px control', () => {
    mocks.getLibrary.mockReturnValue(new Promise(() => {}));
    renderPage();

    expect(screen.getByRole('textbox', { name: '搜索音乐' })).toHaveClass('min-h-11');
  });

  it('does not let an external player revision overwrite a pending library play', async () => {
    const request = deferred<ReturnType<typeof playable>>();
    mocks.getMusicDetail.mockReturnValueOnce(request.promise);
    const user = userEvent.setup();
    renderPage();

    await user.click(await screen.findByRole('button', { name: '播放 Library 1' }));
    await waitFor(() => expect(mocks.getMusicDetail).toHaveBeenCalledWith(1));

    const replacement = playable(99);
    act(() => usePlayerStore.getState().play(replacement, [{
      track: replacement,
      source: { kind: 'PLAYLIST', id: 7 },
    }]));
    await act(async () => request.resolve(playable(1)));

    expect(usePlayerStore.getState().currentTrack?.music_id).toBe(99);
    expect(usePlayerStore.getState().queue[0].source).toEqual({ kind: 'PLAYLIST', id: 7 });
    expect(screen.queryByText('player route')).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: '播放 Library 1' })).not.toBeDisabled();
  });

  it.each(['resolve', 'reject'] as const)(
    'session reset makes an old add %s inert and releases its busy action',
    async (settlement) => {
      const request = deferred<void>();
      mocks.addToPlaylist.mockReturnValueOnce(request.promise);
      mocks.getUserPlaylists.mockResolvedValue([{
        id: 7, user_id: 4, name: 'Target', description: '', item_count: 2, created_at: '',
      }]);
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
      });
      const user = userEvent.setup();
      renderPage();

      const addButton = await screen.findByRole('button', { name: '将 Library 1 添加到歌单' });
      await user.click(addButton);
      expect(addButton).toBeDisabled();

      act(() => {
        clearUserSession();
        useAuthStore.setState({
          token: 'new-token',
          user: {
            user_id: 5,
            username: 'new-owner',
            email: 'new-owner@example.com',
            role: 'NORMAL',
            vip_status: 'NONE',
            vip_expires_at: null,
            capabilities: ['USE_AUTHENTICATED_FEATURES'],
            created_at: '2026-01-01T00:00:00Z',
          },
        });
        useMusicStore.setState({
          library: [music(1)],
          libraryTotal: 1,
          libraryLoading: false,
          libraryError: null,
          userPlaylists: [{ id: 7, name: 'Target', itemCount: 2 }],
        });
      });

      expect(await screen.findByRole('button', { name: '将 Library 1 添加到歌单' })).not.toBeDisabled();
      await act(async () => {
        if (settlement === 'resolve') request.resolve(undefined);
        else request.reject(new Error('old add failed'));
        await request.promise.catch(() => undefined);
      });

      expect(useMusicStore.getState().userPlaylists).toEqual([{
        id: 7,
        user_id: 4,
        name: 'Target',
        description: '',
        item_count: 2,
        itemCount: 2,
        created_at: '',
      }]);
      expect(useToastStore.getState().messages).toEqual([]);
    },
  );

  it.each(['resolve', 'reject'] as const)(
    'clearing the session makes an old play detail %s inert',
    async (settlement) => {
      const request = deferred<ReturnType<typeof playable>>();
      mocks.getMusicDetail.mockReturnValueOnce(request.promise);
      const user = userEvent.setup();
      renderPage();
      await user.click(await screen.findByRole('button', { name: '播放 Library 1' }));
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

  it('a later play click supersedes an older detail response', async () => {
    const first = deferred<ReturnType<typeof playable>>();
    const second = deferred<ReturnType<typeof playable>>();
    mocks.getMusicDetail
      .mockReturnValueOnce(first.promise)
      .mockReturnValueOnce(second.promise);
    const user = userEvent.setup();
    renderPage();

    await user.click(await screen.findByRole('button', { name: '播放 Library 1' }));
    await user.click(screen.getByRole('button', { name: '播放 Library 2' }));
    await act(async () => second.resolve(playable(2)));
    expect(await screen.findByText('player route')).toBeInTheDocument();
    await act(async () => first.resolve(playable(1)));

    expect(usePlayerStore.getState().currentTrack?.music_id).toBe(2);
    expect(usePlayerStore.getState().queueIndex).toBe(1);
  });

  it('unmount makes an old play detail resolve inert', async () => {
    const request = deferred<ReturnType<typeof playable>>();
    mocks.getMusicDetail.mockReturnValueOnce(request.promise);
    const user = userEvent.setup();
    const { unmount } = renderPage();
    await user.click(await screen.findByRole('button', { name: '播放 Library 1' }));
    unmount();

    await act(async () => request.resolve(playable(1)));

    expect(usePlayerStore.getState().queue).toEqual([]);
    expect(useToastStore.getState().messages).toEqual([]);
  });
});
