import { fireEvent, render, screen } from '@testing-library/react';
import { MemoryRouter, Route, Routes } from 'react-router-dom';
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
  uploaded_by: 7,
  can_delete: true,
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
      <MemoryRouter><div onClick={open}>
        <FileCard file={file} onDownload={download} />
      </div></MemoryRouter>,
    );

    fireEvent.click(screen.getByRole('button', { name: '下载 report.pdf' }));

    expect(download).toHaveBeenCalledWith(3);
    expect(open).not.toHaveBeenCalled();
  });

  it('file actions expose the busy state and the complete file name', () => {
    render(
      <MemoryRouter><FileCard
        file={file}
        onDownload={vi.fn()}
        onDelete={vi.fn()}
        downloading
      /></MemoryRouter>,
    );

    expect(screen.getByRole('button', { name: `下载 ${file.file_name}` })).toHaveAttribute(
      'aria-busy',
      'true',
    );
    expect(screen.getByText(file.file_name)).toHaveAttribute('title', file.file_name);
    expect(screen.getByRole('button', { name: `删除 ${file.file_name}` })).toBeDisabled();
  });

  it('file title uses React Router Link for SPA navigation', () => {
    render(<MemoryRouter initialEntries={['/files']}><Routes>
      <Route path="/files" element={<FileCard file={file} />} />
      <Route path="/files/3" element={<div>文件详情页面</div>} />
    </Routes></MemoryRouter>);
    fireEvent.click(screen.getByRole('link', { name: file.file_name }));
    expect(screen.getByText('文件详情页面')).toBeInTheDocument();
  });

  it('enabled music actions invoke their callbacks without bubbling', () => {
    const open = vi.fn();
    const onPlay = vi.fn();
    const onAdd = vi.fn();
    const onRemove = vi.fn();
    render(
      <div onClick={open}>
        <MusicCard
          music={music}
          inPlaylist
          onPlay={onPlay}
          onAddToPlaylist={onAdd}
          onRemove={onRemove}
          busyAction={null}
        />
      </div>,
    );

    const playButton = screen.getByRole('button', { name: '播放 Track' });
    const addButton = screen.getByRole('button', { name: '将 Track 添加到歌单' });
    const removeButton = screen.getByRole('button', { name: '从歌单移除 Track' });
    expect(playButton).toBeEnabled();
    expect(addButton).toBeEnabled();
    expect(removeButton).toBeEnabled();
    fireEvent.click(playButton);
    fireEvent.click(addButton);
    fireEvent.click(removeButton);

    expect(onPlay).toHaveBeenCalledWith(music);
    expect(onAdd).toHaveBeenCalledWith(music.music_id);
    expect(onRemove).toHaveBeenCalledWith(music.music_id);
    expect(open).not.toHaveBeenCalled();
  });

  it('music busy action disables the corresponding button', () => {
    render(
      <MusicCard music={music} onPlay={vi.fn()} onAddToPlaylist={vi.fn()} busyAction="add" />,
    );

    const addButton = screen.getByRole('button', { name: '将 Track 添加到歌单' });
    expect(addButton).toBeDisabled();
  });

  it('music card shows the shared cover and preserves playback', () => {
    const onPlay = vi.fn();
    const onAdd = vi.fn();
    const { container } = render(
      <MusicCard music={music} onPlay={onPlay} onAddToPlaylist={onAdd} busyAction={null} />,
    );

    const cover = container.querySelector('img');
    expect(cover).toHaveAttribute('src', '/covers/crystal-cover-01.webp');
    expect(cover).toHaveAttribute('alt', '');
    expect(cover).toHaveAttribute('aria-hidden', 'true');
    fireEvent.click(screen.getByRole('button', { name: '播放 Track' }));
    expect(onPlay).toHaveBeenCalledWith(music);
  });

  it('pagination exposes the current page and bounded navigation', () => {
    const onChange = vi.fn();
    render(<Pagination current={1} total={60} pageSize={20} onChange={onChange} />);

    expect(screen.getByRole('button', { name: '上一页' })).toBeDisabled();
    expect(screen.getByRole('button', { name: '第 1 页' })).toHaveAttribute('aria-current', 'page');
    expect(screen.getByRole('navigation', { name: '分页导航' })).toHaveClass('pagination');
    for (const button of screen.getAllByRole('button')) expect(button).toHaveClass('pagination-button');
    fireEvent.click(screen.getByRole('button', { name: '下一页' }));
    expect(onChange).toHaveBeenCalledWith(2);
  });
});
