import { render, screen, waitFor } from '@testing-library/react';
import { MemoryRouter, Route, Routes } from 'react-router-dom';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import PlayerPage from '../../src/pages/PlayerPage';
import { usePlayerStore } from '../../src/stores/player';

const mocks = vi.hoisted(() => ({
  getMusicDetail: vi.fn(),
}));

vi.mock('../../src/api/music', () => ({ getMusicDetail: mocks.getMusicDetail }));
vi.mock('../../src/components/AudioPlayer', () => ({ default: () => <div>完整播放器</div> }));

describe('PlayerPage', () => {
  beforeEach(() => {
    mocks.getMusicDetail.mockReset();
    usePlayerStore.setState({
      currentTrack: null,
      playlist: [],
      playlistIndex: -1,
      playing: false,
      currentTime: 0,
      duration: 0,
      volume: 0.8,
    });
  });

  it('restores a playable track from a deep link', async () => {
    mocks.getMusicDetail.mockResolvedValueOnce({
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

    render(
      <MemoryRouter initialEntries={['/player/12']}>
        <Routes>
          <Route path="/player/:id" element={<PlayerPage />} />
        </Routes>
      </MemoryRouter>,
    );

    expect(screen.getByRole('status')).toHaveTextContent('正在加载音乐');
    expect(await screen.findByText('完整播放器')).toBeInTheDocument();
    await waitFor(() => expect(usePlayerStore.getState().currentTrack).toEqual(
      expect.objectContaining({ music_id: 12, file_id: 22 }),
    ));
  });

  it('rejects an invalid route id without requesting the API', async () => {
    render(
      <MemoryRouter initialEntries={['/player/not-a-number']}>
        <Routes>
          <Route path="/player/:id" element={<PlayerPage />} />
        </Routes>
      </MemoryRouter>,
    );

    expect(await screen.findByRole('alert')).toHaveTextContent('音乐编号无效');
    expect(mocks.getMusicDetail).not.toHaveBeenCalled();
  });
});
