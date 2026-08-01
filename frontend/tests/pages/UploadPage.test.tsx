import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import UploadPage from '../../src/pages/UploadPage';

const mocks = vi.hoisted(() => ({
  uploadFile: vi.fn(),
  success: vi.fn(),
  error: vi.fn(),
  info: vi.fn(),
}));

vi.mock('../../src/api/files', () => ({ uploadFile: mocks.uploadFile }));
vi.mock('../../src/stores/auth', () => ({
  useAuthStore: (selector: (state: { user: { role: 'NORMAL' } }) => unknown) => (
    selector({ user: { role: 'NORMAL' } })
  ),
}));
vi.mock('../../src/stores/toast', () => ({
  useToastStore: (selector: (state: {
    success: typeof mocks.success;
    error: typeof mocks.error;
    info: typeof mocks.info;
  }) => unknown) => selector(mocks),
}));

function audioFile(name: string): File {
  return new File(['audio'], name, { type: 'audio/mpeg' });
}

function dropFiles(files: File[]) {
  fireEvent.drop(screen.getByRole('button', { name: /选择音频文件/ }), {
    dataTransfer: { files },
  });
}

describe('UploadPage', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    mocks.uploadFile.mockResolvedValue({
      file_id: 1,
      file_name: 'track.mp3',
      file_hash: 'hash',
      size: 5,
      chunks: 1,
    });
    render(<UploadPage />);
  });

  it('混合批次只上传通过前置校验的文件', async () => {
    dropFiles([
      new File(['notes'], 'notes.txt', { type: 'text/plain' }),
      audioFile('track.mp3'),
    ]);

    expect(await screen.findByText(/不支持该文件类型/)).toBeInTheDocument();
    await waitFor(() => expect(mocks.uploadFile).toHaveBeenCalledTimes(1));
    expect(mocks.uploadFile.mock.calls[0][0]).toEqual(expect.objectContaining({ name: 'track.mp3' }));
    expect(await screen.findByText('上传成功')).toBeInTheDocument();
    expect(mocks.info).toHaveBeenCalledWith('上传结束：1 成功，1 失败，0 取消');
  });

  it('队列保留后端返回的详细错误', async () => {
    mocks.uploadFile.mockRejectedValueOnce(new Error('文件签名与扩展名不匹配'));
    dropFiles([audioFile('broken.mp3')]);

    expect(await screen.findByRole('alert')).toHaveTextContent('文件签名与扩展名不匹配');
    expect(await screen.findByRole('progressbar', { name: 'broken.mp3 上传进度' }))
      .toHaveAttribute('aria-valuetext', '上传失败');
    await waitFor(() => {
      expect(mocks.error).toHaveBeenCalledWith('1 个文件未能上传，请查看队列中的详细原因');
    });
  });
});
