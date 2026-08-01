import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import { beforeEach, describe, expect, it, vi } from 'vitest';

const mocks = vi.hoisted(() => ({
  getUser: vi.fn(), updateUser: vi.fn(), setUser: vi.fn(),
  captureSessionSnapshot: vi.fn(), isSessionSnapshotCurrent: vi.fn(),
}));
const auth = vi.hoisted(() => ({ user: { user_id: 3, role: 'NORMAL' }, sessionRevision: 1, setUser: vi.fn() }));
vi.mock('../../src/api/users', () => ({ getUser: mocks.getUser, updateUser: mocks.updateUser }));
vi.mock('../../src/api/client', () => ({
  captureSessionSnapshot: mocks.captureSessionSnapshot,
  isSessionSnapshotCurrent: mocks.isSessionSnapshotCurrent,
}));
vi.mock('../../src/stores/auth', () => ({
  useAuthStore: (selector: (state: unknown) => unknown) => selector({ user: auth.user, sessionRevision: auth.sessionRevision, setUser: mocks.setUser }),
}));

import ProfilePage from '../../src/pages/ProfilePage';

const profile = {
  user_id: 3, username: 'crystal', email: 'old@example.com', role: 'NORMAL',
  vip_status: 'NONE', vip_expires_at: null,
  capabilities: ['USE_AUTHENTICATED_FEATURES'], created_at: '2026-07-20T12:34:56Z',
};

describe('ProfilePage', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    mocks.captureSessionSnapshot.mockReturnValue({ token: 'token', revision: 1 });
    mocks.isSessionSnapshotCurrent.mockReturnValue(true);
    mocks.getUser.mockResolvedValue(profile);
    auth.user = { user_id: 3, role: 'NORMAL' };
    auth.sessionRevision = 1;
  });

  it('加载失败显示 error 与 retry，重试后 ready', async () => {
    mocks.getUser.mockRejectedValueOnce(new Error('资料服务不可用'));
    render(<MemoryRouter><ProfilePage /></MemoryRouter>);
    expect(screen.getByRole('status')).toHaveTextContent('正在加载个人资料');
    expect(await screen.findByRole('alert')).toHaveTextContent('资料服务不可用');
    mocks.getUser.mockResolvedValueOnce(profile);
    fireEvent.click(screen.getByRole('button', { name: '重试个人资料' }));
    expect(await screen.findByText('crystal')).toBeInTheDocument();
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  });

  it('session 用户变化使旧 save 失效并清 saving/error，再加载新资料', async () => {
    let resolveUpdate!: (value: typeof profile) => void;
    mocks.updateUser.mockReturnValueOnce(new Promise((resolve) => { resolveUpdate = resolve; }));
    const view = render(<MemoryRouter><ProfilePage /></MemoryRouter>);
    fireEvent.click(await screen.findByRole('button', { name: '保存资料' }));
    expect(screen.getByRole('button', { name: '保存中' })).toBeDisabled();
    auth.user = { user_id: 4, role: 'NORMAL' };
    mocks.getUser.mockResolvedValueOnce({ ...profile, user_id: 4, username: 'next-user' });
    view.rerender(<MemoryRouter><ProfilePage /></MemoryRouter>);
    expect(await screen.findByText('next-user')).toBeInTheDocument();
    expect(screen.getByRole('button', { name: '保存资料' })).toBeEnabled();
    resolveUpdate(profile);
    await Promise.resolve();
    expect(mocks.setUser).not.toHaveBeenCalled();
  });

  it('相同 user_id 的 session revision 变化会清 saving 并重新加载', async () => {
    let resolveUpdate!: (value: typeof profile) => void;
    mocks.updateUser.mockReturnValueOnce(new Promise((resolve) => { resolveUpdate = resolve; }));
    const view = render(<MemoryRouter><ProfilePage /></MemoryRouter>);
    fireEvent.click(await screen.findByRole('button', { name: '保存资料' }));
    auth.sessionRevision = 2;
    mocks.getUser.mockResolvedValueOnce({ ...profile, username: 'same-id-new-session' });
    view.rerender(<MemoryRouter><ProfilePage /></MemoryRouter>);
    expect(await screen.findByText('same-id-new-session')).toBeInTheDocument();
    expect(screen.getByRole('button', { name: '保存资料' })).toBeEnabled();
    resolveUpdate(profile);
    await Promise.resolve();
    expect(mocks.setUser).not.toHaveBeenCalled();
  });

  it('仅通过 users API 读取并显示完整身份与 UTC 时间', async () => {
    render(<MemoryRouter><ProfilePage /></MemoryRouter>);

    expect(await screen.findByText('crystal')).toBeInTheDocument();
    expect(mocks.getUser).toHaveBeenCalledWith(3);
    expect(screen.getByText('普通用户')).toBeInTheDocument();
    expect(screen.getByText('未开通')).toBeInTheDocument();
    expect(screen.getByText(/2026-07-20 12:34:56 UTC/)).toBeInTheDocument();
  });

  it('通过 users API 同时支持邮箱和密码更新，并刷新 AuthUser', async () => {
    const updated = { ...profile, email: 'new@example.com' };
    mocks.updateUser.mockResolvedValue(updated);
    render(<MemoryRouter><ProfilePage /></MemoryRouter>);

    fireEvent.change(await screen.findByLabelText('邮箱'), { target: { value: 'new@example.com' } });
    fireEvent.change(screen.getByLabelText('新密码'), { target: { value: 'secure-password' } });
    fireEvent.click(screen.getByRole('button', { name: '保存资料' }));

    await waitFor(() => expect(mocks.updateUser).toHaveBeenCalledWith(3, {
      email: 'new@example.com', password: 'secure-password',
    }));
    expect(mocks.setUser).toHaveBeenCalledWith(updated);
    expect(screen.getByLabelText('新密码')).toHaveValue('');
  });

  it('密码初始为空', async () => {
    render(<MemoryRouter><ProfilePage /></MemoryRouter>);
    expect(await screen.findByLabelText('新密码')).toHaveValue('');
  });

  it('切换账号后旧 load 成功不更新 UI', async () => {
    let resolveLoad!: (value: typeof profile) => void;
    mocks.getUser.mockReturnValueOnce(new Promise((resolve) => { resolveLoad = resolve; }));
    render(<MemoryRouter><ProfilePage /></MemoryRouter>);
    mocks.isSessionSnapshotCurrent.mockReturnValue(false);
    resolveLoad(profile);
    await Promise.resolve();
    expect(screen.queryByText('crystal')).not.toBeInTheDocument();
  });

  it('切换账号后旧 update 成功不写 AuthUser 或当前 UI', async () => {
    let resolveUpdate!: (value: typeof profile) => void;
    mocks.updateUser.mockReturnValueOnce(new Promise((resolve) => { resolveUpdate = resolve; }));
    render(<MemoryRouter><ProfilePage /></MemoryRouter>);
    fireEvent.change(await screen.findByLabelText('邮箱'), { target: { value: 'old-session@example.com' } });
    fireEvent.click(screen.getByRole('button', { name: '保存资料' }));
    mocks.isSessionSnapshotCurrent.mockReturnValue(false);
    resolveUpdate({ ...profile, email: 'old-session@example.com' });
    await Promise.resolve();
    expect(mocks.setUser).not.toHaveBeenCalled();
  });
});
