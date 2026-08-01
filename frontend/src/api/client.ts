export const API_BASE = (import.meta.env.VITE_API_URL ?? '').replace(/\/$/, '');

export class ApiError extends Error {
  status: number;
  code: string | undefined;
  constructor(status: number, message: string, code?: string) {
    super(message);
    this.status = status;
    this.code = code;
  }
}

type ToastFn = (msg: string) => void;
type UnauthorizedHandler = () => void | Promise<void>;
export interface SessionSnapshot {
  token: string | null;
  revision: number;
}

let globalToast: { error: ToastFn; success: ToastFn } | null = null;
let unauthorizedHandler: UnauthorizedHandler | null = null;
let sessionRevision = 0;
let handlerGeneration = 0;
let unauthorizedState = {
  generation: handlerGeneration,
  inProgress: false,
  handledRevision: undefined as number | undefined,
};

export function markSessionChanged(): number {
  sessionRevision += 1;
  return sessionRevision;
}

export function getSessionRevision(): number {
  return sessionRevision;
}

export function captureSessionSnapshot(): SessionSnapshot {
  return { token: localStorage.getItem('token'), revision: sessionRevision };
}

export function isSessionSnapshotCurrent(snapshot: SessionSnapshot): boolean {
  return snapshot.revision === sessionRevision && snapshot.token === localStorage.getItem('token');
}

export function injectToast(toast: typeof globalToast) {
  globalToast = toast;
}

export function injectUnauthorizedHandler(handler: UnauthorizedHandler | null): void {
  unauthorizedHandler = handler;
  handlerGeneration += 1;
  unauthorizedState = {
    generation: handlerGeneration,
    inProgress: false,
    handledRevision: undefined,
  };
}

function isAuthenticationRequest(path: string): boolean {
  const pathname = path.split('?', 1)[0];
  return pathname === '/api/auth/login' || pathname === '/api/auth/register';
}

async function notifyUnauthorized(path: string, requestSession: SessionSnapshot): Promise<void> {
  if (isAuthenticationRequest(path)) return;

  const state = unauthorizedState;
  if (state.inProgress) return;
  if (!requestSession.token || !isSessionSnapshotCurrent(requestSession)) return;
  if (state.handledRevision === requestSession.revision) return;

  state.handledRevision = requestSession.revision;
  state.inProgress = true;
  const handler = unauthorizedHandler;
  try {
    await handler?.();
  } catch {
    // 会话清理失败不能改变原请求的 401 语义。
  } finally {
    if (unauthorizedState === state && unauthorizedState.generation === state.generation) {
      unauthorizedState.inProgress = false;
    }
  }
}

export function handleUnauthorized(path: string, requestSession = captureSessionSnapshot()): void {
  void notifyUnauthorized(path, requestSession);
}

export function parseApiErrorDetails(body: string, fallback: string): { message: string; code?: string } {
  const text = body.trim();
  if (!text) return { message: fallback };

  try {
    const payload: unknown = JSON.parse(text);
    if (payload && typeof payload === 'object') {
      const { code, error, message } = payload as Record<string, unknown>;
      const responseCode = typeof code === 'string' ? code : undefined;
      const detail = [error, message].find(
        (value): value is string => typeof value === 'string' && value.trim().length > 0,
      );
      if (detail) return { message: detail.trim(), code: responseCode };
      if (responseCode !== undefined) return { message: text, code: responseCode };
    }
    if (typeof payload === 'string' && payload.trim()) return { message: payload.trim() };
  } catch {
    // 非 JSON 响应继续按纯文本或 HTML 提取可读信息。
  }

  if (/<(?:!doctype|html|head|body|title|h[1-6]|p|div)\b/i.test(text)) {
    const document = new DOMParser().parseFromString(text, 'text/html');
    document.querySelectorAll('script, style, noscript').forEach((element) => element.remove());
    const htmlMessage = document.body.textContent?.replace(/\s+/g, ' ').trim();
    if (htmlMessage) return { message: htmlMessage };
  }

  return { message: text };
}

async function readErrorDetails(res: Response, fallback: string): Promise<{ message: string; code?: string }> {
  try {
    return parseApiErrorDetails(await res.text(), fallback);
  } catch {
    return { message: fallback };
  }
}

export async function request<T>(
  path: string,
  options: RequestInit = {},
  raw?: boolean,
): Promise<T> {
  const requestSession = captureSessionSnapshot();
  const token = requestSession.token;
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
    const detail = await readErrorDetails(res, fallback);

    if (res.status === 401) {
      await notifyUnauthorized(path, requestSession);
      throw new ApiError(401, detail.message, detail.code);
    }
    if (res.status === 403) {
      globalToast?.error(detail.message);
      throw new ApiError(403, detail.message, detail.code);
    }

    globalToast?.error(detail.message);
    throw new ApiError(res.status, detail.message, detail.code);
  }
  if (res.status === 204) return undefined as T;
  if (raw) return res as unknown as T;
  return res.json();
}
