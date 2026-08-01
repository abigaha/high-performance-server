import { useCallback, useEffect, useRef, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { Pencil, Play, Plus, Trash, X } from '@phosphor-icons/react';
import { captureSessionSnapshot, isSessionSnapshotCurrent } from '../api/client';
import { getMusicDetail, getPlaylistItems } from '../api/music';
import { useAuthStore } from '../stores/auth';
import { useMusicStore } from '../stores/music';
import {
  capturePlayerGeneration,
  capturePlayerStateRevision,
  isPlayerGenerationCurrent,
  usePlayerStore,
} from '../stores/player';
import { useToastStore } from '../stores/toast';
import { getCoverPlaceholder } from '../lib/coverPlaceholder';
import type { Playlist, PlaylistItem } from '../types/api';

export default function UserPlaylistPage() {
  const userPlaylists = useMusicStore((state) => state.userPlaylists);
  const fetchPlaylists = useMusicStore((state) => state.fetchPlaylists);
  const createPlaylist = useMusicStore((state) => state.createPlaylist);
  const renamePlaylist = useMusicStore((state) => state.renamePlaylist);
  const deletePlaylist = useMusicStore((state) => state.deletePlaylist);
  const removeFromPlaylist = useMusicStore((state) => state.removeFromPlaylist);
  const user = useAuthStore((state) => state.user);
  const sessionRevision = useAuthStore((state) => state.sessionRevision);
  const play = usePlayerStore((state) => state.play);
  const showSuccess = useToastStore((state) => state.success);
  const navigate = useNavigate();
  const userId = user?.user_id;

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
  const [removingMusicIds, setRemovingMusicIds] = useState<Set<number>>(() => new Set());
  const [deletingPlaylist, setDeletingPlaylist] = useState(false);
  const [renamingPlaylist, setRenamingPlaylist] = useState(false);
  const [renameName, setRenameName] = useState('');
  const itemsRequestIdRef = useRef(0);
  const playlistsRequestIdRef = useRef(0);
  const playRequestIdRef = useRef(0);
  const mountedRef = useRef(false);
  const sessionScopeRef = useRef(0);
  const operationIdsRef = useRef(new Map<string, number>());

  const startOperation = (key: string) => {
    const id = (operationIdsRef.current.get(key) ?? 0) + 1;
    operationIdsRef.current.set(key, id);
    return id;
  };
  const isOperationCurrent = (key: string, id: number) => operationIdsRef.current.get(key) === id;

  useEffect(() => {
    mountedRef.current = true;
    const operationIds = operationIdsRef.current;
    return () => {
      mountedRef.current = false;
      itemsRequestIdRef.current += 1;
      playlistsRequestIdRef.current += 1;
      playRequestIdRef.current += 1;
      sessionScopeRef.current += 1;
      operationIds.clear();
    };
  }, []);

  useEffect(() => {
    sessionScopeRef.current += 1;
    itemsRequestIdRef.current += 1;
    playlistsRequestIdRef.current += 1;
    playRequestIdRef.current += 1;
    operationIdsRef.current.clear();
    setSelectedPlaylist(null);
    setItems([]);
    setShowCreate(false);
    setNewName('');
    setPlaylistsError(null);
    setItemsError(null);
    setActionError(null);
    setCreating(false);
    setPlayingMusicId(null);
    setRemovingMusicIds(new Set());
    setDeletingPlaylist(false);
    setRenamingPlaylist(false);
    setRenameName('');
  }, [sessionRevision]);

  const loadPlaylists = useCallback(async () => {
    if (userId === undefined) {
      setPlaylistsLoading(false);
      return;
    }

    const requestId = ++playlistsRequestIdRef.current;
    const sessionScope = sessionScopeRef.current;
    const session = captureSessionSnapshot();
    const isCurrent = () => mountedRef.current
      && requestId === playlistsRequestIdRef.current
      && sessionScope === sessionScopeRef.current
      && isSessionSnapshotCurrent(session);
    setPlaylistsLoading(true);
    setPlaylistsError(null);
    try {
      await fetchPlaylists(userId);
    } catch (loadError) {
      if (isCurrent()) setPlaylistsError(errorMessage(loadError, '歌单列表加载失败，请稍后重试'));
    } finally {
      if (isCurrent()) setPlaylistsLoading(false);
    }
  }, [fetchPlaylists, userId]);

  useEffect(() => {
    void loadPlaylists();
  }, [loadPlaylists, sessionRevision]);

  const loadPlaylistItems = useCallback(async (playlist: Playlist) => {
    const requestId = ++itemsRequestIdRef.current;
    const sessionScope = sessionScopeRef.current;
    const session = captureSessionSnapshot();
    const isCurrent = () => mountedRef.current
      && requestId === itemsRequestIdRef.current
      && sessionScope === sessionScopeRef.current
      && isSessionSnapshotCurrent(session);
    playRequestIdRef.current += 1;
    setSelectedPlaylist(playlist);
    setItemsLoading(true);
    setItemsError(null);
    setActionError(null);
    setRemovingMusicIds(new Set());
    setDeletingPlaylist(false);
    setRenamingPlaylist(false);
    setRenameName('');
    try {
      const data = await getPlaylistItems(playlist.id);
      if (isCurrent()) setItems(data);
    } catch (loadError) {
      if (isCurrent()) {
        setItems([]);
        setItemsError(errorMessage(loadError, '歌单内容加载失败，请稍后重试'));
      }
    } finally {
      if (isCurrent()) setItemsLoading(false);
    }
  }, []);

  const handleCreate = async (event: React.FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const name = newName.trim();
    if (!user || !name || creating) return;

    setActionError(null);
    setCreating(true);
    const operationKey = 'create';
    const operationId = startOperation(operationKey);
    const sessionScope = sessionScopeRef.current;
    const session = captureSessionSnapshot();
    const isCurrent = () => mountedRef.current
      && sessionScope === sessionScopeRef.current
      && isOperationCurrent(operationKey, operationId)
      && isSessionSnapshotCurrent(session);
    try {
      await createPlaylist(user.user_id, name);
      if (!isCurrent()) return;
      showSuccess('歌单已创建');
      setShowCreate(false);
      setNewName('');
    } catch (createError) {
      if (isCurrent()) setActionError(errorMessage(createError, '歌单创建失败，请稍后重试'));
    } finally {
      if (isCurrent()) setCreating(false);
    }
  };

  const handlePlay = async (item: PlaylistItem) => {
    const requestId = ++playRequestIdRef.current;
    const sessionScope = sessionScopeRef.current;
    const session = captureSessionSnapshot();
    const playerGeneration = capturePlayerGeneration();
    const playerStateRevision = capturePlayerStateRevision();
    const isActiveRequest = () => mountedRef.current
      && requestId === playRequestIdRef.current
      && sessionScope === sessionScopeRef.current
      && isSessionSnapshotCurrent(session);
    const canApplyResult = () => isActiveRequest()
      && isPlayerGenerationCurrent(playerGeneration)
      && capturePlayerStateRevision() === playerStateRevision;
    setActionError(null);
    setPlayingMusicId(item.music_id);
    try {
      const detail = await getMusicDetail(item.music_id);
      if (!canApplyResult()) return;
      play(detail, items.map((candidate) => ({
        track: candidate.id === item.id ? detail : playlistItemToMusic(candidate),
        source: { kind: 'PLAYLIST', id: selectedPlaylist?.id ?? candidate.playlist_id },
      })));
      navigate(`/player/${item.music_id}`);
    } catch (playError) {
      if (canApplyResult()) {
        setActionError(errorMessage(playError, '音乐加载失败，请稍后重试'));
      }
    } finally {
      if (isActiveRequest()) setPlayingMusicId(null);
    }
  };

  const handleRemove = async (item: PlaylistItem) => {
    if (!selectedPlaylist || removingMusicIds.has(item.music_id)) return;
    if (!window.confirm(`确定从歌单移除“${item.title}”吗？`)) return;

    setActionError(null);
    setRemovingMusicIds((current) => new Set(current).add(item.music_id));
    const operationKey = `remove:${selectedPlaylist.id}:${item.music_id}`;
    const operationId = startOperation(operationKey);
    const selectedRequestId = itemsRequestIdRef.current;
    const sessionScope = sessionScopeRef.current;
    const session = captureSessionSnapshot();
    const isCurrent = () => mountedRef.current
      && selectedRequestId === itemsRequestIdRef.current
      && sessionScope === sessionScopeRef.current
      && isOperationCurrent(operationKey, operationId)
      && isSessionSnapshotCurrent(session);
    try {
      await removeFromPlaylist(selectedPlaylist.id, item.music_id);
      if (!isCurrent()) return;
      setItems((current) => current.filter((candidate) => candidate.music_id !== item.music_id));
      showSuccess('已从歌单移除');
    } catch (removeError) {
      if (isCurrent()) setActionError(errorMessage(removeError, '歌曲移除失败，请稍后重试'));
    } finally {
      if (isCurrent()) setRemovingMusicIds((current) => {
        const next = new Set(current);
        next.delete(item.music_id);
        return next;
      });
    }
  };

  const handleDeletePlaylist = async () => {
    if (!selectedPlaylist || deletingPlaylist) return;
    if (!window.confirm(`确定删除歌单“${selectedPlaylist.name}”吗？`)) return;

    const target = selectedPlaylist;
    setActionError(null);
    setDeletingPlaylist(true);
    const operationKey = `delete:${target.id}`;
    const operationId = startOperation(operationKey);
    const selectedRequestId = itemsRequestIdRef.current;
    const sessionScope = sessionScopeRef.current;
    const session = captureSessionSnapshot();
    const isCurrent = () => mountedRef.current
      && selectedRequestId === itemsRequestIdRef.current
      && sessionScope === sessionScopeRef.current
      && isOperationCurrent(operationKey, operationId)
      && isSessionSnapshotCurrent(session);
    try {
      await deletePlaylist(target.id);
      if (!isCurrent()) return;
      setSelectedPlaylist(null);
      setItems([]);
      showSuccess('歌单已删除');
    } catch (deleteError) {
      if (isCurrent()) setActionError(errorMessage(deleteError, '歌单删除失败，请稍后重试'));
    } finally {
      if (isCurrent()) setDeletingPlaylist(false);
    }
  };

  const handleRenamePlaylist = async (event: React.FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    if (!selectedPlaylist || !renameName.trim() || renamingPlaylist) return;
    const target = selectedPlaylist;
    const operationKey = `rename:${target.id}`;
    const operationId = startOperation(operationKey);
    const selectedRequestId = itemsRequestIdRef.current;
    const sessionScope = sessionScopeRef.current;
    const session = captureSessionSnapshot();
    const name = renameName.trim();
    const isCurrent = () => mountedRef.current
      && selectedRequestId === itemsRequestIdRef.current
      && sessionScope === sessionScopeRef.current
      && isOperationCurrent(operationKey, operationId)
      && isSessionSnapshotCurrent(session);
    setRenamingPlaylist(true);
    setActionError(null);
    try {
      await renamePlaylist(target.id, name, target.description);
      if (!isCurrent()) return;
      setSelectedPlaylist({ ...target, name });
      setRenameName('');
      showSuccess('歌单已重命名');
    } catch (renameError) {
      if (isCurrent()) setActionError(errorMessage(renameError, '歌单重命名失败，请稍后重试'));
    } finally {
      if (isCurrent()) setRenamingPlaylist(false);
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
            className="glass-button min-h-11 min-w-11 !py-1.5 !px-3 !text-xs flex items-center gap-1"
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
              className="glass-input min-h-11 min-w-11 flex-1 !py-1.5 !text-sm"
            />
            <button type="submit" disabled={creating || !newName.trim()} className="glass-button min-h-11 min-w-11 !py-1.5 !text-xs">
              {creating ? '创建中...' : '创建'}
            </button>
          </form>
        )}

        {playlistsLoading ? (
          <p role="status" className="text-sm text-text-muted py-6">正在加载歌单...</p>
        ) : playlistsError ? (
          <div role="alert" className="py-4">
            <p className="text-sm text-destructive mb-3">{playlistsError}</p>
            <button type="button" onClick={() => void loadPlaylists()} className="glass-button min-h-11 min-w-11 !py-1.5 !text-xs">重试</button>
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
                  className={`glass-card min-h-11 min-w-44 p-3 text-left transition-colors lg:min-w-0 ${
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
            <button type="button" onClick={() => void loadPlaylistItems(selectedPlaylist)} className="glass-button min-h-11 min-w-11 text-sm">重试</button>
          </div>
        ) : items.length === 0 ? (
          <div className="py-12 text-center text-text-muted"><div className="mb-4 flex justify-center gap-2"><button type="button" aria-label={`重命名歌单 ${selectedPlaylist.name}`} onClick={() => setRenameName(selectedPlaylist.name)} className="icon-button min-h-11 min-w-11" title="重命名歌单"><Pencil size={18} /></button><button type="button" aria-label={`删除歌单 ${selectedPlaylist.name}`} disabled={deletingPlaylist} onClick={() => void handleDeletePlaylist()} className="icon-button min-h-11 min-w-11 text-text-muted hover:text-destructive disabled:opacity-50" title="删除歌单"><Trash size={18} /></button></div>{renameName && <form onSubmit={(event) => void handleRenamePlaylist(event)} className="mx-auto mb-4 flex max-w-md gap-2 text-left"><label htmlFor="playlist-rename-empty" className="sr-only">歌单新名称</label><input id="playlist-rename-empty" value={renameName} onChange={(event) => setRenameName(event.target.value)} className="glass-input min-h-11 min-w-11 flex-1" disabled={renamingPlaylist} /><button type="submit" className="glass-button min-h-11 min-w-11" disabled={renamingPlaylist || !renameName.trim()}>保存</button></form>}<p>歌单为空</p></div>
        ) : (
          <>
            <div className="mb-4 flex min-w-0 items-center justify-between gap-3">
              <h2 className="min-w-0 break-words text-lg font-display text-primary">{selectedPlaylist.name}</h2>
              <div className="flex shrink-0 gap-2"><button type="button" aria-label={`重命名歌单 ${selectedPlaylist.name}`} onClick={() => setRenameName(selectedPlaylist.name)} className="icon-button min-h-11 min-w-11" title="重命名歌单"><Pencil size={18} /></button><button type="button" aria-label={`删除歌单 ${selectedPlaylist.name}`} disabled={deletingPlaylist} onClick={() => void handleDeletePlaylist()} className="icon-button min-h-11 min-w-11 text-text-muted hover:text-destructive disabled:opacity-50" title="删除歌单"><Trash size={18} /></button></div>
            </div>
            {renameName && <form onSubmit={(event) => void handleRenamePlaylist(event)} className="mb-4 flex gap-2"><label htmlFor="playlist-rename" className="sr-only">歌单新名称</label><input id="playlist-rename" value={renameName} onChange={(event) => setRenameName(event.target.value)} className="glass-input min-h-11 min-w-11 flex-1" disabled={renamingPlaylist} /><button type="submit" className="glass-button min-h-11 min-w-11" disabled={renamingPlaylist || !renameName.trim()}>保存</button></form>}
            <div className="flex flex-col gap-3">
              {items.map((item) => (
                <article
                  key={item.id}
                  className="glass-card grid min-w-0 grid-cols-[3rem_minmax(0,1fr)] items-center gap-3 p-3 sm:grid-cols-[3rem_minmax(0,1fr)_auto]"
                >
                  <img
                    src={getCoverPlaceholder(item.music_id)}
                    alt=""
                    aria-hidden="true"
                    className="aspect-square w-12 rounded-lg object-cover"
                  />
                  <div className="flex-1 min-w-0">
                    <p className="text-sm font-medium truncate">{item.title}</p>
                    <p className="text-xs text-text-muted truncate">{item.artist || '未知艺术家'}</p>
                  </div>
                  <div className="col-span-2 flex min-w-0 flex-wrap gap-2 sm:col-span-1 sm:flex-nowrap">
                    <button
                      type="button"
                      aria-label={`播放 ${item.title}`}
                      disabled={playingMusicId === item.music_id || removingMusicIds.has(item.music_id)}
                      onClick={() => void handlePlay(item)}
                      className="flex min-h-11 min-w-11 items-center gap-1 rounded-lg bg-primary/20 px-3 text-xs text-primary transition-colors hover:bg-primary/30 disabled:opacity-50"
                    >
                      <Play size={14} weight="fill" className="shrink-0" />
                      {playingMusicId === item.music_id ? '载入中' : '播放'}
                    </button>
                    <button
                      type="button"
                      aria-label={`从歌单移除 ${item.title}`}
                      disabled={removingMusicIds.has(item.music_id) || playingMusicId === item.music_id}
                      onClick={() => void handleRemove(item)}
                      className="flex min-h-11 min-w-11 items-center gap-1 px-3 text-xs text-text-muted transition-colors hover:text-destructive disabled:opacity-50"
                    >
                      <Trash size={14} className="shrink-0" />
                      {removingMusicIds.has(item.music_id) ? '移除中' : '移除'}
                    </button>
                  </div>
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
  playlist: { id: number; name: string; description?: string; itemCount: number },
  userId: number,
): Playlist {
  return {
    id: playlist.id,
    user_id: userId,
    name: playlist.name,
    description: playlist.description ?? '',
    item_count: playlist.itemCount,
    created_at: '',
  };
}

function playlistItemToMusic(item: PlaylistItem) {
  return {
    music_id: item.music_id,
    title: item.title,
    artist: item.artist,
    album: '',
    genre: '',
    duration_sec: 0,
    file_hash: item.file_hash,
    file_size: 0,
    content_type: 'audio/mpeg',
  };
}

function errorMessage(error: unknown, fallback: string): string {
  return error instanceof Error && error.message ? error.message : fallback;
}
