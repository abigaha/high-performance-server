import { API_BASE, ApiError, handleUnauthorized, request } from './client';
import { getContentType } from '../lib/uploadPolicy';
import type {
  FileRecord,
  PaginatedResponse,
  FileQuery,
  FileSearchQuery,
  UploadResult,
} from '../types/api';

export async function getFiles(query?: FileQuery): Promise<PaginatedResponse<FileRecord>> {
  const params = new URLSearchParams();
  if (query?.name) params.set('name', query.name);
  if (query?.type) params.set('type', query.type);
  if (query?.offset !== undefined) params.set('offset', String(query.offset));
  if (query?.limit !== undefined) params.set('limit', String(query.limit));
  const qs = params.toString();
  return request<PaginatedResponse<FileRecord>>(`/api/files${qs ? `?${qs}` : ''}`);
}

export async function getFile(id: number): Promise<FileRecord> {
  return request<FileRecord>(`/api/files/${id}`);
}

export async function getFileDownloadUrl(id: number): Promise<string> {
  const response = await request<Response>(`/api/files/${id}/download`, {}, true);
  const url = URL.createObjectURL(await response.blob());
  window.setTimeout(() => URL.revokeObjectURL(url), 60_000);
  return url;
}

export async function getFileStreamUrl(id: number): Promise<string> {
  const response = await request<Response>(
    `/api/files/${id}/stream`,
    { headers: { Accept: 'audio/*' } },
    true,
  );
  return URL.createObjectURL(await response.blob());
}

export async function uploadFile(
  file: File,
  onProgress?: (pct: number) => void,
  signal?: AbortSignal,
): Promise<UploadResult> {
  if (signal?.aborted) {
    throw new DOMException('上传已取消', 'AbortError');
  }

  const token = localStorage.getItem('token');
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    const path = '/api/files/upload';
    let settled = false;

    const cleanup = () => signal?.removeEventListener('abort', abortUpload);
    const resolveOnce = (result: UploadResult) => {
      if (settled) return;
      settled = true;
      cleanup();
      resolve(result);
    };
    const rejectOnce = (error: Error) => {
      if (settled) return;
      settled = true;
      cleanup();
      reject(error);
    };
    const abortUpload = () => xhr.abort();

    xhr.open('POST', `${API_BASE}${path}`);
    if (token) xhr.setRequestHeader('Authorization', `Bearer ${token}`);
    xhr.setRequestHeader('Content-Type', getContentType(file));
    xhr.setRequestHeader('Content-Disposition', buildContentDisposition(file.name));
    xhr.upload.onprogress = (e) => {
      if (e.lengthComputable && onProgress) {
        onProgress(Math.round((e.loaded / e.total) * 100));
      }
    };
    xhr.onload = () => {
      if (xhr.status >= 200 && xhr.status < 300) {
        try {
          resolveOnce(JSON.parse(xhr.responseText) as UploadResult);
        } catch {
          rejectOnce(new ApiError(xhr.status, '服务器返回了无法解析的上传结果'));
        }
        return;
      }

      const fallback = xhr.status === 401
        ? '未登录'
        : xhr.status === 403
          ? '权限不足'
          : `上传失败 (${xhr.status})`;
      const message = parseUploadError(xhr.responseText, fallback);
      if (xhr.status === 401) handleUnauthorized(path);
      rejectOnce(new ApiError(xhr.status, message));
    };
    xhr.onerror = () => rejectOnce(new ApiError(0, '网络错误，请检查连接后重试'));
    xhr.onabort = () => rejectOnce(new DOMException('上传已取消', 'AbortError'));

    signal?.addEventListener('abort', abortUpload, { once: true });
    xhr.send(file);
  });
}

function parseUploadError(responseText: string, fallback: string): string {
  const text = responseText.trim();
  if (!text) return fallback;

  try {
    const payload: unknown = JSON.parse(text);
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

  if (/<(?:!doctype|html|body|head|title|h\d|p|div)\b/i.test(text)) {
    const document = new DOMParser().parseFromString(text, 'text/html');
    const htmlMessage = document.body.textContent?.replace(/\s+/g, ' ').trim();
    if (htmlMessage) return htmlMessage;
  }

  return text;
}

function buildContentDisposition(fileName: string): string {
  const fallback = fileName
    .replace(/[^\x20-\x7E]/g, '_')
    .replace(/["\\/\r\n]/g, '_') || 'upload';
  const encoded = encodeURIComponent(fileName).replace(
    /['()*]/g,
    (character) => `%${character.charCodeAt(0).toString(16).toUpperCase()}`,
  );
  return `attachment; filename="${fallback}"; filename*=UTF-8''${encoded}`;
}

export async function deleteFile(id: number): Promise<void> {
  await request<void>(`/api/files/${id}`, { method: 'DELETE' });
}

export async function searchFiles(query?: FileSearchQuery): Promise<PaginatedResponse<FileRecord>> {
  const params = new URLSearchParams();
  if (query?.q) params.set('q', query.q);
  if (query?.sort) params.set('sort', query.sort);
  if (query?.offset !== undefined) params.set('offset', String(query.offset));
  if (query?.limit !== undefined) params.set('limit', String(query.limit));
  const qs = params.toString();
  return request<PaginatedResponse<FileRecord>>(`/api/files/search${qs ? `?${qs}` : ''}`);
}
