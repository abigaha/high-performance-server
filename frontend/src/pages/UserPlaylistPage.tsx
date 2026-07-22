import { useCallback, useEffect, useRef, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { Play, Plus, Trash, X } from '@phosphor-icons/react';
import { getMusicDetail, createPlaylist, getPlaylistItems } from '../api/music';
import { useAuthStore } from '../stores/auth';
import { useMusicStore } from '../stores/music';
import { usePlayerStore } from '../stores/player';
import { useToastStore } from '../stores/toast';
import type { Playlist, PlaylistItem } from '../types/api';

export default function UserPlaylistPage() {
  const userPlaylists = useMusicStore((state) => state.userPlaylists);
  const fetchPlaylists = useMusicStore((state) => state.fetchPlaylists);
  const removeFromPlaylist = useMusicStore((state) => state.removeFromPlaylist);
  const user = useAuthStore((state) => state.user);
  const play = usePlayerStore((state) => state.play);
  const showSuccess = useToastStore((state) => state.success);
  const navigate = useNavigate();

  const [selectedPlaylist, setSelectedPlaylist] = useState<Playlist | null>(null);
  const [items, setItems] = useState<PlaylistItem[]>([]);
  const [showCreate, setShowCreate] = useState(false);
  const [newName, setNewName] = useState('');
  const [playlistsLoading, setPlaylistsLoading] = useState(true);
  const [playlistsError, setPlaylistsError] = useState<string | null>(null);
  const [itemsLoading, setItemsLoading] = useState(false);
  const [itemsError, setItemsError] = useState<string | null>(null);
  const [actionError, setActionError] = useState<string | null>(null);
  const [creating, setCreating] = useState(false);
  const [playingMusicId, setPlayingMusicId] = useState<number | null>(null);
  const [removingMusicId, setRemovingMusicId] = useState<number | null>(null);
  const itemsRequestIdRef = useRef(0);

  const loadPlaylists = useCallback(async () => {
    if (!user) {
      setPlaylistsLoading(false);
      return;
    }

    setPlaylistsLoading(true);
    setPlaylistsError(null);
    try {
      await fetchPlaylists(user.user_id);
    } catch (loadError) {
      setPlaylistsError(errorMessage(loadError, '歌单列表加载失败，请稍后重试'));
    } finally {
      setPlaylistsLoading(false);
    }
  }, [fetchPlaylists, user]);

  useEffect(() => {
    void loadPlaylists();
  }, [loadPlaylists]);

  const loadPlaylistItems = useCallback(async (playlist: Playlist) => {
    const requestId = ++itemsRequestIdRef.current;
    setSelectedPlaylist(playlist);
    setItemsLoading(true);
    setItemsError(null);
    setActionError(null);
    try {
      const data = await getPlaylistItems(playlist.id);
      if (requestId === itemsRequestIdRef.current) setItems(data);
    } catch (loadError) {
      if (requestId === itemsRequestIdRef.current) {
        setItems([]);
        setItemsError(errorMessage(loadError, '歌单内容加载失败，请稍后重试'));
      }
    } finally {
      if (requestId === itemsRequestIdRef.current) setItemsLoading(false);
    }
  }, []);

  const handleCreate = async (event: React.FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const name = newName.trim();
    if (!user || !name || creating) return;

    setActionError(null);
    setCreating(true);
    try {
      await createPlaylist(user.user_id, name);
      showSuccess('歌单已创建');
      setShowCreate(false);
      setNewName('');
      await fetchPlaylists(user.user_id);
    } catch (createError) {
      setActionError(errorMessage(createError, '歌单创建失败，请稍后重试'));
    } finally {
      setCreating(false);
    }
  };

  const handlePlay = async (item: PlaylistItem) => {
    if (playingMusicId !== null) return;
    setActionError(null);
    setPlayingMusicId(item.music_id);
    try {
      const detail = await getMusicDetail(item.music_id);
      play(detail);
      navigate(`/player/${item.music_id}`);
    } catch (playError) {
      setActionError(errorMessage(playError, '音乐加载失败，请稍后重试'));
    } finally {
      setPlayingMusicId(null);
    }
  };

  const handleRemove = async (item: PlaylistItem) => {
    if (!selectedPlaylist || removingMusicId !== null) return;
    if (!window.confirm(`确定从歌单移除“${item.title}”吗？`)) return;

    setActionError(null);
    setRemovingMusicId(item.music_id);
    try {
      await removeFromPlaylist(selectedPlaylist.id, item.music_id);
      setItems((current) => current.filter((candidate) => candidate.music_id !== item.music_id));
      showSuccess('已从歌单移除');
      if (user) void fetchPlaylists(user.user_id).catch(() => {});
    } catch (removeError) {
      setActionError(errorMessage(removeError, '歌曲移除失败，请稍后重试'));
    } finally {
      setRemovingMusicId(null);
    }
  };

  return (
    <div className="grid min-w-0 gap-6 lg:grid-cols-[18rem_minmax(0,1fr)]">
      <section className="min-w-0" aria-labelledby="playlist-heading">
        <div className="flex flex-wrap items-center justify-between gap-3 mb-4">
          <h1 id="playlist-heading" className="text-xl font-display text-primary">我的歌单</h1>
          <button
            type="button"
            aria-expanded={showCreate}
            onClick={() => setShowCreate((visible) => !visible)}
            className="glass-button !py-1.5 !px-3 !text-xs flex items-center gap-1"
          >
            {showCreate ? <X size={14} /> : <Plus size={14} />}
            {showCreate ? '取消' : '新建'}
          </button>
        </div>

        {showCreate && (
          <form onSubmit={(event) => void handleCreate(event)} className="glass-card p-3 mb-3 flex flex-col sm:flex-row lg:flex-col gap-2">
            <label htmlFor="playlist-name" className="sr-only">歌单名称</label>
            <input
              id="playlist-name"
              type="text"
              maxLength={100}
              placeholder="歌单名称"
              value={newName}
              onChange={(event) => setNewName(event.target.value)}
              disabled={creating}
              className="glass-input min-w-0 flex-1 !py-1.5 !text-sm"
            />
            <button type="submit" disabled={creating || !newName.trim()} className="glass-button !py-1.5 !text-xs">
              {creating ? '创建中...' : '创建'}
            </button>
          </form>
        )}

        {playlistsLoading ? (
          <p role="status" className="text-sm text-text-muted py-6">正在加载歌单...</p>
        ) : playlistsError ? (
          <div role="alert" className="py-4">
            <p className="text-sm text-destructive mb-3">{playlistsError}</p>
            <button type="button" onClick={() => void loadPlaylists()} className="glass-button !py-1.5 !text-xs">重试</button>
          </div>
        ) : userPlaylists.length === 0 ? (
          <p className="text-sm text-text-muted py-4">暂无歌单</p>
        ) : (
          <div className="flex gap-2 overflow-x-auto pb-2 lg:flex-col lg:overflow-visible">
            {userPlaylists.map((playlist) => {
              const fullPlaylist = toPlaylist(playlist, user?.user_id ?? 0);
              return (
                <button
                  key={playlist.id}
                  type="button"
                  aria-pressed={selectedPlaylist?.id === playlist.id}
                  onClick={() => void loadPlaylistItems(fullPlaylist)}
                  className={`glass-card min-w-44 p-3 text-left transition-colors lg:min-w-0 ${
                    selectedPlaylist?.id === playlist.id ? 'ring-2 ring-primary' : ''
                  }`}
                >
                  <span className="block text-sm font-medium truncate">{playlist.name}</span>
                  <span className="block text-xs text-text-muted">{playlist.itemCount} 首</span>
                </button>
              );
            })}
          </div>
        )}
      </section>

      <section className="min-w-0" aria-live="polite">
        {actionError && <p role="alert" className="mb-4 text-sm text-destructive">{actionError}</p>}
        {!selectedPlaylist ? (
          <div className="text-center text-text-muted py-12">请选择一个歌单</div>
        ) : itemsLoading ? (
          <div role="status" className="text-center text-text-muted py-12">正在加载歌单内容...</div>
        ) : itemsError ? (
          <div role="alert" className="text-center py-12">
            <p className="text-sm text-destructive mb-4">{itemsError}</p>
            <button type="button" onClick={() => void loadPlaylistItems(selectedPlaylist)} className="glass-button text-sm">重试</button>
          </div>
        ) : items.length === 0 ? (
          <div className="text-center text-text-muted py-12">歌单为空</div>
        ) : (
          <>
            <h2 className="text-lg font-display text-primary mb-4 break-words">{selectedPlaylist.name}</h2>
            <div className="flex flex-col gap-3">
              {items.map((item) => (
                <article key={item.id} className="glass-card p-3 flex items-center gap-3 min-w-0">
                  <button
                    type="button"
                    aria-label={`播放 ${item.title}`}
                    disabled={playingMusicId !== null || removingMusicId !== null}
                    onClick={() => void handlePlay(item)}
                    className="w-9 h-9 rounded-full bg-primary/20 flex items-center justify-center shrink-0 hover:bg-primary/30 transition-colors disabled:opacity-50"
                  >
                    <Play size={14} weight="fill" className="text-primary" />
                  </button>
                  <div className="flex-1 min-w-0">
                    <p className="text-sm font-medium truncate">{item.title}</p>
                    <p className="text-xs text-text-muted truncate">{item.artist || '未知艺术家'}</p>
                  </div>
                  <button
                    type="button"
                    aria-label={`从歌单移除 ${item.title}`}
                    disabled={removingMusicId !== null || playingMusicId !== null}
                    onClick={() => void handleRemove(item)}
                    className="min-h-9 px-2 text-xs text-text-muted hover:text-destructive transition-colors disabled:opacity-50 shrink-0 flex items-center gap-1"
                  >
                    <Trash size={14} />
                    <span className="hidden sm:inline">{removingMusicId === item.music_id ? '移除中' : '移除'}</span>
                  </button>
                </article>
              ))}
            </div>
          </>
        )}
      </section>
    </div>
  );
}

function toPlaylist(
  playlist: { id: number; name: string; itemCount: number },
  userId: number,
): Playlist {
  return {
    id: playlist.id,
    user_id: userId,
    name: playlist.name,
    description: '',
    item_count: playlist.itemCount,
    created_at: '',
  };
}

function errorMessage(error: unknown, fallback: string): string {
  return error instanceof Error && error.message ? error.message : fallback;
}
