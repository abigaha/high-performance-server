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
  if (res.status === 401) {
    localStorage.removeItem('token');
    window.location.href = '/login';
    throw new ApiError(401, '未登录');
  }
  if (res.status === 403) {
    const msg = '权限不足';
    globalToast?.error(msg);
    throw new ApiError(403, msg);
  }
  if (!res.ok) {
    const msg = `请求失败 (${res.status})`;
    globalToast?.error(msg);
    throw new ApiError(res.status, msg);
  }
  if (raw) return res as unknown as T;
  return res.json();
}
