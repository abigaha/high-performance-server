import { beforeEach, describe, expect, it } from 'vitest';
import {
  usePlayerStore,
  type QueueEntry,
  type QueueSource,
} from '../../src/stores/player';
import type { MusicMeta } from '../../src/types/api';

function track(musicId: number, title = `Song ${musicId}`): MusicMeta {
  return {
    music_id: musicId,
    title,
    artist: 'Artist',
    album: 'Album',
    genre: 'Pop',
    duration_sec: 200,
    file_hash: `hash-${musicId}`,
    file_size: 1000,
    content_type: 'audio/mpeg',
  };
}

const single = (): QueueSource => ({ kind: 'SINGLE', id: null });
const library = (): QueueSource => ({ kind: 'LIBRARY', id: null });
const playlist = (id: number): QueueSource => ({ kind: 'PLAYLIST', id });
const entry = (musicId: number, source: QueueSource, title?: string): QueueEntry => ({
  track: track(musicId, title),
  source,
});

beforeEach(() => {
  usePlayerStore.getState().reset();
});

describe('player store', () => {
  it.each([
    single(),
    library(),
    playlist(7),
  ])('逐项保留 $kind 来源并复制播放快照', (source) => {
    const selected = track(1);
    const input = [entry(1, source), entry(2, source)];

    usePlayerStore.getState().play(selected, input);
    input[0].track.title = 'mutated';
    input[0].source = single();
    input.push(entry(3, source));

    const state = usePlayerStore.getState();
    expect(state.currentTrack).toEqual(selected);
    expect(state.playing).toBe(true);
    expect(state.queue).toHaveLength(2);
    expect(state.queue[0]).toEqual({ track: expect.objectContaining({ title: 'Song 1' }), source });
  });

  it('next and prev follow QueueEntry tracks', () => {
    const first = track(1);
    usePlayerStore.getState().play(first, [entry(1, library()), entry(2, library())]);

    usePlayerStore.getState().next();
    expect(usePlayerStore.getState()).toMatchObject({
      currentTrack: expect.objectContaining({ music_id: 2 }),
      queueIndex: 1,
    });

    usePlayerStore.getState().prev();
    expect(usePlayerStore.getState()).toMatchObject({
      currentTrack: expect.objectContaining({ music_id: 1 }),
      queueIndex: 0,
    });
  });

  it('删除当前歌单时全部同源项转 SINGLE 且当前、顺序和播放进度不中断', () => {
    const selected = track(2);
    usePlayerStore.getState().play(selected, [
      entry(1, playlist(7)),
      { track: selected, source: playlist(7) },
      entry(3, library()),
      entry(4, playlist(7)),
    ]);
    usePlayerStore.getState().setCurrentTime(35);
    const currentTrack = usePlayerStore.getState().currentTrack;

    usePlayerStore.getState().detachSource(playlist(7));

    const state = usePlayerStore.getState();
    expect(state.queue.map(({ track: item, source }) => [item.music_id, source])).toEqual([
      [1, single()],
      [2, single()],
      [3, library()],
      [4, single()],
    ]);
    expect(state.currentTrack).toBe(currentTrack);
    expect(state.queueIndex).toBe(1);
    expect(state.currentTime).toBe(35);
    expect(state.playing).toBe(true);
  });

  it('移除曲目仅删除当前之后同来源同 music，当前转 SINGLE，已播与其他来源重复项保留', () => {
    const selected = track(1, 'current');
    usePlayerStore.getState().play(selected, [
      entry(1, playlist(7), 'played'),
      { track: selected, source: playlist(7) },
      entry(1, playlist(7), 'pending matching'),
      entry(1, library(), 'pending library duplicate'),
      entry(2, playlist(7), 'pending other music'),
      entry(1, playlist(8), 'pending other playlist'),
    ]);
    usePlayerStore.getState().setCurrentTime(18);

    usePlayerStore.getState().removePendingTrack(1, playlist(7));

    const state = usePlayerStore.getState();
    expect(state.queue.map(({ track: item, source }) => [item.title, source])).toEqual([
      ['played', playlist(7)],
      ['current', single()],
      ['pending library duplicate', library()],
      ['pending other music', playlist(7)],
      ['pending other playlist', playlist(8)],
    ]);
    expect(state.currentTrack?.title).toBe('current');
    expect(state.queueIndex).toBe(1);
    expect(state.currentTime).toBe(18);
    expect(state.playing).toBe(true);
  });

  it('当前 music 相同但来源不同，不转换也不删除其他来源项', () => {
    const selected = track(1, 'library current');
    usePlayerStore.getState().play(selected, [
      { track: selected, source: library() },
      entry(1, playlist(7), 'playlist pending'),
    ]);

    usePlayerStore.getState().removePendingTrack(1, playlist(8));

    expect(usePlayerStore.getState().queue.map(({ source }) => source)).toEqual([
      library(),
      playlist(7),
    ]);
  });

  it('hydrateTrack enriches current entry without stopping active playback', () => {
    const selected = track(1);
    usePlayerStore.getState().play(selected, [{ track: selected, source: library() }]);
    usePlayerStore.getState().setCurrentTime(12);

    usePlayerStore.getState().hydrateTrack({ ...selected, title: 'Hydrated Song' });

    const state = usePlayerStore.getState();
    expect(state.currentTrack?.title).toBe('Hydrated Song');
    expect(state.queue[0].track.title).toBe('Hydrated Song');
    expect(state.queue[0].source).toEqual(library());
    expect(state.playing).toBe(true);
    expect(state.currentTime).toBe(12);
  });

  it('无 Store 状态的 deep link 建立单条 SINGLE 且保持暂停', () => {
    usePlayerStore.getState().hydrateTrack(track(9));

    const state = usePlayerStore.getState();
    expect(state.currentTrack?.music_id).toBe(9);
    expect(state.queue).toEqual([{ track: track(9), source: single() }]);
    expect(state.queueIndex).toBe(0);
    expect(state.playing).toBe(false);
  });

  it('reset 完整恢复播放器初始值', () => {
    usePlayerStore.getState().play(track(1), [entry(1, library())]);
    usePlayerStore.getState().setVolume(0.2);
    usePlayerStore.getState().setCurrentTime(50);

    usePlayerStore.getState().reset();

    expect(usePlayerStore.getState()).toMatchObject({
      currentTrack: null,
      queue: [],
      queueIndex: -1,
      playing: false,
      currentTime: 0,
      duration: 0,
      volume: 0.8,
    });
  });

  it('stateRevision only changes for current or queue identity mutations', () => {
    const initialRevision = usePlayerStore.getState().stateRevision;
    usePlayerStore.getState().setCurrentTime(10);
    usePlayerStore.getState().setDuration(200);
    usePlayerStore.getState().setVolume(0.4);
    usePlayerStore.getState().seek(20);
    expect(usePlayerStore.getState().stateRevision).toBe(initialRevision);

    const selected = track(1);
    usePlayerStore.getState().play(selected, [
      { track: selected, source: playlist(7) },
      entry(2, playlist(7)),
    ]);
    const afterPlay = usePlayerStore.getState().stateRevision;
    expect(afterPlay).toBe(initialRevision + 1);

    usePlayerStore.getState().next();
    expect(usePlayerStore.getState().stateRevision).toBe(afterPlay + 1);
    usePlayerStore.getState().detachSource(playlist(7));
    expect(usePlayerStore.getState().stateRevision).toBe(afterPlay + 2);
  });
});
