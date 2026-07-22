import { create } from 'zustand';
import type { MusicMeta, Playlist } from '../types/api';
import * as musicApi from '../api/music';

interface MusicState {
  library: MusicMeta[];
  libraryTotal: number;
  libraryLoading: boolean;
  libraryError: string | null;
  currentPlaylist: { id: number; name: string; items: MusicMeta[] } | null;
  userPlaylists: { id: number; name: string; itemCount: number }[];
  fetchLibrary: (offset: number, limit: number, search?: string) => Promise<void>;
  fetchPlaylists: (userId: number) => Promise<void>;
  addToPlaylist: (playlistId: number, musicId: number) => Promise<void>;
  removeFromPlaylist: (playlistId: number, musicId: number) => Promise<void>;
  reorderPlaylist: (playlistId: number, ids: number[]) => Promise<void>;
}

let libraryRequestId = 0;

function errorMessage(error: unknown, fallback: string): string {
  return error instanceof Error && error.message ? error.message : fallback;
}

export const useMusicStore = create<MusicState>((set) => ({
  library: [],
  libraryTotal: 0,
  libraryLoading: false,
  libraryError: null,
  currentPlaylist: null,
  userPlaylists: [],
  fetchLibrary: async (offset, limit, search) => {
    const requestId = ++libraryRequestId;
    set({ libraryLoading: true, libraryError: null });
    try {
      const res = await musicApi.getLibrary(offset, limit, search);
      if (requestId === libraryRequestId) {
        set({ library: res.items, libraryTotal: res.total, libraryLoading: false });
      }
    } catch (error) {
      if (requestId === libraryRequestId) {
        set({
          libraryLoading: false,
          libraryError: errorMessage(error, '音乐库加载失败，请稍后重试'),
        });
      }
    }
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
