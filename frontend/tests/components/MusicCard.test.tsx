import { render, screen } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import MusicCard from '../../src/components/MusicCard';

const music = {
  music_id: 7,
  title: 'Accessible song',
  artist: 'Artist',
  album: 'Album',
  genre: 'Genre',
  duration_sec: 90,
  file_hash: 'hash',
  file_size: 1024,
  content_type: 'audio/mpeg',
};

describe('MusicCard', () => {
  it('renders every music action as an accessible 44px control', () => {
    render(
      <MusicCard
        music={music}
        inPlaylist
        onPlay={vi.fn()}
        onAddToPlaylist={vi.fn()}
        onRemove={vi.fn()}
      />,
    );

    for (const name of [
      '播放 Accessible song',
      '将 Accessible song 添加到歌单',
      '从歌单移除 Accessible song',
    ]) {
      expect(screen.getByRole('button', { name })).toHaveClass('min-h-11', 'min-w-11');
    }
  });
});
