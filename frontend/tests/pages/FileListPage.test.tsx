import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { StrictMode } from 'react';
import { MemoryRouter } from 'react-router-dom';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import FileListPage from '../../src/pages/FileListPage';
import { clearUserSession } from '../../src/session/clearUserSession';
import { useToastStore } from '../../src/stores/toast';

const file = {
  file_id: 3,
  file_name: 'report.pdf',
  file_hash: 'hash',
  file_size: 1024,
  content_type: 'application/pdf',
  uploaded_by: 7,
  can_delete: true,
  created_at: '2026-07-22T00:00:00Z',
};

const mocks = vi.hoisted(() => ({
  getFiles: vi.fn(),
  deleteFile: vi.fn(),
  getFileDownloadUrl: vi.fn(),
  success: vi.fn(),
}));

vi.mock('../../src/api/files', () => ({
  getFiles: mocks.getFiles,
  deleteFile: mocks.deleteFile,
  getFileDownloadUrl: mocks.getFileDownloadUrl,
}));

interface Deferred<T> {
  promise: Promise<T>;
  resolve: (value: T) => void;
}

function deferred<T>(): Deferred<T> {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((resolvePromise) => {
    resolve = resolvePromise;
  });
  return { promise, resolve };
}

describe('FileListPage', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    clearUserSession();
    useToastStore.setState({ success: mocks.success });
  });

  afterEach(() => {
    vi.restoreAllMocks();
  });

  it('shows a loading status while files are being fetched', () => {
    mocks.getFiles.mockReturnValue(new Promise(() => {}));

    render(
      <MemoryRouter>
        <FileListPage />
      </MemoryRouter>,
    );

    expect(screen.getByRole('status')).toHaveTextContent('正在加载文件...');
  });

  it('shows the detailed load error and retries into the empty state', async () => {
    mocks.getFiles
      .mockRejectedValueOnce(new Error('数据库暂时不可用'))
      .mockResolvedValueOnce({ items: [], total: 0, offset: 0, limit: 20 });

    render(
      <MemoryRouter>
        <FileListPage />
      </MemoryRouter>,
    );

    const alert = await screen.findByRole('alert');
    expect(alert).toHaveTextContent('数据库暂时不可用');
    expect(screen.getAllByRole('alert')).toHaveLength(1);
    expect(alert.querySelector('section[aria-label="卡片"]')).not.toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: '重试' }));
    expect(await screen.findByText('暂无文件')).toBeInTheDocument();
    expect(mocks.getFiles).toHaveBeenCalledTimes(2);
  });

  it('downloads the selected file', async () => {
    mocks.getFiles.mockResolvedValue({ items: [file], total: 1, offset: 0, limit: 20 });
    mocks.getFileDownloadUrl.mockResolvedValue('/api/files/3/download');
    const anchorClick = vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(() => {});

    render(
      <MemoryRouter>
        <FileListPage />
      </MemoryRouter>,
    );

    fireEvent.click(await screen.findByRole('button', { name: `下载 ${file.file_name}` }));

    await waitFor(() => expect(mocks.getFileDownloadUrl).toHaveBeenCalledWith(file.file_id, expect.any(AbortSignal)));
    expect(anchorClick).toHaveBeenCalledOnce();
  });

  it('keeps an old download inert after a session change and a later download', async () => {
    const oldDownload = deferred<string>();
    const newDownload = deferred<string>();
    mocks.getFiles.mockResolvedValue({ items: [file], total: 1, offset: 0, limit: 20 });
    mocks.getFileDownloadUrl
      .mockReturnValueOnce(oldDownload.promise)
      .mockReturnValueOnce(newDownload.promise);
    const anchorClick = vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(() => {});
    const createElement = vi.spyOn(document, 'createElement');

    render(
      <StrictMode>
        <MemoryRouter><FileListPage /></MemoryRouter>
      </StrictMode>,
    );

    const downloadButton = await screen.findByRole('button', { name: `下载 ${file.file_name}` });
    fireEvent.click(downloadButton);
    expect(downloadButton).toBeDisabled();
    const oldSignal = mocks.getFileDownloadUrl.mock.calls[0][1] as AbortSignal;
    expect(oldSignal).toBeInstanceOf(AbortSignal);

    act(() => clearUserSession());
    expect(oldSignal.aborted).toBe(true);
    await waitFor(() => expect(screen.getByRole('button', { name: `下载 ${file.file_name}` })).not.toBeDisabled());
    fireEvent.click(screen.getByRole('button', { name: `下载 ${file.file_name}` }));
    await waitFor(() => expect(mocks.getFileDownloadUrl).toHaveBeenCalledTimes(2));
    expect(screen.getByRole('button', { name: `下载 ${file.file_name}` })).toBeDisabled();
    createElement.mockClear();

    await act(async () => oldDownload.resolve('/api/files/old/download'));

    expect(createElement).not.toHaveBeenCalledWith('a');
    expect(anchorClick).not.toHaveBeenCalled();
    expect(mocks.success).not.toHaveBeenCalled();
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: `下载 ${file.file_name}` })).toBeDisabled();

    await act(async () => newDownload.resolve('/api/files/new/download'));
    await waitFor(() => expect(anchorClick).toHaveBeenCalledOnce());
  });

  it('keeps an unmounted download inert when its promise resolves', async () => {
    const request = deferred<string>();
    mocks.getFiles.mockResolvedValue({ items: [file], total: 1, offset: 0, limit: 20 });
    mocks.getFileDownloadUrl.mockReturnValueOnce(request.promise);
    const anchorClick = vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(() => {});
    const createElement = vi.spyOn(document, 'createElement');

    const { unmount } = render(
      <StrictMode>
        <MemoryRouter><FileListPage /></MemoryRouter>
      </StrictMode>,
    );

    fireEvent.click(await screen.findByRole('button', { name: `下载 ${file.file_name}` }));
    createElement.mockClear();
    unmount();
    await act(async () => request.resolve('/api/files/old/download'));

    expect(createElement).not.toHaveBeenCalledWith('a');
    expect(anchorClick).not.toHaveBeenCalled();
    expect(mocks.success).not.toHaveBeenCalled();
  });

  it('confirms and deletes a file only when the API grants can_delete', async () => {
    mocks.getFiles.mockResolvedValue({ items: [file], total: 1, offset: 0, limit: 20 });
    mocks.deleteFile.mockResolvedValue(undefined);
    const confirm = vi.spyOn(window, 'confirm').mockReturnValue(true);

    render(
      <MemoryRouter>
        <FileListPage />
      </MemoryRouter>,
    );

    fireEvent.click(await screen.findByRole('button', { name: `删除 ${file.file_name}` }));

    expect(confirm).toHaveBeenCalledWith(`确定删除“${file.file_name}”吗？此操作无法撤销。`);
    await waitFor(() => {
      expect(mocks.deleteFile).toHaveBeenCalledWith(file.file_id);
      expect(mocks.success).toHaveBeenCalledWith('文件已删除');
    });
  });

  it('hides delete for a VIP when can_delete is false', async () => {
    mocks.getFiles.mockResolvedValue({ items: [{ ...file, can_delete: false }], total: 1, offset: 0, limit: 20 });

    render(
      <MemoryRouter>
        <FileListPage />
      </MemoryRouter>,
    );

    await screen.findByText(file.file_name);
    expect(screen.queryByRole('button', { name: `删除 ${file.file_name}` })).not.toBeInTheDocument();
  });

  it('名称和类型变化回到第一页，并中止旧请求且拒绝旧响应覆盖', async () => {
    const controllers: AbortSignal[] = [];
    mocks.getFiles.mockImplementation((_query, signal?: AbortSignal) => {
      if (signal) controllers.push(signal);
      return Promise.resolve({ items: [file], total: 41, offset: 0, limit: 20 });
    });
    render(<MemoryRouter><FileListPage /></MemoryRouter>);
    fireEvent.click(await screen.findByRole('button', { name: '下一页' }));
    await waitFor(() => expect(mocks.getFiles).toHaveBeenLastCalledWith(
      expect.objectContaining({ offset: 20 }), expect.any(AbortSignal),
    ));

    fireEvent.change(screen.getByLabelText('文件名称'), { target: { value: 'report' } });
    await waitFor(() => expect(mocks.getFiles).toHaveBeenLastCalledWith(
      expect.objectContaining({ name: 'report', offset: 0 }), expect.any(AbortSignal),
    ));
    fireEvent.change(screen.getByLabelText('文件类型'), { target: { value: 'other' } });
    await waitFor(() => expect(mocks.getFiles).toHaveBeenLastCalledWith(
      expect.objectContaining({ name: 'report', type: 'other', offset: 0 }), expect.any(AbortSignal),
    ));
    expect(controllers.some((signal) => signal.aborted)).toBe(true);
  });

  it('类型选项严格使用顶级类型、other 和空值', () => {
    mocks.getFiles.mockReturnValue(new Promise(() => {}));
    render(<MemoryRouter><FileListPage /></MemoryRouter>);
    expect(screen.getAllByRole('option').map((option) => (option as HTMLOptionElement).value))
      .toEqual(['', 'audio', 'image', 'video', 'other']);
  });

  it('旧请求后完成也不能覆盖新筛选响应', async () => {
    let resolveOld!: (value: unknown) => void;
    let resolveNew!: (value: unknown) => void;
    mocks.getFiles
      .mockReturnValueOnce(new Promise((resolve) => { resolveOld = resolve; }))
      .mockReturnValueOnce(new Promise((resolve) => { resolveNew = resolve; }));
    render(<MemoryRouter><FileListPage /></MemoryRouter>);
    fireEvent.change(screen.getByLabelText('文件名称'), { target: { value: 'new' } });
    const newer = { ...file, file_id: 4, file_name: 'new.pdf' };
    resolveNew({ items: [newer], total: 1, offset: 0, limit: 20 });
    await screen.findByText('new.pdf');
    resolveOld({ items: [file], total: 1, offset: 0, limit: 20 });
    await Promise.resolve();
    expect(screen.queryByText('report.pdf')).not.toBeInTheDocument();
  });

  it('删除完成后按当前筛选重新加载，不能使用删除前的 files closure', async () => {
    const newer = { ...file, file_id: 4, file_name: 'filtered.pdf' };
    mocks.getFiles
      .mockResolvedValueOnce({ items: [file], total: 1, offset: 0, limit: 20 })
      .mockResolvedValueOnce({ items: [newer], total: 1, offset: 0, limit: 20 })
      .mockResolvedValueOnce({ items: [newer], total: 1, offset: 0, limit: 20 });
    mocks.deleteFile.mockResolvedValueOnce(undefined);
    vi.spyOn(window, 'confirm').mockReturnValue(true);

    render(<MemoryRouter><FileListPage /></MemoryRouter>);
    fireEvent.change(await screen.findByLabelText('文件名称'), { target: { value: 'filtered' } });
    await waitFor(() => expect(screen.getByText('filtered.pdf')).toBeInTheDocument());
    fireEvent.click(screen.getByRole('button', { name: '删除 filtered.pdf' }));

    await waitFor(() => expect(mocks.getFiles).toHaveBeenLastCalledWith(
      expect.objectContaining({ name: 'filtered', offset: 0 }), expect.any(AbortSignal),
    ));
    expect(screen.getByText('filtered.pdf')).toBeInTheDocument();
  });

  it('筛选切换后仍结束旧删除自己的忙碌态，且不重新加载旧筛选', async () => {
    const removal = deferred<void>();
    mocks.getFiles.mockImplementation(() => Promise.resolve({ items: [file], total: 1, offset: 0, limit: 20 }));
    mocks.deleteFile.mockReturnValueOnce(removal.promise);
    vi.spyOn(window, 'confirm').mockReturnValue(true);

    render(
      <StrictMode>
        <MemoryRouter><FileListPage /></MemoryRouter>
      </StrictMode>,
    );

    const deleteButton = await screen.findByRole('button', { name: `删除 ${file.file_name}` });
    fireEvent.click(deleteButton);
    expect(deleteButton).toBeDisabled();

    fireEvent.change(screen.getByLabelText('文件名称'), { target: { value: 'filtered' } });
    await waitFor(() => expect(mocks.getFiles).toHaveBeenLastCalledWith(
      expect.objectContaining({ name: 'filtered', offset: 0 }), expect.any(AbortSignal),
    ));

    await act(async () => {
      removal.resolve(undefined);
      await removal.promise;
    });

    await waitFor(() => expect(screen.getByRole('button', { name: `删除 ${file.file_name}` })).not.toBeDisabled());
    expect(mocks.success).not.toHaveBeenCalled();
  });

  it('会话清理后释放旧删除的忙碌态，并让旧完成保持静默', async () => {
    const removal = deferred<void>();
    mocks.getFiles.mockResolvedValue({ items: [file], total: 1, offset: 0, limit: 20 });
    mocks.deleteFile.mockReturnValueOnce(removal.promise);
    vi.spyOn(window, 'confirm').mockReturnValue(true);

    render(
      <StrictMode>
        <MemoryRouter><FileListPage /></MemoryRouter>
      </StrictMode>,
    );

    const deleteButton = await screen.findByRole('button', { name: `删除 ${file.file_name}` });
    fireEvent.click(deleteButton);
    expect(deleteButton).toBeDisabled();

    act(() => clearUserSession());
    await waitFor(() => expect(screen.getByRole('button', { name: `删除 ${file.file_name}` })).not.toBeDisabled());

    await act(async () => {
      removal.resolve(undefined);
      await removal.promise;
    });

    expect(mocks.success).not.toHaveBeenCalled();
  });

  it('标题使用真实链接且操作按钮是链接的 siblings', async () => {
    mocks.getFiles.mockResolvedValue({ items: [file], total: 1, offset: 0, limit: 20 });
    render(<MemoryRouter><FileListPage /></MemoryRouter>);
    const link = await screen.findByRole('link', { name: file.file_name });
    expect(link).toHaveAttribute('href', '/files/3');
    expect(link.contains(screen.getByRole('button', { name: `下载 ${file.file_name}` }))).toBe(false);
  });

  it('renders upload, retry, and empty upload actions as 44px controls', async () => {
    mocks.getFiles
      .mockRejectedValueOnce(new Error('load failed'))
      .mockResolvedValueOnce({ items: [], total: 0, offset: 0, limit: 20 });
    render(<MemoryRouter><FileListPage /></MemoryRouter>);

    const retry = await screen.findByRole('button', { name: '重试' });
    expect(screen.getByRole('button', { name: '上传文件' })).toHaveClass('min-h-11', 'min-w-11');
    expect(retry).toHaveClass('min-h-11', 'min-w-11');
    fireEvent.click(retry);
    expect(await screen.findByRole('button', { name: '上传第一个文件' })).toHaveClass('min-h-11', 'min-w-11');
  });
});
