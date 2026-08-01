export type UserRole = 'GUEST' | 'NORMAL' | 'VIP' | 'ADMIN';

export type UserRoleValue = UserRole | 0 | 1 | 2 | 3 | '0' | '1' | '2' | '3';

export type VipStatus = 'NONE' | 'ACTIVE' | 'EXPIRED';

export type Capability =
  | 'USE_AUTHENTICATED_FEATURES'
  | 'USE_VIP_BENEFITS'
  | 'MANAGE_USERS'
  | 'DELETE_ANY_FILE';

export function normalizeUserRole(role: unknown): UserRole {
  if (role === 'ADMIN' || role === 3 || role === '3') return 'ADMIN';
  if (role === 'VIP' || role === 2 || role === '2') return 'VIP';
  if (role === 'NORMAL' || role === 1 || role === '1') return 'NORMAL';
  return 'GUEST';
}

export interface AuthUser {
  user_id: number;
  username: string;
  email: string;
  role: UserRole;
  vip_status: VipStatus;
  vip_expires_at: string | null;
  capabilities: Capability[];
  created_at: string;
}

export interface VipPlan {
  duration_days: 30 | 90 | 365;
  label: string;
}

export interface VipMembership {
  role: 'NORMAL' | 'VIP';
  vip_status: VipStatus;
  vip_expires_at: string | null;
  server_now: string;
  remaining_seconds: number;
}

export interface AdminUserSummary {
  user_id: number;
  username: string;
  email: string;
  role: UserRole;
  vip_status: VipStatus;
  vip_expires_at: string | null;
  created_at: string;
}

export interface FileRecord {
  file_id: number;
  file_name: string;
  file_hash: string;
  file_size: number;
  content_type: string;
  uploaded_by: number;
  can_delete: boolean;
  created_at: string;
}

export interface MusicMeta {
  music_id: number;
  title: string;
  artist: string;
  album: string;
  genre: string;
  duration_sec: number;
  file_hash: string;
  file_size: number;
  content_type: string;
}

export interface Playlist {
  id: number;
  user_id: number;
  name: string;
  description: string;
  item_count: number;
  created_at: string;
}

export interface PlaylistItem {
  id: number;
  playlist_id: number;
  music_id: number;
  title: string;
  artist: string;
  file_hash: string;
  sort_order: number;
  added_at: string;
}

export interface PaginatedResponse<T> {
  items: T[];
  total: number;
  offset: number;
  limit: number;
}

export interface ApiError {
  error: string;
}
