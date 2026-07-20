import { request } from './client';
import type { AuthUser } from '../types/api';

export async function getUser(id: number): Promise<AuthUser> {
  return request<AuthUser>(`/api/users/${id}`);
}

export async function updateUser(id: number, data: { email?: string; password?: string }): Promise<AuthUser> {
  return request<AuthUser>(`/api/users/${id}`, {
    method: 'PUT',
    body: JSON.stringify(data),
  });
}
