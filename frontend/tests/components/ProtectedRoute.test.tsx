import { render, screen } from '@testing-library/react';
import { MemoryRouter, Route, Routes } from 'react-router-dom';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import ProtectedRoute from '../../src/components/ProtectedRoute';

const auth = vi.hoisted(() => ({
  state: {
    token: null as string | null,
    user: null as { role: 'NORMAL' | 'VIP' } | null,
    loading: false,
    restored: true,
  },
}));

vi.mock('../../src/stores/auth', () => ({
  useAuthStore: (selector: (state: typeof auth.state) => unknown) => selector(auth.state),
}));

function renderRoute(requiredRole?: 'NORMAL' | 'VIP') {
  render(
    <MemoryRouter initialEntries={['/private']}>
      <Routes>
        <Route element={<ProtectedRoute requiredRole={requiredRole} />}>
          <Route path="/private" element={<div>受保护内容</div>} />
        </Route>
        <Route path="/login" element={<div>登录页面</div>} />
        <Route path="/files" element={<div>文件页面</div>} />
      </Routes>
    </MemoryRouter>,
  );
}

describe('ProtectedRoute 会话恢复守卫', () => {
  beforeEach(() => {
    Object.assign(auth.state, { token: null, user: null, loading: false, restored: true });
  });

  it('无令牌时进入登录页', () => {
    renderRoute();
    expect(screen.getByText('登录页面')).toBeInTheDocument();
  });

  it('令牌恢复期间不提前渲染内容或误判 VIP 权限', () => {
    Object.assign(auth.state, { token: 'token', loading: true, restored: false });
    renderRoute('VIP');
    expect(screen.getByRole('status')).toHaveTextContent('正在恢复会话');
    expect(screen.queryByText('文件页面')).not.toBeInTheDocument();
  });

  it('恢复完成后按归一化角色执行权限检查', () => {
    Object.assign(auth.state, { token: 'token', user: { role: 'NORMAL' }, restored: true });
    renderRoute('VIP');
    expect(screen.getByText('文件页面')).toBeInTheDocument();
  });
});
