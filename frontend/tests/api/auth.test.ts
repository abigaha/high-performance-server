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
      json: async () => ({ token: 'abc', user_id: 1, role: 'NORMAL' }),
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
      json: async () => ({ token: 'def', user_id: 2, role: 'VIP' }),
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
  });
});
