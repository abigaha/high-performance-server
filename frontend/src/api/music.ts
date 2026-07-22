import { ApiError, request } from './client';
import type { MusicMeta, Playlist, PlaylistItem, PaginatedResponse } from '../types/api';

interface PlaylistListResponse {
  playlists: Playlist[];
}

interface PlaylistItemsResponse {
  playlist_id: number;
  items: PlaylistItem[];
}

interface MusicFileResponse {
  file_id: number;
  file_hash: string;
  file_size: number;
  content_type: string;
}

interface MusicDetailResponse {
  music_id: number;
  title: string;
  artist: string;
  album: string;
  genre: string;
  duration_sec: number;
  files?: MusicFileResponse[];
}

export interface PlayableMusic extends MusicMeta {
  file_id: number;
}

export async function getLibrary(
  offset = 0,
  limit = 20,
  search?: string,
  signal?: AbortSignal,
): Promise<PaginatedResponse<MusicMeta>> {
  const params = new URLSearchParams({ offset: String(offset), limit: String(limit) });
  if (search) params.set('search', search);
  return request<PaginatedResponse<MusicMeta>>(`/api/music/library?${params}`, { signal });
}

export async function getMusicDetail(id: number): Promise<PlayableMusic> {
  const detail = await request<MusicDetailResponse>(`/api/music/library/${id}`);
  const file = detail.files?.find((candidate) => candidate.content_type.startsWith('audio/'))
    ?? detail.files?.[0];
  if (!file) {
    throw new ApiError(422, '该音乐没有可播放的音频文件');
  }

  return {
    music_id: detail.music_id,
    title: detail.title,
    artist: detail.artist,
    album: detail.album,
    genre: detail.genre,
    duration_sec: detail.duration_sec,
    file_id: file.file_id,
    file_hash: file.file_hash,
    file_size: file.file_size,
    content_type: file.content_type,
  };
}

export async function getUserPlaylists(userId: number): Promise<Playlist[]> {
  const response = await request<PlaylistListResponse>(`/api/users/${userId}/playlists`);
  return response.playlists;
}

export async function createPlaylist(userId: number, name: string, description?: string): Promise<Playlist> {
  return request<Playlist>(`/api/users/${userId}/playlists`, {
    method: 'POST',
    body: JSON.stringify({ name, description }),
  });
}

export async function getPlaylistItems(playlistId: number): Promise<PlaylistItem[]> {
  const response = await request<PlaylistItemsResponse>(`/api/playlists/${playlistId}/items`);
  return response.items;
}

export async function addToPlaylist(playlistId: number, musicId: number): Promise<void> {
  await request<void>(`/api/playlists/${playlistId}/items`, {
    method: 'POST',
    body: JSON.stringify({ music_id: musicId }),
  });
}

export async function removeFromPlaylist(playlistId: number, musicId: number): Promise<void> {
  await request<void>(`/api/playlists/${playlistId}/items/${musicId}`, {
    method: 'DELETE',
  });
}

export async function reorderPlaylist(playlistId: number, ids: number[]): Promise<void> {
  await request<void>(`/api/playlists/${playlistId}/items/reorder`, {
    method: 'PUT',
    body: JSON.stringify({ music_ids: ids }),
  });
}
