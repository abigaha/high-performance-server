import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { MemoryRouter, Route, Routes } from 'react-router-dom';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

const mocks = vi.hoisted(() => ({
  getFile: vi.fn(),
  deleteFile: vi.fn(),
  getFileDownloadUrl: vi.fn(),
  success: vi.fn(),
  captureSessionSnapshot: vi.fn(),
  isSessionSnapshotCurrent: vi.fn(),
}));
const auth = vi.hoisted(() => ({ role: 'NORMAL', sessionRevision: 1 }));

vi.mock('../../src/api/files', () => ({
  getFile: mocks.getFile,
  deleteFile: mocks.deleteFile,
  getFileDownloadUrl: mocks.getFileDownloadUrl,
}));
vi.mock('../../src/stores/auth', () => ({
  useAuthStore: (selector: (state: { user: { role: string }; sessionRevision: number }) => unknown) => (
    selector({ user: { role: auth.role }, sessionRevision: auth.sessionRevision })
  ),
}));
vi.mock('../../src/stores/toast', () => ({
  useToastStore: (selector: (state: { success: typeof mocks.success }) => unknown) => (
    selector({ success: mocks.success })
  ),
}));
vi.mock('../../src/api/client', () => ({
  captureSessionSnapshot: mocks.captureSessionSnapshot,
  isSessionSnapshotCurrent: mocks.isSessionSnapshotCurrent,
}));

import FileDetailPage from '../../src/pages/FileDetailPage';

const file = {
  file_id: 7,
  file_name: 'detail.mp3',
  file_hash: 'hash-7',
  file_size: 1024,
  content_type: 'audio/mpeg',
  uploaded_by: 42,
  can_delete: true,
  created_at: '2026-01-03T00:00:00Z',
};

function PageRoutes() {
  return (
    <MemoryRouter initialEntries={['/files/7']}>
      <Routes>
        <Route path="/files/:id" element={<FileDetailPage />} />
        <Route path="/files" element={<p>文件列表</p>} />
      </Routes>
    </MemoryRouter>
  );
}

function renderPage() {
  return render(<PageRoutes />);
}

describe('FileDetailPage', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    auth.role = 'NORMAL';
    auth.sessionRevision = 1;
    mocks.deleteFile.mockResolvedValue(undefined);
    mocks.captureSessionSnapshot.mockReturnValue({ token: 'token', revision: 1 });
    mocks.isSessionSnapshotCurrent.mockReturnValue(true);
    vi.spyOn(window, 'confirm').mockReturnValue(true);
  });

  afterEach(() => {
    vi.restoreAllMocks();
  });

  it('NORMAL owner 仅按 record.can_delete 显示并执行删除', async () => {
    auth.role = 'NORMAL';
    mocks.getFile.mockResolvedValue({ ...file, can_delete: true });
    renderPage();

    fireEvent.click(await screen.findByRole('button', { name: '删除' }));

    await waitFor(() => expect(mocks.deleteFile).toHaveBeenCalledWith(7));
  });

  it('other VIP 在 record.can_delete=false 时不显示删除', async () => {
    auth.role = 'VIP';
    mocks.getFile.mockResolvedValue({ ...file, uploaded_by: 99, can_delete: false });
    renderPage();

    await screen.findByText('detail.mp3');
    expect(screen.queryByRole('button', { name: '删除' })).not.toBeInTheDocument();
  });

  it('ADMIN 在 record.can_delete=true 时显示并执行删除', async () => {
    auth.role = 'ADMIN';
    mocks.getFile.mockResolvedValue({ ...file, uploaded_by: 99, can_delete: true });
    renderPage();

    fireEvent.click(await screen.findByRole('button', { name: '删除' }));

    await waitFor(() => expect(mocks.deleteFile).toHaveBeenCalledWith(7));
  });

  it('session 变化后旧 load 与 delete 结果不能更新当前界面', async () => {
    let resolveLoad!: (value: typeof file) => void;
    mocks.getFile.mockReturnValueOnce(new Promise((resolve) => { resolveLoad = resolve; }));
    const first = renderPage();
    mocks.isSessionSnapshotCurrent.mockReturnValue(false);
    await act(async () => { resolveLoad(file); });
    expect(screen.queryByText('detail.mp3')).not.toBeInTheDocument();
    first.unmount();

    let resolveDelete!: () => void;
    mocks.isSessionSnapshotCurrent.mockReturnValue(true);
    mocks.getFile.mockResolvedValueOnce(file);
    mocks.deleteFile.mockReturnValueOnce(new Promise<void>((resolve) => { resolveDelete = resolve; }));
    renderPage();
    fireEvent.click(await screen.findByRole('button', { name: '删除' }));
    mocks.isSessionSnapshotCurrent.mockReturnValue(false);
    await act(async () => { resolveDelete(); });
    expect(mocks.success).not.toHaveBeenCalled();
    expect(screen.queryByText('文件列表')).not.toBeInTheDocument();
  });

  it('错误页操作均为 44px 控件', async () => {
    mocks.getFile.mockRejectedValueOnce(new Error('文件详情不可用'));
    renderPage();

    for (const button of [
      await screen.findByRole('button', { name: '重试' }),
      screen.getByRole('button', { name: '返回列表' }),
    ]) {
      expect(button).toHaveClass('min-h-11', 'min-w-11');
    }
  });

  it('卸载时中止下载且迟到 URL 不触发点击', async () => {
    let resolveDownload!: (url: string) => void;
    const pendingDownload = new Promise<string>((resolve) => { resolveDownload = resolve; });
    mocks.getFile.mockResolvedValueOnce(file);
    mocks.getFileDownloadUrl.mockReturnValueOnce(pendingDownload);
    const click = vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(() => {});
    const page = renderPage();

    fireEvent.click(await screen.findByRole('button', { name: '下载' }));
    await waitFor(() => expect(mocks.getFileDownloadUrl).toHaveBeenCalledWith(7, expect.any(AbortSignal)));
    const signal = mocks.getFileDownloadUrl.mock.calls[0][1] as AbortSignal;

    page.unmount();
    expect(signal.aborted).toBe(true);
    await act(async () => { resolveDownload('blob:late-download'); });

    expect(click).not.toHaveBeenCalled();
  });

  it('会话切换中止下载且迟到 URL 不触发点击', async () => {
    let resolveDownload!: (url: string) => void;
    const pendingDownload = new Promise<string>((resolve) => { resolveDownload = resolve; });
    mocks.getFile.mockResolvedValueOnce(file);
    mocks.getFileDownloadUrl.mockReturnValueOnce(pendingDownload);
    const click = vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(() => {});
    const page = renderPage();

    fireEvent.click(await screen.findByRole('button', { name: '下载' }));
    await waitFor(() => expect(mocks.getFileDownloadUrl).toHaveBeenCalledWith(7, expect.any(AbortSignal)));
    const signal = mocks.getFileDownloadUrl.mock.calls[0][1] as AbortSignal;

    auth.sessionRevision = 2;
    page.rerender(<PageRoutes />);
    await waitFor(() => expect(signal.aborted).toBe(true));
    await act(async () => { resolveDownload('blob:late-download'); });

    expect(click).not.toHaveBeenCalled();
  });
});
