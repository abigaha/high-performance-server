import { render, screen } from '@testing-library/react';
import { MemoryRouter, Route, Routes } from 'react-router-dom';
import { describe, expect, it, vi } from 'vitest';

const auth = vi.hoisted(() => ({ role: 'NORMAL' }));
vi.mock('../../src/stores/auth', () => ({
  useAuthStore: (selector: (state: { user: { role: string } }) => unknown) => selector({ user: { role: auth.role } }),
}));

import RoleUsersRedirect from '../../src/components/RoleUsersRedirect';

describe('/users role redirect', () => {
  it.each([
    ['NORMAL', '文件页面'],
    ['VIP', '文件页面'],
    ['ADMIN', '管理页面'],
  ])('%s 重定向到正确目标', (role, target) => {
    auth.role = role;
    render(<MemoryRouter initialEntries={['/users']}><Routes>
      <Route path="/users" element={<RoleUsersRedirect />} />
      <Route path="/files" element={<div>文件页面</div>} />
      <Route path="/admin/users" element={<div>管理页面</div>} />
    </Routes></MemoryRouter>);
    expect(screen.getByText(target)).toBeInTheDocument();
  });
});
