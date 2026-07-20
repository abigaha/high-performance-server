import { request } from './client';
import type { FileRecord, PaginatedResponse, FileQuery, FileSearchQuery } from '../types/api';

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
  const API_BASE = import.meta.env.VITE_API_URL || 'http://127.0.0.1:9090';
  return `${API_BASE}/api/files/${id}/download`;
}

export async function getFileStreamUrl(id: number): Promise<string> {
  const API_BASE = import.meta.env.VITE_API_URL || 'http://127.0.0.1:9090';
  return `${API_BASE}/api/files/${id}/stream`;
}

export async function uploadFile(
  file: File,
  onProgress?: (pct: number) => void,
): Promise<FileRecord> {
  const API_BASE = import.meta.env.VITE_API_URL || 'http://127.0.0.1:9090';
  const token = localStorage.getItem('token');
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open('POST', `${API_BASE}/api/files/upload`);
    if (token) xhr.setRequestHeader('Authorization', `Bearer ${token}`);
    xhr.upload.onprogress = (e) => {
      if (e.lengthComputable && onProgress) {
        onProgress(Math.round((e.loaded / e.total) * 100));
      }
    };
    xhr.onload = () => {
      if (xhr.status >= 200 && xhr.status < 300) {
        resolve(JSON.parse(xhr.responseText));
      } else {
        reject(new Error(`上传失败 (${xhr.status})`));
      }
    };
    xhr.onerror = () => reject(new Error('网络错误'));
    const fd = new FormData();
    fd.append('file', file);
    xhr.send(fd);
  });
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
