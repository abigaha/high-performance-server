import { beforeEach, describe, expect, it, vi } from 'vitest';

const mockFetch = vi.fn();
globalThis.fetch = mockFetch;

beforeEach(() => {
  mockFetch.mockReset();
  localStorage.setItem('token', 'vip-token');
});

describe('vip API', () => {
  it('getVipPlans unwraps the plans envelope', async () => {
    const plans = [
      { duration_days: 30 as const, label: '30 天' },
      { duration_days: 90 as const, label: '90 天' },
      { duration_days: 365 as const, label: '365 天' },
    ];
    mockFetch.mockResolvedValueOnce({ ok: true, status: 200, json: async () => ({ plans }) });

    const { getVipPlans } = await import('../../src/api/vip');

    await expect(getVipPlans()).resolves.toEqual(plans);
    expect(mockFetch).toHaveBeenCalledWith(
      expect.stringContaining('/api/vip/plans'),
      expect.objectContaining({ headers: expect.objectContaining({ Authorization: 'Bearer vip-token' }) }),
    );
  });

  it('gets membership and activates or renews through the shared endpoint', async () => {
    const membership = {
      role: 'VIP' as const,
      vip_status: 'ACTIVE' as const,
      vip_expires_at: '2034-01-01T00:00:00.000000Z',
      server_now: '2033-01-01T00:00:00.000000Z',
      remaining_seconds: 31_536_000,
    };
    mockFetch
      .mockResolvedValueOnce({ ok: true, status: 200, json: async () => membership })
      .mockResolvedValueOnce({ ok: true, status: 200, json: async () => membership });

    const { activateVipMembership, getVipMembership } = await import('../../src/api/vip');

    await expect(getVipMembership()).resolves.toEqual(membership);
    await expect(activateVipMembership(365)).resolves.toEqual(membership);
    expect(mockFetch).toHaveBeenLastCalledWith(
      expect.stringContaining('/api/vip/membership/activate'),
      expect.objectContaining({ method: 'POST', body: JSON.stringify({ duration_days: 365 }) }),
    );
  });
});
