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
    toast.state.messages = [
      { id: 1, type: 'success' as const, text: '上传成功' },
      { id: 2, type: 'error' as const, text: '文件类型不受支持' },
    ];
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

  it('通知容器提供区域语义并完整显示详细错误', () => {
    const longError = '网关返回 502：上游服务暂时不可用，请保留这一整段详细错误并稍后重试';
    toast.state.messages = [{ id: 3, type: 'error', text: longError }];
    render(<Toast />);

    expect(screen.getByRole('region', { name: '通知' })).toBeInTheDocument();
    expect(screen.getByRole('alert')).toHaveTextContent(longError);
    expect(screen.getByRole('button', { name: `关闭通知：${longError}` })).toBeInTheDocument();
  });
});
