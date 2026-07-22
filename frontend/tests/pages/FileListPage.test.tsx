import { fireEvent, render, screen } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import FileListPage from '../../src/pages/FileListPage';

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
vi.mock('../../src/stores/auth', () => ({
  useAuthStore: (selector: (state: { user: { role: 'VIP' } }) => unknown) => (
    selector({ user: { role: 'VIP' } })
  ),
}));
vi.mock('../../src/stores/toast', () => ({
  useToastStore: (selector: (state: { success: typeof mocks.success }) => unknown) => (
    selector({ success: mocks.success })
  ),
}));

describe('FileListPage', () => {
  beforeEach(() => {
    vi.clearAllMocks();
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

    expect(await screen.findByRole('alert')).toHaveTextContent('数据库暂时不可用');
    fireEvent.click(screen.getByRole('button', { name: '重试' }));
    expect(await screen.findByText('暂无文件')).toBeInTheDocument();
    expect(mocks.getFiles).toHaveBeenCalledTimes(2);
  });
});
