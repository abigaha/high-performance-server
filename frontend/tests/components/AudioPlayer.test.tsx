import { act, render, screen, waitFor } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import AudioPlayer from '../../src/components/AudioPlayer';
import { usePlayerStore } from '../../src/stores/player';
import type { PlayableMusic } from '../../src/api/music';

const mocks = vi.hoisted(() => ({
  getFileStreamUrl: vi.fn(),
}));

vi.mock('../../src/api/files', () => ({
  getFileStreamUrl: mocks.getFileStreamUrl,
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

function setTrack(currentTrack: PlayableMusic) {
  usePlayerStore.setState({
    currentTrack,
    playlist: [currentTrack],
    playlistIndex: 0,
    playing: false,
    currentTime: 0,
    duration: currentTrack.duration_sec,
    volume: 0.8,
  });
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
    vi.spyOn(URL, 'revokeObjectURL').mockImplementation(() => {});
    vi.spyOn(HTMLMediaElement.prototype, 'pause').mockImplementation(() => {});
    vi.spyOn(HTMLMediaElement.prototype, 'play').mockResolvedValue();
  });

  afterEach(() => {
    vi.restoreAllMocks();
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
