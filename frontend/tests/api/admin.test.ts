import { beforeEach, describe, expect, it, vi } from 'vitest';

const mockFetch = vi.fn();
globalThis.fetch = mockFetch;

beforeEach(() => {
  mockFetch.mockReset();
  localStorage.setItem('token', 'admin-token');
});

describe('admin API', () => {
  const row = {
    user_id: 9,
    username: '特殊用户',
    email: 'mail+tag@example.com',
    role: 'VIP' as const,
    vip_status: 'ACTIVE' as const,
    vip_expires_at: '2034-01-01T00:00:00.000000Z',
    created_at: '2026-01-02T03:04:05.000000Z',
  };

  it('encodes filtering and pagination and returns the complete page', async () => {
    const page = { items: [row], total: 1, offset: 2, limit: 5 };
    mockFetch.mockResolvedValueOnce({ ok: true, status: 200, json: async () => page });

    const { getAdminUsers } = await import('../../src/api/admin');

    await expect(getAdminUsers({ q: 'mail+tag@example.com', offset: 2, limit: 5 })).resolves.toEqual(page);
    expect(mockFetch).toHaveBeenCalledWith(
      expect.stringContaining('/api/admin/users?q=mail%2Btag%40example.com&offset=2&limit=5'),
      expect.anything(),
    );
  });

  it('grant, renew and revoke return complete server rows', async () => {
    const revoked = { ...row, role: 'NORMAL' as const, vip_status: 'NONE' as const, vip_expires_at: null };
    mockFetch
      .mockResolvedValueOnce({ ok: true, status: 200, json: async () => row })
      .mockResolvedValueOnce({ ok: true, status: 200, json: async () => row })
      .mockResolvedValueOnce({ ok: true, status: 200, json: async () => revoked });

    const { grantUserVip, revokeUserVip } = await import('../../src/api/admin');

    await expect(grantUserVip(9, 30)).resolves.toEqual(row);
    await expect(grantUserVip(9, 90)).resolves.toEqual(row);
    await expect(revokeUserVip(9)).resolves.toEqual(revoked);
    expect(mockFetch).toHaveBeenNthCalledWith(
      1,
      expect.stringContaining('/api/admin/users/9/vip'),
      expect.objectContaining({ method: 'POST', body: JSON.stringify({ duration_days: 30 }) }),
    );
    expect(mockFetch).toHaveBeenLastCalledWith(
      expect.stringContaining('/api/admin/users/9/vip'),
      expect.objectContaining({ method: 'DELETE' }),
    );
  });
});
