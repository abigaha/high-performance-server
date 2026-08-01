import { render, screen } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { createRef } from 'react';

const auth = vi.hoisted(() => ({
  user: { role: 'NORMAL', capabilities: ['USE_AUTHENTICATED_FEATURES'] } as { role: string; capabilities: string[] },
}));
vi.mock('../../src/stores/auth', () => ({
  useAuthStore: (selector: (state: { user: typeof auth.user }) => unknown) => selector({ user: auth.user }),
}));

import Sidebar from '../../src/components/Sidebar';

function renderSidebar() {
  render(
    <MemoryRouter><Sidebar open sidebarRef={createRef<HTMLElement>()} onClose={vi.fn()} /></MemoryRouter>,
  );
}

describe('Sidebar capability entries', () => {
  beforeEach(() => {
    Object.defineProperty(window, 'matchMedia', {
      configurable: true,
      value: vi.fn(() => ({ matches: true, addEventListener: vi.fn(), removeEventListener: vi.fn() })),
    });
  });

  it('所有登录用户显示资料，NORMAL/VIP 显示会员中心', () => {
    auth.user = { role: 'NORMAL', capabilities: ['USE_AUTHENTICATED_FEATURES'] };
    renderSidebar();
    expect(screen.getByRole('link', { name: '个人资料' })).toHaveAttribute('href', '/profile');
    expect(screen.getByRole('link', { name: '会员中心' })).toHaveAttribute('href', '/vip');
    expect(screen.queryByRole('link', { name: '用户管理' })).not.toBeInTheDocument();
  });

  it('仅 MANAGE_USERS 显示管理员入口，ADMIN 不显示会员中心', () => {
    auth.user = { role: 'ADMIN', capabilities: ['MANAGE_USERS'] };
    renderSidebar();
    expect(screen.getByRole('link', { name: '用户管理' })).toHaveAttribute('href', '/admin/users');
    expect(screen.queryByRole('link', { name: '会员中心' })).not.toBeInTheDocument();
  });
});
