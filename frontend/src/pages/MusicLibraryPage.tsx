import { useEffect, useState, useCallback } from 'react';
import { useMusicStore } from '../stores/music';
import { useAuthStore } from '../stores/auth';
import { usePlayerStore } from '../stores/player';
import { useToastStore } from '../stores/toast';
import { useNavigate } from 'react-router-dom';
import MusicCard from '../components/MusicCard';
import Pagination from '../components/Pagination';
import type { MusicMeta } from '../types/api';
import { getMusicDetail } from '../api/music';

const PAGE_SIZE = 20;

export default function MusicLibraryPage() {
  const library = useMusicStore((state) => state.library);
  const libraryTotal = useMusicStore((state) => state.libraryTotal);
  const libraryLoading = useMusicStore((state) => state.libraryLoading);
  const libraryError = useMusicStore((state) => state.libraryError);
  const fetchLibrary = useMusicStore((state) => state.fetchLibrary);
  const fetchPlaylists = useMusicStore((state) => state.fetchPlaylists);
  const userPlaylists = useMusicStore((state) => state.userPlaylists);
  const addToPlaylist = useMusicStore((state) => state.addToPlaylist);
  const user = useAuthStore((s) => s.user);
  const play = usePlayerStore((state) => state.play);
  const showSuccess = useToastStore((state) => state.success);
  const showInfo = useToastStore((state) => state.info);
  const navigate = useNavigate();
  const [page, setPage] = useState(1);
  const [search, setSearch] = useState('');
  const [debouncedSearch, setDebouncedSearch] = useState('');
  const [showPlaylistPicker, setShowPlaylistPicker] = useState<number | null>(null);
  const [playingMusicId, setPlayingMusicId] = useState<number | null>(null);
  const [addingMusicId, setAddingMusicId] = useState<number | null>(null);
  const [actionError, setActionError] = useState<string | null>(null);

  const fetchData = useCallback(async (pageNum: number, q?: string) => {
    const offset = (pageNum - 1) * PAGE_SIZE;
    await fetchLibrary(offset, PAGE_SIZE, q);
  }, [fetchLibrary]);

  useEffect(() => {
    const timeout = window.setTimeout(() => setDebouncedSearch(search.trim()), 300);
    return () => window.clearTimeout(timeout);
  }, [search]);

  useEffect(() => {
    void fetchData(page, debouncedSearch);
  }, [page, debouncedSearch, fetchData]);

  useEffect(() => {
    if (user) void fetchPlaylists(user.user_id).catch(() => {});
  }, [user, fetchPlaylists]);

  const handlePlay = async (music: MusicMeta) => {
    if (playingMusicId !== null) return;
    setActionError(null);
    setPlayingMusicId(music.music_id);
    try {
      const detail = await getMusicDetail(music.music_id);
      play(detail);
      navigate(`/player/${music.music_id}`);
    } catch (playError) {
      setActionError(errorMessage(playError, '音乐加载失败，请稍后重试'));
    } finally {
      setPlayingMusicId(null);
    }
  };

  const handleAddToPlaylist = async (musicId: number) => {
    if (userPlaylists.length === 0) {
      showInfo('还没有歌单，请先创建');
      return;
    }
    if (userPlaylists.length === 1) {
      setActionError(null);
      setAddingMusicId(musicId);
      try {
        await addToPlaylist(userPlaylists[0].id, musicId);
        showSuccess('已添加到歌单');
      } catch (addError) {
        setActionError(errorMessage(addError, '添加到歌单失败，请稍后重试'));
      } finally {
        setAddingMusicId(null);
      }
      return;
    }
    setShowPlaylistPicker(showPlaylistPicker === musicId ? null : musicId);
  };

  const handleSelectPlaylist = async (playlistId: number, musicId: number) => {
    setActionError(null);
    setAddingMusicId(musicId);
    try {
      await addToPlaylist(playlistId, musicId);
      showSuccess('已添加到歌单');
      setShowPlaylistPicker(null);
    } catch (addError) {
      setActionError(errorMessage(addError, '添加到歌单失败，请稍后重试'));
    } finally {
      setAddingMusicId(null);
    }
  };

  return (
    <div className="min-w-0">
      <h1 className="text-xl font-display text-primary mb-6">音乐库</h1>

      <div className="mb-6">
        <input
          type="text"
          aria-label="搜索音乐"
          placeholder="搜索音乐..."
          value={search}
          onChange={(e) => {
            setSearch(e.target.value);
            setPage(1);
          }}
          className="glass-input w-full max-w-md"
        />
      </div>

      {actionError && <p role="alert" className="mb-4 text-sm text-destructive">{actionError}</p>}

      {libraryLoading ? (
        <div role="status" className="text-center text-text-muted py-12">正在加载音乐...</div>
      ) : libraryError ? (
        <div role="alert" className="text-center py-12">
          <p className="text-sm text-destructive mb-4">{libraryError}</p>
          <button type="button" onClick={() => void fetchData(page, debouncedSearch)} className="glass-button text-sm">重试</button>
        </div>
      ) : library.length === 0 ? (
        <div className="text-center text-text-muted py-12">
          {debouncedSearch ? `没有找到与“${debouncedSearch}”匹配的音乐` : '暂无音乐'}
        </div>
      ) : (
        <>
          <div className="grid grid-cols-2 sm:grid-cols-3 lg:grid-cols-4 xl:grid-cols-5 gap-4">
            {library.map((m) => (
              <div key={m.music_id} className="relative">
                <MusicCard
                  music={m}
                  onPlay={handlePlay}
                  onAddToPlaylist={handleAddToPlaylist}
                  busyAction={
                    playingMusicId === m.music_id
                      ? 'play'
                      : addingMusicId === m.music_id
                        ? 'add'
                        : null
                  }
                />
                {showPlaylistPicker === m.music_id && (
                  <div className="absolute top-full mt-2 left-0 right-0 glass-card p-2 z-10" role="menu" aria-label="选择歌单">
                    <p className="text-xs text-text-muted px-2 py-1">添加到歌单：</p>
                    {userPlaylists.map((p) => (
                      <button
                        key={p.id}
                        type="button"
                        role="menuitem"
                        disabled={addingMusicId === m.music_id}
                        onClick={() => void handleSelectPlaylist(p.id, m.music_id)}
                        className="w-full text-left text-sm px-2 py-1.5 hover:bg-white/10 rounded-lg transition-colors"
                      >
                        {p.name} ({p.itemCount})
                      </button>
                    ))}
                  </div>
                )}
              </div>
            ))}
          </div>
          <Pagination current={page} total={libraryTotal} pageSize={PAGE_SIZE} onChange={setPage} />
        </>
      )}
    </div>
  );
}

function errorMessage(error: unknown, fallback: string): string {
  return error instanceof Error && error.message ? error.message : fallback;
}
