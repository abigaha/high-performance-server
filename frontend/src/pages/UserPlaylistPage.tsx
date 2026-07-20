import { useEffect, useState } from 'react';
import { useMusicStore } from '../stores/music';
import { useAuthStore } from '../stores/auth';
import { usePlayerStore } from '../stores/player';
import { useToastStore } from '../stores/toast';
import { useNavigate } from 'react-router-dom';
import { Plus, Play } from '@phosphor-icons/react';
import * as musicApi from '../api/music';
import type { Playlist, PlaylistItem } from '../types/api';

export default function UserPlaylistPage() {
  const { userPlaylists, fetchPlaylists, removeFromPlaylist } = useMusicStore();
  const user = useAuthStore((s) => s.user);
  const player = usePlayerStore();
  const toast = useToastStore();
  const navigate = useNavigate();

  const [selectedPlaylist, setSelectedPlaylist] = useState<Playlist | null>(null);
  const [items, setItems] = useState<PlaylistItem[]>([]);
  const [showCreate, setShowCreate] = useState(false);
  const [newName, setNewName] = useState('');
  const [loading, setLoading] = useState(false);

  useEffect(() => {
    if (user) fetchPlaylists(user.user_id);
  }, [user, fetchPlaylists]);

  const selectPlaylist = async (p: Playlist) => {
    setSelectedPlaylist(p);
    setLoading(true);
    try {
      const data = await musicApi.getPlaylistItems(p.id);
      setItems(data);
    } catch {
      toast.error('加载歌单失败');
    } finally {
      setLoading(false);
    }
  };

  const handleCreate = async () => {
    if (!user || !newName.trim()) return;
    try {
      await musicApi.createPlaylist(user.user_id, newName.trim());
      toast.success('歌单已创建');
      setShowCreate(false);
      setNewName('');
      fetchPlaylists(user.user_id);
    } catch {
      toast.error('创建失败');
    }
  };

  const handlePlay = async (item: PlaylistItem) => {
    const music = {
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
    const list = items.map((i) => ({
      music_id: i.music_id, title: i.title, artist: i.artist, album: '', genre: '',
      duration_sec: 0, file_hash: i.file_hash, file_size: 0, content_type: 'audio/mpeg',
    }));
    player.play(music, list);
    navigate(`/player/${item.music_id}`);
  };

  const handleRemove = async (musicId: number) => {
    if (!selectedPlaylist) return;
    try {
      await removeFromPlaylist(selectedPlaylist.id, musicId);
      toast.success('已移除');
      selectPlaylist(selectedPlaylist);
    } catch {
      toast.error('移除失败');
    }
  };

  return (
    <div className="flex gap-6">
      <div className="w-72 shrink-0">
        <div className="flex items-center justify-between mb-4">
          <h1 className="text-xl font-display text-primary">我的歌单</h1>
          <button onClick={() => setShowCreate(!showCreate)} className="glass-button !py-1.5 !px-3 !text-xs flex items-center gap-1">
            <Plus size={14} /> 新建
          </button>
        </div>

        {showCreate && (
          <div className="glass-card p-3 mb-3 flex gap-2">
            <input
              type="text"
              placeholder="歌单名称"
              value={newName}
              onChange={(e) => setNewName(e.target.value)}
              className="glass-input flex-1 !py-1.5 !text-sm"
            />
            <button onClick={handleCreate} className="glass-button !py-1.5 !text-xs">确定</button>
          </div>
        )}

        <div className="flex flex-col gap-2">
          {userPlaylists.length === 0 ? (
            <p className="text-sm text-text-muted">暂无歌单</p>
          ) : (
            userPlaylists.map((p) => (
              <button
                key={p.id}
                onClick={() => selectPlaylist({ id: p.id, user_id: user?.user_id || 0, name: p.name, description: '', item_count: p.itemCount, created_at: '' })}
                className={`glass-card p-3 text-left transition-all hover:shadow-md ${
                  selectedPlaylist?.id === p.id ? 'ring-1 ring-primary' : ''
                }`}
              >
                <p className="text-sm font-medium truncate">{p.name}</p>
                <p className="text-xs text-text-muted">{p.itemCount} 首</p>
              </button>
            ))
          )}
        </div>
      </div>

      <div className="flex-1">
        {!selectedPlaylist ? (
          <div className="text-center text-text-muted py-12">请选择一个歌单</div>
        ) : loading ? (
          <div className="text-center text-text-muted py-12">加载中...</div>
        ) : items.length === 0 ? (
          <div className="text-center text-text-muted py-12">歌单为空</div>
        ) : (
          <>
            <h2 className="text-lg font-display text-primary mb-4">{selectedPlaylist.name}</h2>
            <div className="flex flex-col gap-3">
              {items.map((item) => (
                <div key={item.id} className="glass-card p-3 flex items-center gap-3">
                  <button
                    onClick={() => handlePlay(item)}
                    className="w-8 h-8 rounded-full bg-primary/20 flex items-center justify-center shrink-0 hover:bg-primary/30 transition-colors"
                  >
                    <Play size={14} weight="fill" className="text-primary" />
                  </button>
                  <div className="flex-1 min-w-0">
                    <p className="text-sm font-medium truncate">{item.title}</p>
                    <p className="text-xs text-text-muted">{item.artist}</p>
                  </div>
                  <button
                    onClick={() => handleRemove(item.music_id)}
                    className="text-xs text-text-muted hover:text-destructive transition-colors"
                  >
                    移除
                  </button>
                </div>
              ))}
            </div>
          </>
        )}
      </div>
    </div>
  );
}
