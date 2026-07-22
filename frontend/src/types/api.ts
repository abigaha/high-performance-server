import type {
  AuthUser,
  FileRecord,
  MusicMeta,
  Playlist,
  PlaylistItem,
  PaginatedResponse,
  UserRole,
  UserRoleValue,
} from './models';

export type {
  AuthUser,
  FileRecord,
  MusicMeta,
  Playlist,
  PlaylistItem,
  PaginatedResponse,
  UserRole,
  UserRoleValue,
};

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
  role: UserRole;
}

export interface UploadResult {
  file_id: number;
  file_name: string;
  file_hash: string;
  size: number;
  chunks?: number;
  exists?: boolean;
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
