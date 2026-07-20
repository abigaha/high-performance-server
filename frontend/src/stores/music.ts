import { create } from 'zustand';
import type { MusicMeta, Playlist } from '../types/api';
import * as musicApi from '../api/music';

interface MusicState {
  library: MusicMeta[];
  libraryTotal: number;
  currentPlaylist: { id: number; name: string; items: MusicMeta[] } | null;
  userPlaylists: { id: number; name: string; itemCount: number }[];
  fetchLibrary: (offset: number, limit: number, search?: string) => Promise<void>;
  fetchPlaylists: (userId: number) => Promise<void>;
  addToPlaylist: (playlistId: number, musicId: number) => Promise<void>;
  removeFromPlaylist: (playlistId: number, musicId: number) => Promise<void>;
  reorderPlaylist: (playlistId: number, ids: number[]) => Promise<void>;
}

export const useMusicStore = create<MusicState>((set) => ({
  library: [],
  libraryTotal: 0,
  currentPlaylist: null,
  userPlaylists: [],
  fetchLibrary: async (offset, limit, search) => {
    const res = await musicApi.getLibrary(offset, limit, search);
    set({ library: res.items, libraryTotal: res.total });
  },
  fetchPlaylists: async (userId) => {
    const list = await musicApi.getUserPlaylists(userId);
    set({ userPlaylists: list.map((p: Playlist) => ({ id: p.id, name: p.name, itemCount: p.item_count })) });
  },
  addToPlaylist: async (playlistId, musicId) => {
    await musicApi.addToPlaylist(playlistId, musicId);
  },
  removeFromPlaylist: async (playlistId, musicId) => {
    await musicApi.removeFromPlaylist(playlistId, musicId);
  },
  reorderPlaylist: async (playlistId, ids) => {
    await musicApi.reorderPlaylist(playlistId, ids);
  },
}));
