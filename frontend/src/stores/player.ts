import { create } from 'zustand';
import type { MusicMeta } from '../types/api';

interface PlayerState {
  currentTrack: MusicMeta | null;
  playlist: MusicMeta[];
  playlistIndex: number;
  playing: boolean;
  currentTime: number;
  duration: number;
  volume: number;
  play: (track: MusicMeta, list?: MusicMeta[]) => void;
  pause: () => void;
  resume: () => void;
  seek: (time: number) => void;
  next: () => void;
  prev: () => void;
  setVolume: (vol: number) => void;
  setCurrentTime: (t: number) => void;
  setDuration: (d: number) => void;
  hydrateTrack: (track: MusicMeta) => void;
}

export const usePlayerStore = create<PlayerState>((set, get) => ({
  currentTrack: null,
  playlist: [],
  playlistIndex: -1,
  playing: false,
  currentTime: 0,
  duration: 0,
  volume: 0.8,
  play: (track, list) => {
    const idx = list ? list.findIndex((t) => t.music_id === track.music_id) : -1;
    set({
      currentTrack: track,
      playlist: list || [track],
      playlistIndex: idx >= 0 ? idx : 0,
      playing: true,
      currentTime: 0,
      duration: track.duration_sec || 0,
    });
  },
  pause: () => set({ playing: false }),
  resume: () => set({ playing: true }),
  seek: (time) => set({ currentTime: time }),
  next: () => {
    const { playlist, playlistIndex } = get();
    if (playlistIndex < playlist.length - 1) {
      const nextIdx = playlistIndex + 1;
      const track = playlist[nextIdx];
      set({
        currentTrack: track,
        playlistIndex: nextIdx,
        currentTime: 0,
        duration: track.duration_sec || 0,
        playing: true,
      });
    }
  },
  prev: () => {
    const { playlist, playlistIndex } = get();
    if (playlistIndex > 0) {
      const prevIdx = playlistIndex - 1;
      const track = playlist[prevIdx];
      set({
        currentTrack: track,
        playlistIndex: prevIdx,
        currentTime: 0,
        duration: track.duration_sec || 0,
        playing: true,
      });
    }
  },
  setVolume: (vol) => set({ volume: Math.max(0, Math.min(1, vol)) }),
  setCurrentTime: (t) => set({ currentTime: Number.isFinite(t) ? Math.max(0, t) : 0 }),
  setDuration: (d) => set({ duration: Number.isFinite(d) ? Math.max(0, d) : 0 }),
  hydrateTrack: (track) => set((state) => {
    const isCurrentTrack = state.currentTrack?.music_id === track.music_id;
    const existingIndex = state.playlist.findIndex((item) => item.music_id === track.music_id);
    const playlist = existingIndex >= 0
      ? state.playlist.map((item, index) => (index === existingIndex ? track : item))
      : [track];

    return {
      currentTrack: track,
      playlist,
      playlistIndex: existingIndex >= 0 ? existingIndex : 0,
      playing: isCurrentTrack ? state.playing : false,
      currentTime: isCurrentTrack ? state.currentTime : 0,
      duration: track.duration_sec || (isCurrentTrack ? state.duration : 0),
    };
  }),
}));
