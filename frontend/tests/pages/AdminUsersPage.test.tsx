import { act, render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { MemoryRouter } from 'react-router-dom';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import AdminUsersPage from '../../src/pages/AdminUsersPage';

const mocks = vi.hoisted(() => ({
  getAdminUsers: vi.fn(),
  grantUserVip: vi.fn(),
  revokeUserVip: vi.fn(),
}));

vi.mock('../../src/api/admin', () => mocks);

const adminUser = { user_id: 1, username: 'alice', email: 'alice@example.com', role: 'NORMAL', vip_status: 'NONE', vip_expires_at: null, created_at: '2026-01-01T00:00:00Z' } as const;

describe('AdminUsersPage mutation ordering', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    mocks.getAdminUsers.mockResolvedValue({ items: [adminUser], total: 1, offset: 0, limit: 20 });
  });

  it('mutation aborts the active list request and its response cannot overwrite the mutation row', async () => {
    let resolveList!: (value: unknown) => void;
    mocks.getAdminUsers
      .mockResolvedValueOnce({ items: [adminUser], total: 1, offset: 0, limit: 20 })
      .mockReturnValueOnce(new Promise((resolve) => { resolveList = resolve; }));
    mocks.grantUserVip.mockResolvedValue({ ...adminUser, role: 'VIP', vip_status: 'ACTIVE' });
    render(<MemoryRouter><AdminUsersPage /></MemoryRouter>);
    const grant = () => screen.getAllByRole('button', { name: '授予 alice 30 天会员' })[0];

    const search = screen.getByRole('textbox');
    await userEvent.type(search, 'a');
    await new Promise((resolve) => window.setTimeout(resolve, 350));
    await waitFor(() => expect(mocks.getAdminUsers).toHaveBeenCalledTimes(2));
    await userEvent.click(grant());
    await waitFor(() => expect(mocks.grantUserVip).toHaveBeenCalledWith(1, 30));
    await act(async () => resolveList({ items: [adminUser], total: 1, offset: 0, limit: 20 }));

    await waitFor(() => expect(screen.getAllByText('有效')).toHaveLength(2));
    expect(screen.queryByText('未开通')).not.toBeInTheDocument();
  });
});
