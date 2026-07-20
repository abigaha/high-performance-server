import { useEffect, useState, useCallback } from 'react';
import { useMusicStore } from '../stores/music';
import { useAuthStore } from '../stores/auth';
import { usePlayerStore } from '../stores/player';
import { useToastStore } from '../stores/toast';
import { useNavigate } from 'react-router-dom';
import MusicCard from '../components/MusicCard';
import Pagination from '../components/Pagination';
import type { MusicMeta } from '../types/api';

const PAGE_SIZE = 20;

export default function MusicLibraryPage() {
  const { library, libraryTotal, fetchLibrary, fetchPlaylists, userPlaylists, addToPlaylist } = useMusicStore();
  const user = useAuthStore((s) => s.user);
  const player = usePlayerStore();
  const toast = useToastStore();
  const navigate = useNavigate();
  const [page, setPage] = useState(1);
  const [search, setSearch] = useState('');
  const [showPlaylistPicker, setShowPlaylistPicker] = useState<number | null>(null);

  const fetchData = useCallback(async (pageNum: number, q?: string) => {
    const offset = (pageNum - 1) * PAGE_SIZE;
    await fetchLibrary(offset, PAGE_SIZE, q);
  }, [fetchLibrary]);

  useEffect(() => {
    fetchData(page, search);
    if (user) fetchPlaylists(user.user_id);
  }, [page, search, fetchData, user, fetchPlaylists]);

  const handlePlay = (music: MusicMeta) => {
    player.play(music, library);
    navigate(`/player/${music.music_id}`);
  };

  const handleAddToPlaylist = async (musicId: number) => {
    if (userPlaylists.length === 0) {
      toast.info('还没有歌单，请先创建');
      return;
    }
    if (userPlaylists.length === 1) {
      try {
        await addToPlaylist(userPlaylists[0].id, musicId);
        toast.success('已添加到歌单');
      } catch {
        toast.error('添加失败');
      }
      return;
    }
    setShowPlaylistPicker(showPlaylistPicker === musicId ? null : musicId);
  };

  const handleSelectPlaylist = async (playlistId: number, musicId: number) => {
    try {
      await addToPlaylist(playlistId, musicId);
      toast.success('已添加到歌单');
      setShowPlaylistPicker(null);
    } catch {
      toast.error('添加失败');
    }
  };

  return (
    <div>
      <h1 className="text-xl font-display text-primary mb-6">音乐库</h1>

      <div className="mb-6">
        <input
          type="text"
          placeholder="搜索音乐..."
          value={search}
          onChange={(e) => { setSearch(e.target.value); setPage(1); }}
          className="glass-input w-full max-w-md"
        />
      </div>

      {library.length === 0 ? (
        <div className="text-center text-text-muted py-12">暂无音乐</div>
      ) : (
        <>
          <div className="grid grid-cols-2 sm:grid-cols-3 lg:grid-cols-4 xl:grid-cols-5 gap-4">
            {library.map((m) => (
              <div key={m.music_id} className="relative">
                <MusicCard
                  music={m}
                  onPlay={handlePlay}
                  onAddToPlaylist={handleAddToPlaylist}
                />
                {showPlaylistPicker === m.music_id && (
                  <div className="absolute bottom-16 left-0 right-0 glass-card p-2 z-10">
                    <p className="text-xs text-text-muted px-2 py-1">添加到歌单：</p>
                    {userPlaylists.map((p) => (
                      <button
                        key={p.id}
                        onClick={() => handleSelectPlaylist(p.id, m.music_id)}
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
