import { request } from './client';
import type { MusicMeta, Playlist, PlaylistItem, PaginatedResponse } from '../types/api';

interface PlaylistListResponse {
  playlists: Playlist[];
}

interface PlaylistItemsResponse {
  playlist_id: number;
  items: PlaylistItem[];
}

export async function getLibrary(offset = 0, limit = 20, search?: string): Promise<PaginatedResponse<MusicMeta>> {
  const params = new URLSearchParams({ offset: String(offset), limit: String(limit) });
  if (search) params.set('search', search);
  return request<PaginatedResponse<MusicMeta>>(`/api/music/library?${params}`);
}

export async function getMusicDetail(id: number): Promise<MusicMeta> {
  return request<MusicMeta>(`/api/music/library/${id}`);
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
