import { fireEvent, render, screen } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import FileCard from '../../src/components/FileCard';
import MusicCard from '../../src/components/MusicCard';
import Pagination from '../../src/components/Pagination';
import type { FileRecord, MusicMeta } from '../../src/types/api';

const file: FileRecord = {
  file_id: 3,
  file_name: 'report.pdf',
  file_hash: 'hash',
  file_size: 1024,
  content_type: 'application/pdf',
  created_at: '2026-07-22T00:00:00Z',
};

const music: MusicMeta = {
  music_id: 4,
  title: 'Track',
  artist: 'Artist',
  album: '',
  genre: '',
  duration_sec: 60,
  file_hash: 'music-hash',
  file_size: 2048,
  content_type: 'audio/mpeg',
};

describe('content controls', () => {
  it('file actions have names and do not bubble into the card link', () => {
    const open = vi.fn();
    const download = vi.fn();
    render(
      <div onClick={open}>
        <FileCard file={file} onDownload={download} />
      </div>,
    );

    fireEvent.click(screen.getByRole('button', { name: '下载 report.pdf' }));

    expect(download).toHaveBeenCalledWith(3);
    expect(open).not.toHaveBeenCalled();
  });

  it('music actions do not bubble and expose their busy state', () => {
    const open = vi.fn();
    render(
      <div onClick={open}>
        <MusicCard music={music} onPlay={vi.fn()} onAddToPlaylist={vi.fn()} busyAction="add" />
      </div>,
    );

    const addButton = screen.getByRole('button', { name: '将 Track 添加到歌单' });
    expect(addButton).toBeDisabled();
    fireEvent.click(addButton);
    expect(open).not.toHaveBeenCalled();
  });

  it('pagination exposes the current page and bounded navigation', () => {
    const onChange = vi.fn();
    render(<Pagination current={1} total={60} pageSize={20} onChange={onChange} />);

    expect(screen.getByRole('button', { name: '上一页' })).toBeDisabled();
    expect(screen.getByRole('button', { name: '第 1 页' })).toHaveAttribute('aria-current', 'page');
    fireEvent.click(screen.getByRole('button', { name: '下一页' }));
    expect(onChange).toHaveBeenCalledWith(2);
  });
});
