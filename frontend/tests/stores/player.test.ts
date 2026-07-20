import { describe, it, expect, beforeEach } from 'vitest';
import { usePlayerStore } from '../../src/stores/player';
import type { MusicMeta } from '../../src/types/api';

const mockTrack: MusicMeta = {
  music_id: 1,
  title: 'Test Song',
  artist: 'Test Artist',
  album: 'Test Album',
  genre: 'Pop',
  duration_sec: 200,
  file_hash: 'abc123',
  file_size: 1000,
  content_type: 'audio/mpeg',
};

beforeEach(() => {
  usePlayerStore.setState({
    currentTrack: null,
    playlist: [],
    playlistIndex: -1,
    playing: false,
    currentTime: 0,
    duration: 0,
    volume: 0.8,
  });
});

describe('player store', () => {
  it('play sets current track and starts playing', () => {
    const store = usePlayerStore.getState();
    store.play(mockTrack);

    const state = usePlayerStore.getState();
    expect(state.currentTrack?.music_id).toBe(1);
    expect(state.playing).toBe(true);
    expect(state.playlist).toHaveLength(1);
  });

  it('pause and resume toggle playing state', () => {
    const store = usePlayerStore.getState();
    store.play(mockTrack);
    usePlayerStore.getState().pause();
    expect(usePlayerStore.getState().playing).toBe(false);
    usePlayerStore.getState().resume();
    expect(usePlayerStore.getState().playing).toBe(true);
  });

  it('next goes to the next track in playlist', () => {
    const track2 = { ...mockTrack, music_id: 2, title: 'Song 2' };
    const store = usePlayerStore.getState();
    store.play(mockTrack, [mockTrack, track2]);

    usePlayerStore.getState().next();
    const state = usePlayerStore.getState();
    expect(state.currentTrack?.music_id).toBe(2);
    expect(state.playlistIndex).toBe(1);
  });

  it('setVolume clamps between 0 and 1', () => {
    usePlayerStore.getState().setVolume(1.5);
    expect(usePlayerStore.getState().volume).toBe(1);
    usePlayerStore.getState().setVolume(-1);
    expect(usePlayerStore.getState().volume).toBe(0);
  });
});
