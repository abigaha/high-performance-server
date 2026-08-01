import { request } from './client';
import type { AdminUserSummary, PaginatedResponse } from '../types/api';

export interface AdminUsersQuery {
  q?: string;
  offset?: number;
  limit?: number;
}

export function getAdminUsers(
  query: AdminUsersQuery = {},
  signal?: AbortSignal,
): Promise<PaginatedResponse<AdminUserSummary>> {
  const params = new URLSearchParams();
  if (query.q !== undefined) params.set('q', query.q);
  if (query.offset !== undefined) params.set('offset', String(query.offset));
  if (query.limit !== undefined) params.set('limit', String(query.limit));
  const suffix = params.size > 0 ? `?${params.toString()}` : '';
  return request<PaginatedResponse<AdminUserSummary>>(`/api/admin/users${suffix}`, { signal });
}

export function grantUserVip(userId: number, durationDays: 30 | 90 | 365): Promise<AdminUserSummary> {
  return request<AdminUserSummary>(`/api/admin/users/${userId}/vip`, {
    method: 'POST',
    body: JSON.stringify({ duration_days: durationDays }),
  });
}

export function revokeUserVip(userId: number): Promise<AdminUserSummary> {
  return request<AdminUserSummary>(`/api/admin/users/${userId}/vip`, { method: 'DELETE' });
}
