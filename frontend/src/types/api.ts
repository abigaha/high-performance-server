import type {
  AuthUser,
  FileRecord,
  MusicMeta,
  Playlist,
  PlaylistItem,
  PaginatedResponse,
  UserRole,
  UserRoleValue,
  VipStatus,
  Capability,
  VipPlan,
  VipMembership,
  AdminUserSummary,
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
  VipStatus,
  Capability,
  VipPlan,
  VipMembership,
  AdminUserSummary,
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
  user: AuthUser;
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
