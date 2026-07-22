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
      json: async () => ({ token: 'abc', user_id: 1, role: 1 }),
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
    expect(res.role).toBe('NORMAL');
  });

  it('register calls POST /api/auth/register', async () => {
    mockFetch.mockResolvedValueOnce({
      ok: true,
      json: async () => ({ token: 'def', user_id: 2, role: '2' }),
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
    expect(res.role).toBe('VIP');
  });

  it('getMe 也归一化数字角色', async () => {
    mockFetch.mockResolvedValueOnce({
      ok: true,
      json: async () => ({ user_id: 3, username: 'music', email: '', role: 2 }),
    });

    const { getMe } = await import('../../src/api/auth');

    await expect(getMe()).resolves.toEqual(
      expect.objectContaining({ user_id: 3, role: 'VIP' }),
    );
  });
});
