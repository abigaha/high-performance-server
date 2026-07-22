import { request } from './client';
import { normalizeUserRole } from '../types/models';
import type { AuthResponse, AuthUser, UserRoleValue } from '../types/api';

interface AuthResponsePayload extends Omit<AuthResponse, 'role'> {
  role: UserRoleValue;
}

interface AuthUserPayload extends Omit<AuthUser, 'role'> {
  role: UserRoleValue;
}

function normalizeAuthResponse(payload: AuthResponsePayload): AuthResponse {
  return { ...payload, role: normalizeUserRole(payload.role) };
}

export async function login(username: string, password: string): Promise<AuthResponse> {
  const response = await request<AuthResponsePayload>('/api/auth/login', {
    method: 'POST',
    body: JSON.stringify({ username, password }),
  });
  return normalizeAuthResponse(response);
}

export async function register(username: string, password: string, email: string): Promise<AuthResponse> {
  const response = await request<AuthResponsePayload>('/api/auth/register', {
    method: 'POST',
    body: JSON.stringify({ username, password, email }),
  });
  return normalizeAuthResponse(response);
}

export async function logout(): Promise<void> {
  await request<void>('/api/auth/logout', { method: 'POST' });
}

export async function getMe(): Promise<AuthUser> {
  const response = await request<AuthUserPayload>('/api/auth/me');
  return { ...response, role: normalizeUserRole(response.role) };
}
