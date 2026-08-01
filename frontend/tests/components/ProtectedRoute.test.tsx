import { render, screen } from '@testing-library/react';
import { MemoryRouter, Route, Routes, useLocation } from 'react-router-dom';
import { describe, expect, it, vi } from 'vitest';

const auth = vi.hoisted(() => ({
  state: { token: 'token', user: null as null | { role: string; capabilities: string[] }, loading: true, restored: false },
}));
vi.mock('../../src/stores/auth', () => ({
  useAuthStore: (selector: (state: typeof auth.state) => unknown) => selector(auth.state),
}));

import ProtectedRoute from '../../src/components/ProtectedRoute';

function LoginDestination() {
  const location = useLocation();
  const from = (location.state as {
    from?: { pathname?: string; search?: string; hash?: string };
  } | null)?.from;
  const destination = from
    ? `${from.pathname ?? ''}${from.search ?? ''}${from.hash ?? ''}`
    : '无来源';

  return (
    <div>
      登录页面
      <output data-testid="login-from">{destination}</output>
    </div>
  );
}

function renderGuard(capability?: 'MANAGE_USERS', roles?: Array<'NORMAL' | 'VIP'>) {
  render(
    <MemoryRouter initialEntries={['/restricted']}>
      <Routes>
        <Route element={<ProtectedRoute requiredCapability={capability} allowedRoles={roles} />}>
          <Route path="/restricted" element={<div>受限内容</div>} />
        </Route>
        <Route path="/files" element={<div>文件页面</div>} />
      </Routes>
    </MemoryRouter>,
  );
}

describe('ProtectedRoute capability guard', () => {
  it('会话恢复期间不闪现受限内容', () => {
    auth.state = { token: 'token', user: null, loading: true, restored: false };
    renderGuard('MANAGE_USERS');
    expect(screen.getByRole('status')).toHaveTextContent('正在恢复会话');
    expect(screen.queryByText('受限内容')).not.toBeInTheDocument();
  });

  it('恢复后将无 capability 或角色不符的深链重定向到 files', () => {
    auth.state = { token: 'token', user: { role: 'VIP', capabilities: [] }, loading: false, restored: true };
    renderGuard('MANAGE_USERS');
    expect(screen.getByText('文件页面')).toBeInTheDocument();

    auth.state = { token: 'token', user: { role: 'ADMIN', capabilities: ['MANAGE_USERS'] }, loading: false, restored: true };
    renderGuard(undefined, ['NORMAL', 'VIP']);
    expect(screen.getAllByText('文件页面')).toHaveLength(2);
  });

  it.each([
    ['NORMAL', true],
    ['VIP', true],
    ['ADMIN', false],
  ] as const)('VIP 路由对 %s 的允许结果为 %s', (role, allowed) => {
    auth.state = { token: 'token', user: { role, capabilities: role === 'ADMIN' ? ['MANAGE_USERS'] : [] }, loading: false, restored: true };
    renderGuard(undefined, ['NORMAL', 'VIP']);
    expect(screen.queryByText('受限内容') !== null).toBe(allowed);
    expect(screen.queryByText('文件页面') !== null).toBe(!allowed);
  });

  it('未认证用户跳转登录且不渲染受限内容', () => {
    auth.state = { token: '', user: null, loading: false, restored: true };
    render(
      <MemoryRouter initialEntries={['/restricted?folder=recent#latest']}>
        <Routes>
          <Route element={<ProtectedRoute />}><Route path="/restricted" element={<div>受限内容</div>} /></Route>
          <Route path="/login" element={<LoginDestination />} />
        </Routes>
      </MemoryRouter>,
    );
    expect(screen.getByText('登录页面')).toBeInTheDocument();
    expect(screen.getByTestId('login-from')).toHaveTextContent('/restricted?folder=recent#latest');
    expect(screen.queryByText('受限内容')).not.toBeInTheDocument();
  });

  it('ADMIN 具备 MANAGE_USERS 时允许进入管理页', () => {
    auth.state = {
      token: 'token',
      user: { role: 'ADMIN', capabilities: ['MANAGE_USERS'] },
      loading: false,
      restored: true,
    };
    renderGuard('MANAGE_USERS');
    expect(screen.getByText('受限内容')).toBeInTheDocument();
    expect(screen.queryByText('文件页面')).not.toBeInTheDocument();
  });
});
