import type { AuthUser, FileRecord, MusicMeta, Playlist, PlaylistItem, PaginatedResponse } from './models';

export type { AuthUser, FileRecord, MusicMeta, Playlist, PlaylistItem, PaginatedResponse };

export interface LoginRequest {
  username: string;
  password: string;
}

export interface RegisterRequest {
  username: string;
  password: string;
  email: string;
}

export interface AuthResponse {
  token: string;
  user_id: number;
  role: 'GUEST' | 'NORMAL' | 'VIP';
}

export interface FileQuery {
  name?: string;
  type?: string;
  offset?: number;
  limit?: number;
}

export interface FileSearchQuery {
  q?: string;
  sort?: string;
  offset?: number;
  limit?: number;
}

export interface UpdateUserRequest {
  email?: string;
  password?: string;
}
