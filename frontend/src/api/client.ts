export const API_BASE = (import.meta.env.VITE_API_URL ?? '').replace(/\/$/, '');

export class ApiError extends Error {
  status: number;
  constructor(status: number, message: string) {
    super(message);
    this.status = status;
  }
}

type ToastFn = (msg: string) => void;
let globalToast: { error: ToastFn; success: ToastFn } | null = null;

export function injectToast(toast: typeof globalToast) {
  globalToast = toast;
}

export function handleUnauthorized(path: string): void {
  const isLoginRequest = path.split('?', 1)[0] === '/api/auth/login';
  if (isLoginRequest) return;

  localStorage.removeItem('token');
  if (window.location.pathname !== '/login') {
    window.location.href = '/login';
  }
}

async function readErrorMessage(res: Response, fallback: string): Promise<string> {
  let body: string;
  try {
    body = (await res.text()).trim();
  } catch {
    return fallback;
  }
  if (!body) return fallback;

  try {
    const payload: unknown = JSON.parse(body);
    if (payload && typeof payload === 'object') {
      const { error, message } = payload as Record<string, unknown>;
      const detail = [error, message].find(
        (value): value is string => typeof value === 'string' && value.trim().length > 0,
      );
      if (detail) return detail.trim();
    }
    if (typeof payload === 'string' && payload.trim()) return payload.trim();
  } catch {
    // 非 JSON 响应继续按纯文本或 HTML 提取可读信息。
  }

  if (/<(?:!doctype|html|head|body|title|h[1-6]|p|div)\b/i.test(body)) {
    const document = new DOMParser().parseFromString(body, 'text/html');
    document.querySelectorAll('script, style, noscript').forEach((element) => element.remove());
    const htmlMessage = document.body.textContent?.replace(/\s+/g, ' ').trim();
    if (htmlMessage) return htmlMessage;
  }

  return body;
}

export async function request<T>(
  path: string,
  options: RequestInit = {},
  raw?: boolean,
): Promise<T> {
  const token = localStorage.getItem('token');
  const headers: Record<string, string> = {
    ...(options.body && !(options.body instanceof FormData)
      ? { 'Content-Type': 'application/json' }
      : {}),
    ...(token ? { Authorization: `Bearer ${token}` } : {}),
    ...(options.headers as Record<string, string> || {}),
  };
  const res = await fetch(`${API_BASE}${path}`, { ...options, headers });
  if (!res.ok) {
    const fallback = res.status === 401
      ? '未登录'
      : res.status === 403
        ? '权限不足'
        : `请求失败 (${res.status})`;
    const msg = await readErrorMessage(res, fallback);

    if (res.status === 401) {
      handleUnauthorized(path);
      throw new ApiError(401, msg);
    }
    if (res.status === 403) {
      globalToast?.error(msg);
      throw new ApiError(403, msg);
    }

    globalToast?.error(msg);
    throw new ApiError(res.status, msg);
  }
  if (raw) return res as unknown as T;
  return res.json();
}
