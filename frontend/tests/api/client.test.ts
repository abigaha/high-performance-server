import { beforeEach, describe, expect, it, vi } from 'vitest';
import { ApiError, injectToast, request } from '../../src/api/client';

const mockFetch = vi.fn();
globalThis.fetch = mockFetch;

function errorResponse(status: number, payload: unknown): Response {
  const body = typeof payload === 'string' ? payload : JSON.stringify(payload);
  return {
    ok: false,
    status,
    text: vi.fn().mockResolvedValue(body),
  } as unknown as Response;
}

beforeEach(() => {
  mockFetch.mockReset();
  localStorage.clear();
  injectToast(null);
  window.history.replaceState({}, '', '/');
});

describe('API 客户端错误处理', () => {
  it('优先抛出后端 error 字段并用于错误提示', async () => {
    const toastError = vi.fn();
    injectToast({ error: toastError, success: vi.fn() });
    mockFetch.mockResolvedValueOnce(errorResponse(400, { error: '用户名已存在' }));

    const result = request('/api/auth/register', { method: 'POST' });

    await expect(result).rejects.toEqual(expect.objectContaining({
      status: 400,
      message: '用户名已存在',
    }));
    expect(toastError).toHaveBeenCalledWith('用户名已存在');
  });

  it('后端仅提供 message 字段时保留其详细信息', async () => {
    mockFetch.mockResolvedValueOnce(errorResponse(500, { message: '服务暂时不可用' }));

    await expect(request('/api/files')).rejects.toEqual(expect.objectContaining({
      status: 500,
      message: '服务暂时不可用',
    }));
  });

  it('保留纯文本错误正文', async () => {
    mockFetch.mockResolvedValueOnce(errorResponse(502, '上游服务暂时不可用'));

    await expect(request('/api/files')).rejects.toEqual(
      new ApiError(502, '上游服务暂时不可用'),
    );
  });

  it('从 HTML 错误页提取可读正文并忽略脚本', async () => {
    mockFetch.mockResolvedValueOnce(errorResponse(
      503,
      '<!doctype html><html><body><h1>服务维护中</h1><p>请稍后重试</p><script>ignore()</script></body></html>',
    ));

    await expect(request('/api/files')).rejects.toEqual(
      new ApiError(503, '服务维护中请稍后重试'),
    );
  });

  it('错误正文无法读取时回退到状态码提示', async () => {
    mockFetch.mockResolvedValueOnce({
      ok: false,
      status: 502,
      text: vi.fn().mockRejectedValue(new Error('body unavailable')),
    } as unknown as Response);

    await expect(request('/api/files')).rejects.toEqual(
      new ApiError(502, '请求失败 (502)'),
    );
  });

  it('登录 401 保留现有会话和后端原因且不触发重定向', async () => {
    localStorage.setItem('token', 'existing-token');
    window.history.replaceState({}, '', '/login');
    mockFetch.mockResolvedValueOnce(errorResponse(401, { error: '用户名或密码错误' }));

    await expect(request('/api/auth/login', { method: 'POST' })).rejects.toEqual(
      expect.objectContaining({ status: 401, message: '用户名或密码错误' }),
    );
    expect(localStorage.getItem('token')).toBe('existing-token');
    expect(window.location.pathname).toBe('/login');
  });

  it('普通接口 401 清理失效会话并保留后端原因', async () => {
    localStorage.setItem('token', 'expired-token');
    window.history.replaceState({}, '', '/login');
    mockFetch.mockResolvedValueOnce(errorResponse(401, { error: '需要登录' }));

    await expect(request('/api/files')).rejects.toEqual(
      expect.objectContaining({ status: 401, message: '需要登录' }),
    );
    expect(localStorage.getItem('token')).toBeNull();
  });
});
