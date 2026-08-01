import type { MusicMeta } from '../types/api';
import { Play, Plus, Trash } from '@phosphor-icons/react';
import { getCoverPlaceholder } from '../lib/coverPlaceholder';

interface Props {
  music: MusicMeta;
  inPlaylist?: boolean;
  onPlay: (music: MusicMeta) => void;
  onAddToPlaylist?: (musicId: number) => void;
  onRemove?: (musicId: number) => void;
  busyAction?: 'play' | 'add' | 'remove' | null;
}

function formatDuration(sec: number): string {
  const m = Math.floor(sec / 60);
  const s = Math.floor(sec % 60);
  return `${m}:${s.toString().padStart(2, '0')}`;
}

export default function MusicCard({
  music,
  inPlaylist,
  onPlay,
  onAddToPlaylist,
  onRemove,
  busyAction = null,
}: Props) {
  return (
    <div className="glass-card p-4 flex flex-col gap-3 hover:shadow-lg transition-shadow">
      <div className="aspect-square w-full overflow-hidden rounded-xl bg-[var(--surface-subtle)]">
        <img
          src={getCoverPlaceholder(music.music_id)}
          alt=""
          aria-hidden="true"
          className="h-full w-full object-cover"
        />
      </div>
      <div className="flex-1 min-w-0">
        <p className="text-sm font-medium truncate">{music.title}</p>
        <p className="text-xs text-text-muted truncate">{music.artist}</p>
      </div>
      <div className="flex items-center justify-between text-xs text-text-muted">
        <span>{music.album || '—'}</span>
        <span>{formatDuration(music.duration_sec)}</span>
      </div>
      <div className="flex flex-wrap gap-2 pt-1" onClick={(event) => event.stopPropagation()}>
        <button
          type="button"
          aria-label={`播放 ${music.title}`}
          disabled={busyAction !== null}
          onClick={(event) => {
            event.stopPropagation();
            onPlay(music);
          }}
          className="glass-button min-h-11 min-w-11 !py-1.5 !px-3 !text-xs flex items-center gap-1"
        >
          <Play size={14} weight="fill" /> {busyAction === 'play' ? '载入中' : '播放'}
        </button>
        {onAddToPlaylist && (
          <button
            type="button"
            aria-label={`将 ${music.title} 添加到歌单`}
            disabled={busyAction !== null}
            onClick={(event) => {
              event.stopPropagation();
              onAddToPlaylist(music.music_id);
            }}
            className="glass-button min-h-11 min-w-11 !py-1.5 !px-3 !text-xs flex items-center gap-1 !bg-transparent !text-primary !border !border-primary/30"
          >
            <Plus size={14} /> {busyAction === 'add' ? '添加中' : '添加'}
          </button>
        )}
        {inPlaylist && onRemove && (
          <button
            type="button"
            aria-label={`从歌单移除 ${music.title}`}
            disabled={busyAction !== null}
            onClick={(event) => {
              event.stopPropagation();
              onRemove(music.music_id);
            }}
            className="glass-button min-h-11 min-w-11 !py-1.5 !px-3 !text-xs flex items-center gap-1 !bg-red-500/80"
          >
            <Trash size={14} /> {busyAction === 'remove' ? '移除中' : '移除'}
          </button>
        )}
      </div>
    </div>
  );
}
