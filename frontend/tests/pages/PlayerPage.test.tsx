import { act, render, screen, waitFor } from '@testing-library/react';
import { MemoryRouter, Route, Routes } from 'react-router-dom';
import { StrictMode } from 'react';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import PlayerPage from '../../src/pages/PlayerPage';
import { markSessionChanged } from '../../src/api/client';
import { clearUserSession } from '../../src/session/clearUserSession';
import { usePlayerStore } from '../../src/stores/player';

const mocks = vi.hoisted(() => ({
  getMusicDetail: vi.fn(),
}));

vi.mock('../../src/api/music', () => ({ getMusicDetail: mocks.getMusicDetail }));
vi.mock('../../src/components/AudioPlayer', () => ({
  default: ({ mode }: { mode: 'mini' | 'fullscreen' }) => <div data-mode={mode}>完整播放器</div>,
}));

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

function renderPlayerPage(path = '/player/12') {
  return render(
    <StrictMode>
      <MemoryRouter initialEntries={[path]}>
        <Routes>
          <Route path="/player/:id" element={<PlayerPage />} />
        </Routes>
      </MemoryRouter>
    </StrictMode>,
  );
}

describe('PlayerPage', () => {
  beforeEach(() => {
    mocks.getMusicDetail.mockReset();
    usePlayerStore.getState().reset();
  });

  it('restores a playable track from a deep link', async () => {
    mocks.getMusicDetail.mockResolvedValue({
      music_id: 12,
      file_id: 22,
      title: 'Deep Link',
      artist: 'Artist',
      album: '',
      genre: '',
      duration_sec: 90,
      file_hash: 'hash',
      file_size: 1024,
      content_type: 'audio/mpeg',
    });

    renderPlayerPage();

    expect(screen.getByRole('status')).toHaveTextContent('正在加载音乐');
    expect(await screen.findByText('完整播放器')).toHaveAttribute('data-mode', 'fullscreen');
    await waitFor(() => expect(usePlayerStore.getState().currentTrack).toEqual(
      expect.objectContaining({ music_id: 12, file_id: 22 }),
    ));
    expect(usePlayerStore.getState().queue).toEqual([{
      track: expect.objectContaining({ music_id: 12, file_id: 22 }),
      source: { kind: 'SINGLE', id: null },
    }]);
  });

  it('rejects an invalid route id without requesting the API', async () => {
    renderPlayerPage('/player/not-a-number');

    expect(await screen.findByRole('alert')).toHaveTextContent('音乐编号无效');
    expect(mocks.getMusicDetail).not.toHaveBeenCalled();
  });

  it('renders both return actions as 44px controls', async () => {
    mocks.getMusicDetail.mockRejectedValue(new Error('detail failed'));
    renderPlayerPage();

    await screen.findByRole('alert');
    const buttons = screen.getAllByRole('button', { name: '返回音乐库' });
    expect(buttons).toHaveLength(2);
    for (const button of buttons) {
      expect(button).toHaveClass('min-h-11', 'min-w-11');
    }
  });

  it('does not let an old deep-link response overwrite state after reset', async () => {
    let resolveOld!: (track: ReturnType<typeof playableTrack>) => void;
    mocks.getMusicDetail.mockReturnValue(new Promise((resolve) => {
      resolveOld = resolve;
    }));
    renderPlayerPage();
    await waitFor(() => expect(mocks.getMusicDetail).toHaveBeenCalledWith(12));

    usePlayerStore.getState().reset();
    const replacement = playableTrack(99);
    usePlayerStore.getState().play(replacement, [{
      track: replacement,
      source: { kind: 'LIBRARY', id: null },
    }]);
    resolveOld(playableTrack(12));

    await waitFor(() => expect(usePlayerStore.getState().currentTrack?.music_id).toBe(99));
    expect(usePlayerStore.getState().queue[0].source).toEqual({ kind: 'LIBRARY', id: null });
  });

  it('ends its loading state when a player revision makes the detail stale', async () => {
    const request = deferred<ReturnType<typeof playableTrack>>();
    mocks.getMusicDetail.mockReturnValue(request.promise);
    renderPlayerPage();
    await waitFor(() => expect(mocks.getMusicDetail).toHaveBeenCalledWith(12));

    const replacement = playableTrack(99);
    act(() => usePlayerStore.getState().play(replacement, [{
      track: replacement,
      source: { kind: 'LIBRARY', id: null },
    }]));

    await act(async () => request.resolve(playableTrack(12)));

    await waitFor(() => expect(screen.queryByRole('status')).not.toBeInTheDocument());
    expect(usePlayerStore.getState().currentTrack?.music_id).toBe(99);
    expect(usePlayerStore.getState().queue[0].source).toEqual({ kind: 'LIBRARY', id: null });
  });

  it('discards a session-stale detail while still ending its loading state', async () => {
    const request = deferred<ReturnType<typeof playableTrack>>();
    mocks.getMusicDetail.mockReturnValue(request.promise);
    renderPlayerPage();
    await waitFor(() => expect(mocks.getMusicDetail).toHaveBeenCalledWith(12));

    act(() => markSessionChanged());
    await act(async () => request.resolve(playableTrack(12)));

    expect(await screen.findByText('未找到可播放的音乐')).toBeInTheDocument();
    expect(usePlayerStore.getState().queue).toEqual([]);
  });

  it('immediately leaves loading when a pending detail is invalidated by player revision', async () => {
    mocks.getMusicDetail.mockReturnValue(new Promise(() => {}));
    renderPlayerPage();
    await waitFor(() => expect(mocks.getMusicDetail).toHaveBeenCalledWith(12));

    const replacement = playableTrack(99);
    act(() => usePlayerStore.getState().play(replacement, [{
      track: replacement,
      source: { kind: 'LIBRARY', id: null },
    }]));

    expect(await screen.findByText('完整播放器')).toBeInTheDocument();
    expect(screen.queryByRole('status')).not.toBeInTheDocument();
    expect(usePlayerStore.getState().currentTrack?.music_id).toBe(99);
  });

  it('immediately leaves loading when a pending detail is invalidated by session reset', async () => {
    mocks.getMusicDetail.mockReturnValue(new Promise(() => {}));
    renderPlayerPage();
    await waitFor(() => expect(mocks.getMusicDetail).toHaveBeenCalledWith(12));

    act(() => clearUserSession());

    expect(await screen.findByText('未找到可播放的音乐')).toBeInTheDocument();
    expect(screen.queryByRole('status')).not.toBeInTheDocument();
  });

  it.each([
    ['next', () => usePlayerStore.getState().next(), 13, 2],
    ['prev', () => usePlayerStore.getState().prev(), 11, 0],
  ] as const)('does not apply a late detail after %s changes the current entry', async (
    _name,
    move,
    expectedMusicId,
    expectedIndex,
  ) => {
    const request = deferred<ReturnType<typeof playableTrack>>();
    mocks.getMusicDetail.mockReturnValue(request.promise);
    renderPlayerPage();
    await waitFor(() => expect(mocks.getMusicDetail).toHaveBeenCalledWith(12));
    const selected = playableTrack(12);
    act(() => {
      usePlayerStore.getState().play(selected, [
        { track: playableTrack(11), source: { kind: 'LIBRARY', id: null } },
        { track: selected, source: { kind: 'LIBRARY', id: null } },
        { track: playableTrack(13), source: { kind: 'LIBRARY', id: null } },
      ]);
      move();
    });
    const before = usePlayerStore.getState();

    await act(async () => request.resolve({ ...playableTrack(12), title: 'Late detail' }));

    const after = usePlayerStore.getState();
    expect(after.currentTrack?.music_id).toBe(expectedMusicId);
    expect(after.queueIndex).toBe(expectedIndex);
    expect(after.queue).toEqual(before.queue);
    expect(after.playing).toBe(true);
  });

  it('does not hydrate a late detail over another play of the same music from a different source', async () => {
    const request = deferred<ReturnType<typeof playableTrack>>();
    mocks.getMusicDetail.mockReturnValue(request.promise);
    renderPlayerPage();
    await waitFor(() => expect(mocks.getMusicDetail).toHaveBeenCalledWith(12));
    const replacement = { ...playableTrack(12), title: 'Playlist version' };
    act(() => usePlayerStore.getState().play(replacement, [{
      track: replacement,
      source: { kind: 'PLAYLIST', id: 7 },
    }]));

    await act(async () => request.resolve({ ...playableTrack(12), title: 'Late single detail' }));

    expect(usePlayerStore.getState().currentTrack?.title).toBe('Playlist version');
    expect(usePlayerStore.getState().queue).toEqual([{
      track: expect.objectContaining({ title: 'Playlist version' }),
      source: { kind: 'PLAYLIST', id: 7 },
    }]);
    expect(usePlayerStore.getState().playing).toBe(true);
  });

  it('allows detail hydration when only playback progress changes', async () => {
    const selected = { ...playableTrack(12), title: 'Metadata' };
    usePlayerStore.getState().play(selected, [{
      track: selected,
      source: { kind: 'LIBRARY', id: null },
    }]);
    const request = deferred<ReturnType<typeof playableTrack>>();
    mocks.getMusicDetail.mockReturnValue(request.promise);
    renderPlayerPage();
    await waitFor(() => expect(mocks.getMusicDetail).toHaveBeenCalledWith(12));
    act(() => usePlayerStore.getState().setCurrentTime(27));

    await act(async () => request.resolve({ ...playableTrack(12), title: 'Hydrated' }));

    expect(usePlayerStore.getState().currentTrack?.title).toBe('Hydrated');
    expect(usePlayerStore.getState().queue[0].source).toEqual({ kind: 'LIBRARY', id: null });
    expect(usePlayerStore.getState().currentTime).toBe(27);
    expect(usePlayerStore.getState().playing).toBe(true);
  });

  it('does not show an old detail error after the queue revision changes', async () => {
    const request = deferred<ReturnType<typeof playableTrack>>();
    mocks.getMusicDetail.mockReturnValue(request.promise);
    renderPlayerPage();
    await waitFor(() => expect(mocks.getMusicDetail).toHaveBeenCalledWith(12));
    const selected = playableTrack(12);
    act(() => {
      usePlayerStore.getState().play(selected, [
        { track: selected, source: { kind: 'LIBRARY', id: null } },
        { track: playableTrack(13), source: { kind: 'LIBRARY', id: null } },
      ]);
      usePlayerStore.getState().next();
    });

    await act(async () => {
      request.reject(new Error('old detail failed'));
      await request.promise.catch(() => undefined);
    });

    expect(usePlayerStore.getState().currentTrack?.music_id).toBe(13);
    expect(usePlayerStore.getState().playing).toBe(true);
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  });
});

function playableTrack(musicId: number) {
  return {
    music_id: musicId,
    file_id: musicId + 10,
    title: `Track ${musicId}`,
    artist: 'Artist',
    album: '',
    genre: '',
    duration_sec: 90,
    file_hash: `hash-${musicId}`,
    file_size: 1024,
    content_type: 'audio/mpeg',
  };
}
