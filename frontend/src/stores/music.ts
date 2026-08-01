import { create } from 'zustand';
import type { MusicMeta, Playlist } from '../types/api';
import * as musicApi from '../api/music';
import { usePlayerStore } from './player';
import { captureSessionSnapshot, isSessionSnapshotCurrent } from '../api/client';

interface StoredPlaylist {
  id: number;
  name: string;
  description?: string;
  itemCount: number;
  user_id?: number;
  item_count?: number;
  created_at?: string;
}

interface MusicState {
  library: MusicMeta[];
  libraryTotal: number;
  libraryLoading: boolean;
  libraryError: string | null;
  currentPlaylist: { id: number; name: string; items: MusicMeta[] } | null;
  userPlaylists: StoredPlaylist[];
  fetchLibrary: (offset: number, limit: number, search?: string) => Promise<void>;
  fetchPlaylists: (userId: number) => Promise<void>;
  createPlaylist: (userId: number, name: string, description?: string) => Promise<void>;
  renamePlaylist: (playlistId: number, name: string, description: string) => Promise<void>;
  deletePlaylist: (playlistId: number) => Promise<void>;
  addToPlaylist: (playlistId: number, musicId: number) => Promise<void>;
  removeFromPlaylist: (playlistId: number, musicId: number) => Promise<void>;
  reorderPlaylist: (playlistId: number, ids: number[]) => Promise<void>;
  reset: () => void;
}

let libraryRequestId = 0;
let playlistsRequestId = 0;
let musicGeneration = 0;
const mutationIds = new Map<string, number>();

function startMutation(key: string): number {
  const id = (mutationIds.get(key) ?? 0) + 1;
  mutationIds.set(key, id);
  return id;
}

function isMutationCurrent(key: string, id: number): boolean {
  return mutationIds.get(key) === id;
}

const initialMusicState = {
  library: [] as MusicMeta[],
  libraryTotal: 0,
  libraryLoading: false,
  libraryError: null,
  currentPlaylist: null,
  userPlaylists: [] as StoredPlaylist[],
};

function errorMessage(error: unknown, fallback: string): string {
  return error instanceof Error && error.message ? error.message : fallback;
}

function toStoredPlaylist(playlist: Playlist): StoredPlaylist {
  return {
    ...playlist,
    description: playlist.description,
    itemCount: playlist.item_count,
  };
}

function invalidatePendingPlaylistPlayback(): void {
  usePlayerStore.setState((state) => ({ stateRevision: state.stateRevision + 1 }));
}

export const useMusicStore = create<MusicState>((set) => ({
  ...initialMusicState,
  fetchLibrary: async (offset, limit, search) => {
    const generation = musicGeneration;
    const requestId = ++libraryRequestId;
    set({ libraryLoading: true, libraryError: null });
    try {
      const res = await musicApi.getLibrary(offset, limit, search);
      if (generation === musicGeneration && requestId === libraryRequestId) {
        set({ library: res.items, libraryTotal: res.total, libraryLoading: false });
      }
    } catch (error) {
      if (generation === musicGeneration && requestId === libraryRequestId) {
        set({
          libraryLoading: false,
          libraryError: errorMessage(error, '音乐库加载失败，请稍后重试'),
        });
      }
    }
  },
  fetchPlaylists: async (userId) => {
    const generation = musicGeneration;
    const requestId = ++playlistsRequestId;
    const session = captureSessionSnapshot();
    const list = await musicApi.getUserPlaylists(userId);
    if (generation === musicGeneration && requestId === playlistsRequestId && isSessionSnapshotCurrent(session)) {
      set({ userPlaylists: list.map((playlist) => toStoredPlaylist(playlist)) });
    }
  },
  createPlaylist: async (userId, name, description) => {
    const generation = musicGeneration;
    const key = `playlist-create:${userId}`;
    const operationId = startMutation(key);
    const session = captureSessionSnapshot();
    const playlist = await musicApi.createPlaylist(userId, name, description);
    if (generation !== musicGeneration || !isMutationCurrent(key, operationId) || !isSessionSnapshotCurrent(session)) return;
    set((state) => ({
      userPlaylists: [
        ...state.userPlaylists.filter((item) => item.id !== playlist.id),
        toStoredPlaylist(playlist),
      ],
    }));
  },
  renamePlaylist: async (playlistId, name, description) => {
    const generation = musicGeneration;
    const key = `playlist:${playlistId}`;
    const operationId = startMutation(key);
    const session = captureSessionSnapshot();
    const playlist = await musicApi.renamePlaylist(playlistId, name, description);
    if (generation !== musicGeneration || !isMutationCurrent(key, operationId) || !isSessionSnapshotCurrent(session)) return;
    const updatedPlaylist = toStoredPlaylist(playlist);
    set((state) => ({
      userPlaylists: state.userPlaylists.map((item) => item.id === playlistId
        ? {
            ...item,
            ...updatedPlaylist,
          }
        : item),
      currentPlaylist: state.currentPlaylist?.id === playlistId
        ? { ...state.currentPlaylist, name: playlist.name }
        : state.currentPlaylist,
    }));
  },
  deletePlaylist: async (playlistId) => {
    const generation = musicGeneration;
    const key = `playlist:${playlistId}`;
    const operationId = startMutation(key);
    const session = captureSessionSnapshot();
    await musicApi.deletePlaylist(playlistId);
    if (generation !== musicGeneration || !isMutationCurrent(key, operationId) || !isSessionSnapshotCurrent(session)) return;
    set((state) => ({
      userPlaylists: state.userPlaylists.filter((item) => item.id !== playlistId),
      currentPlaylist: state.currentPlaylist?.id === playlistId ? null : state.currentPlaylist,
    }));
    usePlayerStore.getState().detachSource({ kind: 'PLAYLIST', id: playlistId });
    invalidatePendingPlaylistPlayback();
  },
  addToPlaylist: async (playlistId, musicId) => {
    const generation = musicGeneration;
    const key = `playlist-item:${playlistId}:${musicId}`;
    const operationId = startMutation(key);
    const session = captureSessionSnapshot();
    await musicApi.addToPlaylist(playlistId, musicId);
    if (generation !== musicGeneration || !isMutationCurrent(key, operationId) || !isSessionSnapshotCurrent(session)) return;
    set((state) => ({
      userPlaylists: state.userPlaylists.map((item) => item.id === playlistId
        ? { ...item, itemCount: item.itemCount + 1 }
        : item),
    }));
  },
  removeFromPlaylist: async (playlistId, musicId) => {
    const generation = musicGeneration;
    const key = `playlist-item:${playlistId}:${musicId}`;
    const operationId = startMutation(key);
    const session = captureSessionSnapshot();
    await musicApi.removeFromPlaylist(playlistId, musicId);
    if (generation !== musicGeneration || !isMutationCurrent(key, operationId) || !isSessionSnapshotCurrent(session)) return;
    set((state) => ({
      userPlaylists: state.userPlaylists.map((item) => item.id === playlistId
        ? { ...item, itemCount: Math.max(0, item.itemCount - 1) }
        : item),
      currentPlaylist: state.currentPlaylist?.id === playlistId
        ? {
            ...state.currentPlaylist,
            items: state.currentPlaylist.items.filter((item) => item.music_id !== musicId),
          }
        : state.currentPlaylist,
    }));
    usePlayerStore.getState().removePendingTrack(musicId, { kind: 'PLAYLIST', id: playlistId });
    invalidatePendingPlaylistPlayback();
  },
  reorderPlaylist: async (playlistId, ids) => {
    const generation = musicGeneration;
    const key = `playlist:${playlistId}`;
    const operationId = startMutation(key);
    const session = captureSessionSnapshot();
    await musicApi.reorderPlaylist(playlistId, ids);
    if (generation !== musicGeneration || !isMutationCurrent(key, operationId) || !isSessionSnapshotCurrent(session)) return;
    set((state) => {
      if (state.currentPlaylist?.id !== playlistId || state.currentPlaylist.items.length !== ids.length) return {};
      const byId = new Map(state.currentPlaylist.items.map((item) => [item.music_id, item]));
      const items = ids.map((id) => byId.get(id));
      if (items.some((item) => item === undefined)) return {};
      return {
        currentPlaylist: {
          ...state.currentPlaylist,
          items: items as MusicMeta[],
        },
      };
    });
    invalidatePendingPlaylistPlayback();
  },
  reset: () => {
    musicGeneration += 1;
    libraryRequestId += 1;
    playlistsRequestId += 1;
    mutationIds.clear();
    set({ ...initialMusicState, library: [], userPlaylists: [] });
  },
}));
