import { describe, it, expect, vi, beforeEach } from 'vitest';

const mockFetch = vi.fn();
globalThis.fetch = mockFetch;

beforeEach(() => {
  mockFetch.mockReset();
  localStorage.clear();
});

describe('auth API', () => {
  it('login calls POST /api/auth/login', async () => {
    mockFetch.mockResolvedValueOnce({
      ok: true,
      status: 200,
      json: async () => ({
        token: 'abc',
        user: {
          user_id: 1,
          username: 'testuser',
          email: 'test@example.com',
          role: 1,
          vip_status: 'NONE',
          vip_expires_at: null,
          capabilities: ['USE_AUTHENTICATED_FEATURES'],
          created_at: '2026-01-02T03:04:05.000000Z',
        },
      }),
    });

    const { login } = await import('../../src/api/auth');
    const res = await login('testuser', 'pass123');

    expect(mockFetch).toHaveBeenCalledWith(
      expect.stringContaining('/api/auth/login'),
      expect.objectContaining({
        method: 'POST',
        body: expect.stringContaining('testuser'),
      }),
    );
    expect(res.token).toBe('abc');
    expect(res.user.role).toBe('NORMAL');
    expect(res.user.created_at).toBe('2026-01-02T03:04:05.000000Z');
  });

  it('register calls POST /api/auth/register', async () => {
    mockFetch.mockResolvedValueOnce({
      ok: true,
      status: 201,
      json: async () => ({
        token: 'def',
        user: {
          user_id: 2,
          username: 'newuser',
          email: 'a@b.com',
          role: '2',
          vip_status: 'ACTIVE',
          vip_expires_at: '2026-12-31T00:00:00.000000Z',
          capabilities: ['USE_AUTHENTICATED_FEATURES', 'USE_VIP_BENEFITS'],
          created_at: '2026-01-02T03:04:05.000000Z',
        },
      }),
    });

    const { register } = await import('../../src/api/auth');
    const res = await register('newuser', 'pass456', 'a@b.com');

    expect(mockFetch).toHaveBeenCalledWith(
      expect.stringContaining('/api/auth/register'),
      expect.objectContaining({
        method: 'POST',
        body: expect.stringContaining('newuser'),
      }),
    );
    expect(res.token).toBe('def');
    expect(res.user.role).toBe('VIP');
  });

  it('getMe 也归一化数字角色', async () => {
    mockFetch.mockResolvedValueOnce({
      ok: true,
      status: 200,
      json: async () => ({
        user_id: 3,
        username: 'music',
        email: '',
        role: 3,
        vip_status: 'NONE',
        vip_expires_at: null,
        capabilities: ['USE_AUTHENTICATED_FEATURES', 'MANAGE_USERS', 'DELETE_ANY_FILE'],
        created_at: '2026-01-02T03:04:05.000000Z',
      }),
    });

    const { getMe } = await import('../../src/api/auth');

    await expect(getMe()).resolves.toEqual(
      expect.objectContaining({
        user_id: 3,
        role: 'ADMIN',
        created_at: '2026-01-02T03:04:05.000000Z',
      }),
    );
  });
});
