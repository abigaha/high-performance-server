import { act, fireEvent, render, screen } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import { beforeEach, describe, expect, it, vi } from 'vitest';

const mocks = vi.hoisted(() => ({
  getVipPlans: vi.fn(),
  getVipMembership: vi.fn(),
  activateVipMembership: vi.fn(),
  getUser: vi.fn(),
  setUser: vi.fn(),
  captureSessionSnapshot: vi.fn(),
  isSessionSnapshotCurrent: vi.fn(),
}));
const auth = vi.hoisted(() => ({ user: { user_id: 7, role: 'NORMAL' }, sessionRevision: 1, setUser: vi.fn() }));

vi.mock('../../src/api/vip', () => ({
  getVipPlans: mocks.getVipPlans,
  getVipMembership: mocks.getVipMembership,
  activateVipMembership: mocks.activateVipMembership,
}));
vi.mock('../../src/api/users', () => ({ getUser: mocks.getUser }));
vi.mock('../../src/api/client', () => ({
  captureSessionSnapshot: mocks.captureSessionSnapshot,
  isSessionSnapshotCurrent: mocks.isSessionSnapshotCurrent,
}));
vi.mock('../../src/stores/auth', () => ({
  useAuthStore: (selector: (state: unknown) => unknown) => selector({ user: auth.user, sessionRevision: auth.sessionRevision, setUser: mocks.setUser }),
}));

import VipCenterPage from '../../src/pages/VipCenterPage';

describe('VipCenterPage', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    vi.useFakeTimers();
    vi.setSystemTime(new Date('2030-01-01T00:00:00Z'));
    mocks.captureSessionSnapshot.mockReturnValue({ token: 'token', revision: 1 });
    mocks.isSessionSnapshotCurrent.mockReturnValue(true);
    auth.user = { user_id: 7, role: 'NORMAL' };
    auth.sessionRevision = 1;
    mocks.getVipPlans.mockResolvedValue([{ duration_days: 30, label: '30 天' }]);
    mocks.getVipMembership.mockResolvedValue({
      role: 'VIP', vip_status: 'ACTIVE', vip_expires_at: '2030-01-01T00:02:00Z',
      server_now: '2030-01-01T00:01:00Z', remaining_seconds: 999,
    });
  });

  it('按 NTP midpoint 校准 5 秒 RTT，不把完整 RTT 加到 server timestamp', async () => {
    let resolveMembership!: (value: unknown) => void;
    mocks.getVipMembership.mockReturnValueOnce(new Promise((resolve) => { resolveMembership = resolve; }));
    render(<MemoryRouter><VipCenterPage /></MemoryRouter>);
    await act(async () => vi.advanceTimersByTime(5_000));
    resolveMembership({
      role: 'VIP', vip_status: 'ACTIVE', vip_expires_at: '2030-01-01T00:01:02Z',
      server_now: '2030-01-01T00:00:02.500Z', remaining_seconds: 60,
    });
    await act(async () => {});
    expect(screen.getByText('00:00:57')).toBeInTheDocument();
  });

  it('membership 立即完成而 plans 延迟 10 秒时扣除渲染前经过时间', async () => {
    let resolvePlans!: (value: Array<{ duration_days: 30; label: string }>) => void;
    mocks.getVipPlans.mockReturnValueOnce(new Promise((resolve) => { resolvePlans = resolve; }));
    mocks.getVipMembership.mockResolvedValueOnce({
      role: 'VIP', vip_status: 'ACTIVE', vip_expires_at: '2030-01-01T00:01:00Z',
      server_now: '2030-01-01T00:00:00Z', remaining_seconds: 60,
    });

    render(<MemoryRouter><VipCenterPage /></MemoryRouter>);
    await act(async () => {});
    await act(async () => vi.advanceTimersByTime(10_000));
    resolvePlans([{ duration_days: 30, label: '30 天' }]);
    await act(async () => {});

    expect(screen.getByText('00:00:50')).toBeInTheDocument();
  });

  it('membership 慢响应只按自身 RTT midpoint 校准', async () => {
    let resolveMembership!: (value: unknown) => void;
    mocks.getVipMembership.mockReturnValueOnce(new Promise((resolve) => { resolveMembership = resolve; }));
    render(<MemoryRouter><VipCenterPage /></MemoryRouter>);
    await act(async () => vi.advanceTimersByTime(10_000));
    resolveMembership({
      role: 'VIP', vip_status: 'ACTIVE', vip_expires_at: '2030-01-01T00:01:05Z',
      server_now: '2030-01-01T00:00:05Z', remaining_seconds: 60,
    });
    await act(async () => {});

    expect(screen.getByText('00:00:55')).toBeInTheDocument();
  });

  it('校准后 Date.now 跳变不改变倒计时，performance 推进才会改变', async () => {
    let monotonicNow = 0;
    vi.spyOn(performance, 'now').mockImplementation(() => monotonicNow);
    render(<MemoryRouter><VipCenterPage /></MemoryRouter>);
    await act(async () => {});
    expect(screen.getByText('00:01:00')).toBeInTheDocument();

    vi.setSystemTime(new Date('2040-01-01T00:00:00Z'));
    monotonicNow = 1_000;
    await act(async () => vi.advanceTimersByTime(1_000));
    expect(screen.getByText('00:00:59')).toBeInTheDocument();

    vi.setSystemTime(new Date('2020-01-01T00:00:00Z'));
    monotonicNow = 2_000;
    await act(async () => vi.advanceTimersByTime(1_000));
    expect(screen.getByText('00:00:58')).toBeInTheDocument();
  });

  it('加载失败显示显式 error 与 retry，重试后进入 ready', async () => {
    mocks.getVipMembership.mockRejectedValueOnce(new Error('会员服务不可用'));
    render(<MemoryRouter><VipCenterPage /></MemoryRouter>);
    expect(screen.getByRole('status')).toHaveTextContent('正在加载会员信息');
    await act(async () => {});
    expect(screen.getByRole('alert')).toHaveTextContent('会员服务不可用');
    mocks.getVipMembership.mockResolvedValueOnce({
      role: 'NORMAL', vip_status: 'NONE', vip_expires_at: null,
      server_now: '2030-01-01T00:00:00Z', remaining_seconds: 0,
    });
    fireEvent.click(screen.getByRole('button', { name: '重试会员信息' }));
    await act(async () => {});
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
    expect(screen.getAllByText('未开通')).toHaveLength(2);
  });

  it('session 用户变化会使旧 op 失效、清 busy/error 并重新加载', async () => {
    mocks.getVipMembership.mockResolvedValueOnce({
      role: 'NORMAL', vip_status: 'NONE', vip_expires_at: null,
      server_now: '2030-01-01T00:00:00Z', remaining_seconds: 0,
    });
    let resolveActivate!: (value: unknown) => void;
    mocks.activateVipMembership.mockReturnValueOnce(new Promise((resolve) => { resolveActivate = resolve; }));
    const view = render(<MemoryRouter><VipCenterPage /></MemoryRouter>);
    await act(async () => {});
    fireEvent.click(screen.getByRole('button', { name: '激活 30 天会员' }));
    expect(screen.getByRole('button', { name: '处理中' })).toBeDisabled();
    auth.user = { user_id: 8, role: 'NORMAL' };
    mocks.getVipMembership.mockResolvedValueOnce({
      role: 'NORMAL', vip_status: 'NONE', vip_expires_at: null,
      server_now: '2030-01-01T00:00:00Z', remaining_seconds: 0,
    });
    view.rerender(<MemoryRouter><VipCenterPage /></MemoryRouter>);
    await act(async () => {});
    expect(screen.getByRole('button', { name: '激活 30 天会员' })).toBeEnabled();
    resolveActivate({
      role: 'VIP', vip_status: 'ACTIVE', vip_expires_at: '2030-02-01T00:00:00Z',
      server_now: '2030-01-01T00:00:00Z', remaining_seconds: 2_678_400,
    });
    await act(async () => {});
    expect(screen.queryByText(/2030-02-01/)).not.toBeInTheDocument();
  });

  it('相同 user_id 的 session revision 变化也会失效旧 op 并重载', async () => {
    mocks.getVipMembership.mockResolvedValueOnce({
      role: 'NORMAL', vip_status: 'NONE', vip_expires_at: null,
      server_now: '2030-01-01T00:00:00Z', remaining_seconds: 0,
    });
    let resolveActivate!: (value: unknown) => void;
    mocks.activateVipMembership.mockReturnValueOnce(new Promise((resolve) => { resolveActivate = resolve; }));
    const view = render(<MemoryRouter><VipCenterPage /></MemoryRouter>);
    await act(async () => {});
    fireEvent.click(screen.getByRole('button', { name: '激活 30 天会员' }));
    auth.sessionRevision = 2;
    mocks.getVipMembership.mockResolvedValueOnce({
      role: 'NORMAL', vip_status: 'NONE', vip_expires_at: null,
      server_now: '2030-01-01T00:00:00Z', remaining_seconds: 0,
    });
    view.rerender(<MemoryRouter><VipCenterPage /></MemoryRouter>);
    await act(async () => {});
    expect(mocks.getVipMembership).toHaveBeenCalledTimes(2);
    expect(screen.getByRole('button', { name: '激活 30 天会员' })).toBeEnabled();
    resolveActivate({ role: 'VIP', vip_status: 'ACTIVE', vip_expires_at: '2030-02-01T00:00:00Z', server_now: '2030-01-01T00:00:00Z', remaining_seconds: 1 });
    await act(async () => {});
    expect(screen.queryByText(/2030-02-01/)).not.toBeInTheDocument();
  });

  it('归零后停止 interval，显示已过期并保持续期语义', async () => {
    mocks.getVipMembership.mockResolvedValueOnce({
      role: 'VIP', vip_status: 'ACTIVE', vip_expires_at: '2030-01-01T00:00:02Z',
      server_now: '2030-01-01T00:00:00Z', remaining_seconds: 2,
    });
    const clearInterval = vi.spyOn(window, 'clearInterval');
    render(<MemoryRouter><VipCenterPage /></MemoryRouter>);
    await act(async () => {});
    await act(async () => vi.advanceTimersByTime(2_000));
    expect(screen.getByText('已过期')).toBeInTheDocument();
    expect(screen.getByRole('button', { name: '续期 30 天会员' })).toBeInTheDocument();
    expect(clearInterval).toHaveBeenCalled();
    const tickCount = vi.getTimerCount();
    await act(async () => vi.advanceTimersByTime(5_000));
    expect(vi.getTimerCount()).toBe(tickCount);
  });

  it('unmount 清理倒计时 interval', async () => {
    const clearInterval = vi.spyOn(window, 'clearInterval');
    const view = render(<MemoryRouter><VipCenterPage /></MemoryRouter>);
    await act(async () => {});
    view.unmount();
    expect(clearInterval).toHaveBeenCalled();
  });

  it('切换账号后静默丢弃旧 activate 和 refresh 成功响应', async () => {
    mocks.getVipMembership.mockResolvedValueOnce({
      role: 'NORMAL', vip_status: 'NONE', vip_expires_at: null,
      server_now: '2030-01-01T00:00:00Z', remaining_seconds: 0,
    });
    let resolveActivate!: (value: unknown) => void;
    mocks.activateVipMembership.mockReturnValueOnce(new Promise((resolve) => { resolveActivate = resolve; }));
    render(<MemoryRouter><VipCenterPage /></MemoryRouter>);
    await act(async () => {});
    fireEvent.click(screen.getByRole('button', { name: '激活 30 天会员' }));
    mocks.isSessionSnapshotCurrent.mockReturnValue(false);
    resolveActivate({
      role: 'VIP', vip_status: 'ACTIVE', vip_expires_at: '2030-02-01T00:00:00Z',
      server_now: '2030-01-01T00:00:00Z', remaining_seconds: 2_678_400,
    });
    await act(async () => {});
    expect(mocks.getUser).not.toHaveBeenCalled();
    expect(mocks.setUser).not.toHaveBeenCalled();
    expect(screen.queryByText(/2030-02-01/)).not.toBeInTheDocument();
  });

  it('activate 成功后切换账号时丢弃旧 refresh 响应', async () => {
    mocks.getVipMembership.mockResolvedValueOnce({
      role: 'NORMAL', vip_status: 'NONE', vip_expires_at: null,
      server_now: '2030-01-01T00:00:00Z', remaining_seconds: 0,
    });
    mocks.activateVipMembership.mockResolvedValueOnce({
      role: 'VIP', vip_status: 'ACTIVE', vip_expires_at: '2030-02-01T00:00:00Z',
      server_now: '2030-01-01T00:00:00Z', remaining_seconds: 2_678_400,
    });
    let resolveUser!: (value: unknown) => void;
    mocks.getUser.mockReturnValueOnce(new Promise((resolve) => { resolveUser = resolve; }));
    render(<MemoryRouter><VipCenterPage /></MemoryRouter>);
    await act(async () => {});
    fireEvent.click(screen.getByRole('button', { name: '激活 30 天会员' }));
    await act(async () => {});
    mocks.isSessionSnapshotCurrent.mockReturnValue(false);
    resolveUser({ user_id: 7, username: 'old-user', role: 'VIP' });
    await act(async () => {});
    expect(mocks.setUser).not.toHaveBeenCalled();
    expect(screen.queryByText(/2030-02-01/)).not.toBeInTheDocument();
  });

  it('按 server_now 校准倒计时，而不是使用浏览器与到期时间的差值', async () => {
    render(<MemoryRouter><VipCenterPage /></MemoryRouter>);
    await act(async () => {});
    expect(screen.getByText('00:01:00')).toBeInTheDocument();
    await act(async () => vi.advanceTimersByTime(1_000));
    expect(screen.getByText('00:00:59')).toBeInTheDocument();
    expect(screen.getByText(/2030-01-01 00:02:00 UTC/)).toBeInTheDocument();
  });

  it('激活后采用完整 membership，并刷新 AuthUser', async () => {
    mocks.getVipMembership.mockResolvedValueOnce({
      role: 'NORMAL', vip_status: 'NONE', vip_expires_at: null,
      server_now: '2030-01-01T00:00:00Z', remaining_seconds: 0,
    });
    const membership = {
      role: 'VIP', vip_status: 'ACTIVE', vip_expires_at: '2030-02-01T00:00:00Z',
      server_now: '2030-01-01T00:00:00Z', remaining_seconds: 2_678_400,
    };
    const authUser = { user_id: 7, username: 'alice', role: 'VIP', created_at: '2029-01-01T00:00:00Z' };
    mocks.activateVipMembership.mockResolvedValue(membership);
    mocks.getUser.mockResolvedValue(authUser);

    render(<MemoryRouter><VipCenterPage /></MemoryRouter>);
    await act(async () => {});
    fireEvent.click(screen.getByRole('button', { name: '激活 30 天会员' }));

    await act(async () => {});
    expect(mocks.activateVipMembership).toHaveBeenCalledWith(30);
    expect(mocks.getUser).toHaveBeenCalledWith(7);
    expect(mocks.setUser).toHaveBeenCalledWith(authUser);
    expect(screen.getByText(/2030-02-01 00:00:00 UTC/)).toBeInTheDocument();
  });
});
