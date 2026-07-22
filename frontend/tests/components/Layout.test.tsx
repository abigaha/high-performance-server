import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { MemoryRouter, Route, Routes, useNavigate } from 'react-router-dom';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import AppLayout from '../../src/components/Layout';

const auth = vi.hoisted(() => ({
  state: {
    user: { user_id: 1, username: 'testuser', email: '', role: 'NORMAL' as const },
    logout: vi.fn(),
  },
}));

vi.mock('../../src/stores/auth', () => ({
  useAuthStore: (selector: (state: typeof auth.state) => unknown) => selector(auth.state),
}));
vi.mock('../../src/components/AudioPlayer', () => ({
  default: () => <div data-testid="mini-player" />,
}));

function FilesPage() {
  const navigate = useNavigate();
  return (
    <div>
      文件内容
      <button type="button" onClick={() => navigate('/upload')}>程序跳转</button>
    </div>
  );
}

function renderLayout(initialEntry = '/files') {
  render(
    <MemoryRouter initialEntries={[initialEntry]}>
      <Routes>
        <Route element={<AppLayout />}>
          <Route path="/files" element={<FilesPage />} />
          <Route path="/upload" element={<div>上传页面内容</div>} />
          <Route path="/player/:id" element={<div>全屏播放器页面</div>} />
        </Route>
        <Route path="/login" element={<div>登录页面</div>} />
      </Routes>
    </MemoryRouter>,
  );
}

describe('应用响应式壳层', () => {
  beforeEach(() => {
    vi.stubGlobal('requestAnimationFrame', (callback: FrameRequestCallback) => {
      callback(0);
      return 1;
    });
    vi.stubGlobal('cancelAnimationFrame', vi.fn());
    Object.defineProperty(window, 'matchMedia', {
      configurable: true,
      value: vi.fn().mockImplementation((query: string) => ({
        matches: false,
        media: query,
        onchange: null,
        addEventListener: vi.fn(),
        removeEventListener: vi.fn(),
        addListener: vi.fn(),
        removeListener: vi.fn(),
        dispatchEvent: vi.fn(),
      })),
    });
    document.body.style.overflow = '';
  });

  it('移动正文不带固定左偏移且 Header 显示页面与用户', () => {
    renderLayout();

    const main = screen.getByRole('main');
    expect(main).not.toHaveClass('ml-60');
    expect(main).toHaveClass('lg:ml-60');
    expect(screen.getByRole('heading', { name: '文件' })).toBeInTheDocument();
    expect(screen.getByLabelText(/当前用户：testuser/)).toBeInTheDocument();
    expect(screen.getByTestId('mini-player')).toBeInTheDocument();
  });

  it('Header 在 lg 隐藏菜单按钮且保留其他图标按钮的响应式宽度', () => {
    renderLayout();

    const menuButton = screen.getByRole('button', { name: '打开导航菜单' });
    expect(menuButton).toHaveClass('icon-button', 'lg:hidden');
    expect(menuButton).not.toHaveClass('hidden');

    const logoutButton = screen.getByRole('button', { name: '退出登录' });
    expect(logoutButton).toHaveClass('icon-button', 'sm:w-auto', 'sm:px-3');
  });

  it('全屏播放器路由不重复挂载 mini 播放器', () => {
    renderLayout('/player/42');

    expect(screen.getByText('全屏播放器页面')).toBeInTheDocument();
    expect(screen.getByRole('heading', { name: '播放器' })).toBeInTheDocument();
    expect(screen.queryByTestId('mini-player')).not.toBeInTheDocument();
  });

  it('Escape 和遮罩关闭抽屉并将焦点还给菜单按钮', async () => {
    const user = userEvent.setup();
    renderLayout();
    const trigger = screen.getByRole('button', { name: '打开导航菜单' });

    await user.click(trigger);
    expect(trigger).toHaveAttribute('aria-expanded', 'true');
    expect(screen.getByRole('dialog', { name: '主导航' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: '关闭导航菜单' })).toHaveFocus();
    expect(document.body.style.overflow).toBe('hidden');

    fireEvent.keyDown(document, { key: 'Escape' });
    await waitFor(() => expect(trigger).toHaveAttribute('aria-expanded', 'false'));
    expect(trigger).toHaveFocus();
    expect(document.body.style.overflow).toBe('');

    await user.click(trigger);
    await user.click(screen.getByRole('button', { name: '关闭导航遮罩' }));
    expect(trigger).toHaveAttribute('aria-expanded', 'false');
    expect(trigger).toHaveFocus();
  });

  it('路由变化会关闭抽屉、恢复焦点并更新标题', async () => {
    const user = userEvent.setup();
    renderLayout();
    const trigger = screen.getByRole('button', { name: '打开导航菜单' });

    await user.click(trigger);
    await user.click(screen.getByRole('button', { name: '程序跳转' }));

    expect(await screen.findByText('上传页面内容')).toBeInTheDocument();
    await waitFor(() => expect(trigger).toHaveAttribute('aria-expanded', 'false'));
    expect(screen.getByRole('heading', { name: '上传音频' })).toBeInTheDocument();
    expect(trigger).toHaveFocus();
  });
});
