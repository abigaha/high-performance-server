import { create } from 'zustand';
import type { MusicMeta } from '../types/api';

export type QueueSource =
  | { kind: 'SINGLE'; id: null }
  | { kind: 'LIBRARY'; id: null }
  | { kind: 'PLAYLIST'; id: number };

export interface QueueEntry {
  track: MusicMeta;
  source: QueueSource;
}

interface PlayerState {
  currentTrack: MusicMeta | null;
  queue: QueueEntry[];
  queueIndex: number;
  playing: boolean;
  currentTime: number;
  duration: number;
  volume: number;
  stateRevision: number;
  play: (track: MusicMeta, queue: QueueEntry[]) => void;
  pause: () => void;
  resume: () => void;
  seek: (time: number) => void;
  next: () => void;
  prev: () => void;
  setVolume: (vol: number) => void;
  setCurrentTime: (t: number) => void;
  setDuration: (d: number) => void;
  hydrateTrack: (track: MusicMeta) => void;
  detachSource: (source: QueueSource) => void;
  removePendingTrack: (musicId: number, source: QueueSource) => void;
  reset: () => void;
}

const initialPlayerState = {
  currentTrack: null,
  queue: [] as QueueEntry[],
  queueIndex: -1,
  playing: false,
  currentTime: 0,
  duration: 0,
  volume: 0.8,
  stateRevision: 0,
};

let playerGeneration = 0;

export function capturePlayerGeneration(): number {
  return playerGeneration;
}

export function isPlayerGenerationCurrent(generation: number): boolean {
  return generation === playerGeneration;
}

export function capturePlayerStateRevision(): number {
  return usePlayerStore.getState().stateRevision;
}

const singleSource = (): QueueSource => ({ kind: 'SINGLE', id: null });

function sameSource(left: QueueSource, right: QueueSource): boolean {
  return left.kind === right.kind && left.id === right.id;
}

function copyEntry(entry: QueueEntry): QueueEntry {
  return {
    track: { ...entry.track },
    source: { ...entry.source },
  };
}

export const usePlayerStore = create<PlayerState>((set, get) => ({
  ...initialPlayerState,
  play: (track, queue) => {
    const selectedIndex = queue.findIndex((entry) => entry.track === track);
    const fallbackIndex = queue.findIndex((entry) => entry.track.music_id === track.music_id);
    const queueIndex = selectedIndex >= 0 ? selectedIndex : fallbackIndex;
    const snapshot = (queue.length > 0 ? queue : [{ track, source: singleSource() }]).map(copyEntry);
    const normalizedIndex = queueIndex >= 0 ? queueIndex : 0;
    const currentTrack = snapshot[normalizedIndex].track;
    set((state) => ({
      currentTrack,
      queue: snapshot,
      queueIndex: normalizedIndex,
      playing: true,
      currentTime: 0,
      duration: currentTrack.duration_sec || 0,
      stateRevision: state.stateRevision + 1,
    }));
  },
  pause: () => set({ playing: false }),
  resume: () => set({ playing: true }),
  seek: (time) => set({ currentTime: time }),
  next: () => {
    const { queue, queueIndex } = get();
    if (queueIndex < queue.length - 1) {
      const nextIndex = queueIndex + 1;
      const nextTrack = queue[nextIndex].track;
      set((state) => ({
        currentTrack: nextTrack,
        queueIndex: nextIndex,
        currentTime: 0,
        duration: nextTrack.duration_sec || 0,
        playing: true,
        stateRevision: state.stateRevision + 1,
      }));
    }
  },
  prev: () => {
    const { queue, queueIndex } = get();
    if (queueIndex > 0) {
      const previousIndex = queueIndex - 1;
      const previousTrack = queue[previousIndex].track;
      set((state) => ({
        currentTrack: previousTrack,
        queueIndex: previousIndex,
        currentTime: 0,
        duration: previousTrack.duration_sec || 0,
        playing: true,
        stateRevision: state.stateRevision + 1,
      }));
    }
  },
  setVolume: (vol) => set({ volume: Math.max(0, Math.min(1, vol)) }),
  setCurrentTime: (time) => set({ currentTime: Number.isFinite(time) ? Math.max(0, time) : 0 }),
  setDuration: (duration) => set({ duration: Number.isFinite(duration) ? Math.max(0, duration) : 0 }),
  hydrateTrack: (track) => set((state) => {
    const isCurrentTrack = state.currentTrack?.music_id === track.music_id;
    if (!isCurrentTrack) {
      const hydratedTrack = { ...track };
      return {
        currentTrack: hydratedTrack,
        queue: [{ track: hydratedTrack, source: singleSource() }],
        queueIndex: 0,
        playing: false,
        currentTime: 0,
        duration: track.duration_sec || 0,
        stateRevision: state.stateRevision + 1,
      };
    }

    const hydratedTrack = { ...track };
    return {
      currentTrack: hydratedTrack,
      queue: state.queue.map((entry, index) => index === state.queueIndex
        ? { ...entry, track: hydratedTrack }
        : entry),
      duration: track.duration_sec || state.duration,
      stateRevision: state.stateRevision + 1,
    };
  }),
  detachSource: (source) => set((state) => {
    if (!state.queue.some((entry) => sameSource(entry.source, source))) return {};
    return {
      queue: state.queue.map((entry) => sameSource(entry.source, source)
      ? { ...entry, source: singleSource() }
      : entry),
      stateRevision: state.stateRevision + 1,
    };
  }),
  removePendingTrack: (musicId, source) => set((state) => {
    let changed = false;
    const queue = state.queue
      .map((entry, index) => {
        const isCurrentMatch = index === state.queueIndex
          && entry.track.music_id === musicId
          && sameSource(entry.source, source);
        if (!isCurrentMatch) return entry;
        changed = true;
        return { ...entry, source: singleSource() };
      })
      .filter((entry, index) => {
        const keep = index <= state.queueIndex
          || entry.track.music_id !== musicId
          || !sameSource(entry.source, source);
        if (!keep) changed = true;
        return keep;
      });
    return changed ? { queue, stateRevision: state.stateRevision + 1 } : {};
  }),
  reset: () => {
    playerGeneration += 1;
    set((state) => ({
      ...initialPlayerState,
      queue: [],
      stateRevision: state.stateRevision + 1,
    }));
  },
}));
