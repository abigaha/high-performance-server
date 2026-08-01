import { act, cleanup, render, screen, waitFor } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import { StrictMode } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import AudioPlayer from '../../src/components/AudioPlayer';
import { usePlayerStore } from '../../src/stores/player';
import type { PlayableMusic } from '../../src/api/music';

const mocks = vi.hoisted(() => ({
  getFileStreamUrl: vi.fn(),
  getMusicDetail: vi.fn(),
}));

vi.mock('../../src/api/files', () => ({
  getFileStreamUrl: mocks.getFileStreamUrl,
}));
vi.mock('../../src/api/music', () => ({
  getMusicDetail: mocks.getMusicDetail,
}));

const track: PlayableMusic = {
  music_id: 5,
  file_id: 15,
  title: 'Playable',
  artist: 'Artist',
  album: '',
  genre: '',
  duration_sec: 120,
  file_hash: 'hash',
  file_size: 4096,
  content_type: 'audio/mpeg',
};

const nextTrack: PlayableMusic = {
  ...track,
  music_id: 6,
  file_id: 16,
  title: 'Next track',
};

function setTrack(currentTrack: PlayableMusic, playing = false) {
  usePlayerStore.setState({
    currentTrack,
    queue: [{ track: currentTrack, source: { kind: 'SINGLE', id: null } }],
    queueIndex: 0,
    playing,
    currentTime: 0,
    duration: currentTrack.duration_sec,
    volume: 0.8,
  });
}

function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (reason: unknown) => void;
  const promise = new Promise<T>((resolvePromise, rejectPromise) => {
    resolve = resolvePromise;
    reject = rejectPromise;
  });
  return { promise, resolve, reject };
}

function renderPlayer(mode: 'mini' | 'fullscreen' = 'fullscreen', route = '/files') {
  return render(
    <MemoryRouter initialEntries={[route]}>
      <AudioPlayer mode={mode} />
    </MemoryRouter>,
  );
}

describe('AudioPlayer', () => {
  beforeEach(() => {
    setTrack(track);
    mocks.getFileStreamUrl.mockReset();
    mocks.getFileStreamUrl.mockResolvedValue('blob:track-15');
    mocks.getMusicDetail.mockReset();
    mocks.getMusicDetail.mockImplementation(async (musicId: number) => ({
      ...track,
      music_id: musicId,
      file_id: musicId + 10,
    }));
    vi.spyOn(URL, 'revokeObjectURL').mockImplementation(() => {});
    vi.spyOn(HTMLMediaElement.prototype, 'pause').mockImplementation(() => {});
    vi.spyOn(HTMLMediaElement.prototype, 'play').mockResolvedValue();
    vi.spyOn(HTMLMediaElement.prototype, 'load').mockImplementation(() => {});
  });

  afterEach(() => {
    cleanup();
    vi.restoreAllMocks();
  });

  it('hydrates a queued metadata-only track before requesting its stream', async () => {
    const metadataOnly = { ...nextTrack };
    delete (metadataOnly as { file_id?: number }).file_id;
    setTrack(metadataOnly);

    renderPlayer();

    await waitFor(() => expect(mocks.getMusicDetail).toHaveBeenCalledWith(6));
    await waitFor(() => expect(mocks.getFileStreamUrl).toHaveBeenCalledWith(16));
    expect(usePlayerStore.getState().currentTrack).toEqual(expect.objectContaining({
      music_id: 6,
      file_id: 16,
    }));
  });

  it.each(['track change', 'reset'] as const)(
    'ignores an old audio.play rejection after %s starts a new track',
    async (transition) => {
      const oldPlay = deferred<void>();
      vi.mocked(HTMLMediaElement.prototype.play)
        .mockReturnValueOnce(oldPlay.promise)
        .mockResolvedValue(undefined);
      setTrack(track, true);
      const { container } = render(
        <StrictMode>
          <MemoryRouter initialEntries={['/files']}>
            <AudioPlayer mode="fullscreen" />
          </MemoryRouter>
        </StrictMode>,
      );
      await waitFor(() => expect(HTMLMediaElement.prototype.play).toHaveBeenCalled());

      act(() => {
        if (transition === 'reset') usePlayerStore.getState().reset();
        setTrack(nextTrack, true);
      });
      await waitFor(() => expect(container.querySelector('audio')).toHaveAttribute('src', 'blob:track-15'));
      await act(async () => {
        oldPlay.reject(new Error('old autoplay rejection'));
        await oldPlay.promise.catch(() => undefined);
      });

      expect(usePlayerStore.getState().currentTrack?.music_id).toBe(6);
      expect(usePlayerStore.getState().playing).toBe(true);
      expect(screen.queryByText(/old autoplay rejection/)).not.toBeInTheDocument();
    },
  );

  it('fullscreen 模式展示装饰封面且只挂载一个 audio', async () => {
    const { container } = renderPlayer('fullscreen', '/player/5');

    await waitFor(() => expect(container.querySelector('audio')).toHaveAttribute('src', 'blob:track-15'));
    expect(container.querySelectorAll('audio')).toHaveLength(1);
    const cover = container.querySelector('img');
    expect(cover).toHaveAttribute('src', '/covers/crystal-cover-02.webp');
    expect(cover).toHaveAttribute('alt', '');
    expect(cover).toHaveAttribute('aria-hidden', 'true');
  });

  it('mini 模式展示同一路径的装饰封面且只挂载一个 audio', async () => {
    const { container } = renderPlayer('mini');

    await waitFor(() => expect(container.querySelector('audio')).toHaveAttribute('src', 'blob:track-15'));
    expect(container.querySelectorAll('audio')).toHaveLength(1);
    const cover = container.querySelector('img');
    expect(cover).toHaveAttribute('src', '/covers/crystal-cover-02.webp');
    expect(cover).toHaveAttribute('alt', '');
    expect(cover).toHaveAttribute('aria-hidden', 'true');
  });

  it('renders only the fullscreen audio element on a player route', () => {
    const { container } = render(
      <MemoryRouter initialEntries={['/player/5']}>
        <AudioPlayer mode="mini" />
        <AudioPlayer mode="fullscreen" />
      </MemoryRouter>,
    );

    expect(container.querySelectorAll('audio')).toHaveLength(1);
    expect(screen.getByRole('slider', { name: '播放进度' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: '上一首' })).toBeDisabled();
    expect(screen.getByRole('button', { name: '下一首' })).toBeDisabled();
    expect(mocks.getFileStreamUrl).toHaveBeenCalledTimes(1);
  });

  it('mini 播放器与侧栏统一从 lg 断点开始左偏移', () => {
    renderPlayer('mini');

    const miniPlayer = screen.getByLabelText('迷你播放器');
    expect(miniPlayer).toHaveClass('lg:left-60');
    expect(miniPlayer).not.toHaveClass('md:left-60');
  });

  it('曲目切换和卸载时回收当前 Blob URL', async () => {
    mocks.getFileStreamUrl
      .mockResolvedValueOnce('blob:track-15')
      .mockResolvedValueOnce('blob:track-16');
    const { container, unmount } = renderPlayer();

    await waitFor(() => expect(container.querySelector('audio')).toHaveAttribute('src', 'blob:track-15'));
    act(() => setTrack(nextTrack));
    await waitFor(() => expect(container.querySelector('audio')).toHaveAttribute('src', 'blob:track-16'));

    expect(URL.revokeObjectURL).toHaveBeenCalledWith('blob:track-15');
    unmount();
    expect(URL.revokeObjectURL).toHaveBeenCalledWith('blob:track-16');
  });

  it('曲目切换和卸载时先终止 audio 资源选择再回收其 Blob URL', async () => {
    const teardownEvents: string[] = [];
    mocks.getFileStreamUrl
      .mockResolvedValueOnce('blob:track-15')
      .mockResolvedValueOnce('blob:track-16');
    vi.mocked(HTMLMediaElement.prototype.pause).mockImplementation(() => {
      teardownEvents.push('pause');
    });
    vi.mocked(HTMLMediaElement.prototype.load).mockImplementation(() => {
      teardownEvents.push('load');
    });
    vi.mocked(URL.revokeObjectURL).mockImplementation((url) => {
      teardownEvents.push(`revoke:${url}`);
    });
    const { container, unmount } = renderPlayer();

    const audio = await waitFor(() => {
      const element = container.querySelector('audio');
      expect(element).toHaveAttribute('src', 'blob:track-15');
      return element as HTMLAudioElement;
    });
    vi.spyOn(audio, 'removeAttribute').mockImplementation((name) => {
      teardownEvents.push(`removeAttribute:${name}`);
      Element.prototype.removeAttribute.call(audio, name);
    });
    const expectReleaseSequence = (url: string) => {
      const revokeIndex = teardownEvents.indexOf(`revoke:${url}`);
      expect(revokeIndex).toBeGreaterThanOrEqual(3);
      expect(teardownEvents.slice(revokeIndex - 3, revokeIndex + 1)).toEqual([
        'pause',
        'removeAttribute:src',
        'load',
        `revoke:${url}`,
      ]);
    };
    teardownEvents.length = 0;

    act(() => setTrack(nextTrack));
    await waitFor(() => expect(audio).toHaveAttribute('src', 'blob:track-16'));
    expectReleaseSequence('blob:track-15');

    teardownEvents.length = 0;
    unmount();
    expectReleaseSequence('blob:track-16');
  });

  it('currentSrc 滞后时回收旧 owned URL 但不清空新的 src 属性', async () => {
    const { container, unmount } = renderPlayer();
    const audio = await waitFor(() => {
      const element = container.querySelector('audio');
      expect(element).toHaveAttribute('src', 'blob:track-15');
      return element as HTMLAudioElement;
    });
    Object.defineProperty(audio, 'currentSrc', {
      configurable: true,
      value: 'blob:track-15',
    });
    audio.setAttribute('src', 'blob:track-16');
    const removeAttribute = vi.spyOn(audio, 'removeAttribute');

    unmount();

    expect(removeAttribute).not.toHaveBeenCalledWith('src');
    expect(audio).toHaveAttribute('src', 'blob:track-16');
    expect(URL.revokeObjectURL).toHaveBeenCalledWith('blob:track-15');
  });

  it('曲目切换后回收迟到响应且不覆盖新曲目 URL', async () => {
    let resolveFirst: ((url: string) => void) | undefined;
    mocks.getFileStreamUrl
      .mockImplementationOnce(() => new Promise<string>((resolve) => {
        resolveFirst = resolve;
      }))
      .mockResolvedValueOnce('blob:track-16');
    const { container } = renderPlayer();

    await waitFor(() => expect(mocks.getFileStreamUrl).toHaveBeenCalledWith(15));
    act(() => setTrack(nextTrack));
    await waitFor(() => expect(container.querySelector('audio')).toHaveAttribute('src', 'blob:track-16'));

    await act(async () => resolveFirst?.('blob:late-track-15'));
    await waitFor(() => expect(URL.revokeObjectURL).toHaveBeenCalledWith('blob:late-track-15'));
    expect(container.querySelector('audio')).toHaveAttribute('src', 'blob:track-16');
  });
});
