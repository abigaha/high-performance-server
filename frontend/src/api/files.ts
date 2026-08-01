import {
  API_BASE,
  ApiError,
  captureSessionSnapshot,
  handleUnauthorized,
  isSessionSnapshotCurrent,
  parseApiErrorDetails,
  request,
  type SessionSnapshot,
} from './client';
import { getContentType } from '../lib/uploadPolicy';
import type {
  FileRecord,
  PaginatedResponse,
  FileQuery,
  FileSearchQuery,
  UploadResult,
} from '../types/api';

export async function getFiles(query?: FileQuery, signal?: AbortSignal): Promise<PaginatedResponse<FileRecord>> {
  const params = new URLSearchParams();
  if (query?.name) params.set('name', query.name);
  if (query?.type) params.set('type', query.type);
  if (query?.offset !== undefined) params.set('offset', String(query.offset));
  if (query?.limit !== undefined) params.set('limit', String(query.limit));
  const qs = params.toString();
  return request<PaginatedResponse<FileRecord>>(`/api/files${qs ? `?${qs}` : ''}`, { signal });
}

export async function getFile(id: number): Promise<FileRecord> {
  return request<FileRecord>(`/api/files/${id}`);
}

function assertDownloadActive(signal: AbortSignal | undefined, session: SessionSnapshot): void {
  if (signal?.aborted || !isSessionSnapshotCurrent(session)) {
    throw new DOMException('下载已取消', 'AbortError');
  }
}

export async function getFileDownloadUrl(id: number, signal?: AbortSignal): Promise<string> {
  const session = captureSessionSnapshot();
  assertDownloadActive(signal, session);
  const response = await request<Response>(`/api/files/${id}/download`, { signal }, true);
  assertDownloadActive(signal, session);
  const blob = await response.blob();
  assertDownloadActive(signal, session);
  const url = URL.createObjectURL(blob);
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

  const requestSession = captureSessionSnapshot();
  const token = requestSession.token;
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
      const detail = parseApiErrorDetails(xhr.responseText, fallback);
      if (xhr.status === 401) handleUnauthorized(path, requestSession);
      rejectOnce(new ApiError(xhr.status, detail.message, detail.code));
    };
    xhr.onerror = () => rejectOnce(new ApiError(0, '网络错误，请检查连接后重试'));
    xhr.onabort = () => rejectOnce(new DOMException('上传已取消', 'AbortError'));

    signal?.addEventListener('abort', abortUpload, { once: true });
    xhr.send(file);
  });
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
