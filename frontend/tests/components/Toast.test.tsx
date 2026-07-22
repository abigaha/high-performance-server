import { render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import Toast from '../../src/components/Toast';

const toast = vi.hoisted(() => ({
  state: {
    messages: [
      { id: 1, type: 'success' as const, text: '上传成功' },
      { id: 2, type: 'error' as const, text: '文件类型不受支持' },
    ],
    remove: vi.fn(),
  },
}));

vi.mock('../../src/stores/toast', () => ({
  useToastStore: (selector: (state: typeof toast.state) => unknown) => selector(toast.state),
}));

describe('Toast', () => {
  beforeEach(() => {
    toast.state.remove.mockReset();
  });

  it('按消息级别提供 live region 并允许显式关闭', async () => {
    const user = userEvent.setup();
    render(<Toast />);

    expect(screen.getByRole('status')).toHaveTextContent('上传成功');
    expect(screen.getByRole('alert')).toHaveTextContent('文件类型不受支持');
    const close = screen.getByRole('button', { name: '关闭通知：文件类型不受支持' });
    await user.click(close);
    expect(toast.state.remove).toHaveBeenCalledWith(2);
  });

  it('通知容器使用移动端安全的水平边距', () => {
    render(<Toast />);
    expect(screen.getByLabelText('通知')).toHaveClass('inset-x-3');
  });
});
