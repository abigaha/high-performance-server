import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import {
  ApiError,
  captureSessionSnapshot,
  handleUnauthorized,
  injectToast,
  injectUnauthorizedHandler,
  markSessionChanged,
  request,
} from '../../src/api/client';

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
  injectUnauthorizedHandler(null);
  window.history.replaceState({}, '', '/');
});

afterEach(() => {
  injectUnauthorizedHandler(null);
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

  it('ApiError 保留后端 code 字段', async () => {
    mockFetch.mockResolvedValueOnce(errorResponse(409, { code: 'PLAYLIST_DUPLICATE', error: '歌单已存在' }));

    await expect(request('/api/playlists')).rejects.toMatchObject({
      status: 409,
      code: 'PLAYLIST_DUPLICATE',
      message: '歌单已存在',
    });
  });

  it('ApiError 在响应仅含 code 时仍保留字符串 code', async () => {
    mockFetch.mockResolvedValueOnce(errorResponse(400, { code: 'INVALID_REQUEST' }));

    await expect(request('/api/files')).rejects.toMatchObject({
      status: 400,
      code: 'INVALID_REQUEST',
      message: '{"code":"INVALID_REQUEST"}',
    });
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
    const unauthorized = vi.fn();
    injectUnauthorizedHandler(unauthorized);
    localStorage.setItem('token', 'existing-token');
    window.history.replaceState({}, '', '/login');
    mockFetch.mockResolvedValueOnce(errorResponse(401, { error: '用户名或密码错误' }));

    await expect(request('/api/auth/login', { method: 'POST' })).rejects.toEqual(
      expect.objectContaining({ status: 401, message: '用户名或密码错误' }),
    );
    expect(localStorage.getItem('token')).toBe('existing-token');
    expect(window.location.pathname).toBe('/login');
    expect(unauthorized).not.toHaveBeenCalled();
  });

  it('注册 401 不触发全局未授权处理', async () => {
    const unauthorized = vi.fn();
    injectUnauthorizedHandler(unauthorized);
    localStorage.setItem('token', 'existing-token');
    mockFetch.mockResolvedValueOnce(errorResponse(401, { error: '注册失败' }));

    await expect(request('/api/auth/register', { method: 'POST' })).rejects.toMatchObject({ status: 401 });

    expect(localStorage.getItem('token')).toBe('existing-token');
    expect(unauthorized).not.toHaveBeenCalled();
  });

  it('普通接口 401 清理失效会话并保留后端原因', async () => {
    const unauthorized = vi.fn(() => {
      localStorage.removeItem('token');
      markSessionChanged();
    });
    injectUnauthorizedHandler(unauthorized);
    localStorage.setItem('token', 'expired-token');
    window.history.replaceState({}, '', '/login');
    mockFetch.mockResolvedValueOnce(errorResponse(401, { error: '需要登录' }));

    await expect(request('/api/files')).rejects.toEqual(
      expect.objectContaining({ status: 401, message: '需要登录' }),
    );
    expect(localStorage.getItem('token')).toBeNull();
    expect(unauthorized).toHaveBeenCalledTimes(1);
  });

  it('204 响应返回 undefined 且不解析 JSON', async () => {
    const json = vi.fn().mockRejectedValue(new Error('不应解析空响应'));
    mockFetch.mockResolvedValueOnce({ ok: true, status: 204, json } as unknown as Response);

    await expect(request<void>('/api/playlists/1', { method: 'DELETE' })).resolves.toBeUndefined();
    expect(json).not.toHaveBeenCalled();
  });

  it('raw 模式的 204 仍返回 undefined', async () => {
    mockFetch.mockResolvedValueOnce({ ok: true, status: 204 } as unknown as Response);

    await expect(request<void>('/api/no-content', {}, true)).resolves.toBeUndefined();
  });

  it('同一会话的并发 401 只触发一次处理', async () => {
    const unauthorized = vi.fn(() => {
      localStorage.removeItem('token');
      markSessionChanged();
      window.history.replaceState({}, '', '/login');
    });
    injectUnauthorizedHandler(unauthorized);
    localStorage.setItem('token', 'expired-token');
    window.history.replaceState({}, '', '/files');
    const revision = captureSessionSnapshot().revision;
    mockFetch
      .mockResolvedValueOnce(errorResponse(401, { error: '需要登录' }))
      .mockResolvedValueOnce(errorResponse(401, { error: '需要登录' }));

    const results = await Promise.allSettled([request('/api/files'), request('/api/music/library')]);

    expect(results.every((result) => result.status === 'rejected')).toBe(true);
    expect(unauthorized).toHaveBeenCalledTimes(1);
    expect(localStorage.getItem('token')).toBeNull();
    expect(captureSessionSnapshot().revision).toBe(revision + 1);
    expect(window.location.pathname).toBe('/login');
  });

  it('旧会话请求延迟返回 401 时不清理新会话', async () => {
    let resolveResponse!: (response: Response) => void;
    const pendingResponse = new Promise<Response>((resolve) => {
      resolveResponse = resolve;
    });
    const unauthorized = vi.fn();
    injectUnauthorizedHandler(unauthorized);
    localStorage.setItem('token', 'token-a');
    mockFetch.mockReturnValueOnce(pendingResponse);

    const staleRequest = request('/api/files');
    localStorage.setItem('token', 'token-b');
    resolveResponse(errorResponse(401, { error: '旧会话已失效' }));

    await expect(staleRequest).rejects.toMatchObject({ status: 401 });
    expect(localStorage.getItem('token')).toBe('token-b');
    expect(unauthorized).not.toHaveBeenCalled();
  });

  it('XHR 调用者传入旧请求 token 时不清理当前新会话', async () => {
    const unauthorized = vi.fn();
    injectUnauthorizedHandler(unauthorized);
    localStorage.setItem('token', 'token-a');
    const requestSession = captureSessionSnapshot();
    localStorage.setItem('token', 'token-b');
    markSessionChanged();

    handleUnauthorized('/api/files/upload', requestSession);
    await Promise.resolve();

    expect(localStorage.getItem('token')).toBe('token-b');
    expect(unauthorized).not.toHaveBeenCalled();
  });

  it('同 token 重新登录后旧 fetch 的 401 不清理新逻辑会话', async () => {
    let resolveResponse!: (response: Response) => void;
    const pendingResponse = new Promise<Response>((resolve) => {
      resolveResponse = resolve;
    });
    const unauthorized = vi.fn();
    injectUnauthorizedHandler(unauthorized);
    localStorage.setItem('token', 'same-token');
    markSessionChanged();
    mockFetch.mockReturnValueOnce(pendingResponse);

    const staleRequest = request('/api/files');
    localStorage.setItem('token', 'same-token');
    markSessionChanged();
    resolveResponse(errorResponse(401, { error: '旧会话已失效' }));

    await expect(staleRequest).rejects.toMatchObject({ status: 401 });
    expect(localStorage.getItem('token')).toBe('same-token');
    expect(unauthorized).not.toHaveBeenCalled();
  });

  it('同 token 重新登录后旧 XHR 的 401 不清理新逻辑会话', async () => {
    const unauthorized = vi.fn();
    injectUnauthorizedHandler(unauthorized);
    localStorage.setItem('token', 'same-token');
    markSessionChanged();
    const requestSession = captureSessionSnapshot();

    localStorage.setItem('token', 'same-token');
    markSessionChanged();
    handleUnauthorized('/api/files/upload', requestSession);
    await Promise.resolve();

    expect(localStorage.getItem('token')).toBe('same-token');
    expect(unauthorized).not.toHaveBeenCalled();
  });

  it('未授权回调内再次收到 401 不会递归调用回调', async () => {
    localStorage.setItem('token', 'expired-token');
    mockFetch
      .mockResolvedValueOnce(errorResponse(401, { error: '需要登录' }))
      .mockResolvedValueOnce(errorResponse(401, { error: '仍需登录' }));
    const unauthorized = vi.fn(async () => {
      localStorage.removeItem('token');
      markSessionChanged();
      await expect(request('/api/session/cleanup')).rejects.toMatchObject({ status: 401 });
    });
    injectUnauthorizedHandler(unauthorized);

    await expect(request('/api/files')).rejects.toMatchObject({ status: 401 });

    expect(unauthorized).toHaveBeenCalledTimes(1);
  });

  it('未授权回调失败不会掩盖原始 401', async () => {
    localStorage.setItem('token', 'expired-token');
    injectUnauthorizedHandler(vi.fn().mockRejectedValue(new Error('清理失败')));
    mockFetch.mockResolvedValueOnce(errorResponse(401, { error: '需要登录' }));

    await expect(request('/api/files')).rejects.toEqual(
      expect.objectContaining({ status: 401, message: '需要登录' }),
    );
  });

  it('后注入的 handler 替换旧 handler 且可清空', async () => {
    const first = vi.fn();
    const replacement = vi.fn();
    injectUnauthorizedHandler(first);
    injectUnauthorizedHandler(replacement);
    localStorage.setItem('token', 'first-session');
    mockFetch.mockResolvedValueOnce(errorResponse(401, { error: '需要登录' }));

    await expect(request('/api/files')).rejects.toMatchObject({ status: 401 });
    expect(first).not.toHaveBeenCalled();
    expect(replacement).toHaveBeenCalledTimes(1);

    injectUnauthorizedHandler(null);
    localStorage.setItem('token', 'second-session');
    mockFetch.mockResolvedValueOnce(errorResponse(401, { error: '需要登录' }));
    await expect(request('/api/files')).rejects.toMatchObject({ status: 401 });
    expect(replacement).toHaveBeenCalledTimes(1);
  });

  it('HMR 替换 handler 后旧 generation 的 finally 不覆盖新 handler 状态', async () => {
    let release!: () => void;
    const pending = new Promise<void>((resolve) => {
      release = resolve;
    });
    const first = vi.fn(() => pending);
    const replacement = vi.fn();
    injectUnauthorizedHandler(first);
    localStorage.setItem('token', 'same-token');
    mockFetch
      .mockResolvedValueOnce(errorResponse(401, { error: '需要登录' }))
      .mockResolvedValueOnce(errorResponse(401, { error: '需要登录' }));

    const firstRequest = request('/api/files');
    await vi.waitFor(() => expect(first).toHaveBeenCalledTimes(1));
    injectUnauthorizedHandler(replacement);
    localStorage.setItem('token', 'same-token');
    markSessionChanged();
    const secondRequest = request('/api/music/library');
    const requestsSettled = Promise.allSettled([firstRequest, secondRequest]);
    await vi.waitFor(() => expect(replacement).toHaveBeenCalledTimes(1));
    release();

    await requestsSettled;
    localStorage.setItem('token', 'third-session');
    markSessionChanged();
    mockFetch.mockResolvedValueOnce(errorResponse(401, { error: '需要登录' }));
    await expect(request('/api/playlists')).rejects.toMatchObject({ status: 401 });
    expect(replacement).toHaveBeenCalledTimes(2);
  });

  it('handler 卸载后旧 generation 的 finally 不恢复已卸载 handler', async () => {
    let release!: () => void;
    const pending = new Promise<void>((resolve) => {
      release = resolve;
    });
    const handler = vi.fn(() => pending);
    injectUnauthorizedHandler(handler);
    localStorage.setItem('token', 'first-session');
    markSessionChanged();
    mockFetch.mockResolvedValueOnce(errorResponse(401, { error: '需要登录' }));

    const firstRequest = request('/api/files');
    await vi.waitFor(() => expect(handler).toHaveBeenCalledTimes(1));
    injectUnauthorizedHandler(null);
    release();
    await expect(firstRequest).rejects.toMatchObject({ status: 401 });

    localStorage.setItem('token', 'second-session');
    markSessionChanged();
    mockFetch.mockResolvedValueOnce(errorResponse(401, { error: '需要登录' }));
    await expect(request('/api/music/library')).rejects.toMatchObject({ status: 401 });
    expect(handler).toHaveBeenCalledTimes(1);
  });
});
