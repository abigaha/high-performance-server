import { useEffect, useRef, useState } from 'react';
import { useLocation } from 'react-router-dom';
import { Pause, Play, SkipBack, SkipForward, SpeakerHigh } from '@phosphor-icons/react';
import { getFileStreamUrl } from '../api/files';
import { usePlayerStore } from '../stores/player';
import type { MusicMeta } from '../types/api';

interface Props {
  mode: 'mini' | 'fullscreen';
}

interface StreamableTrack extends MusicMeta {
  file_id?: number;
}

export default function AudioPlayer({ mode }: Props) {
  const location = useLocation();
  const audioRef = useRef<HTMLAudioElement>(null);
  const ownedStreamUrlRef = useRef<string | null>(null);
  const [streamUrl, setStreamUrl] = useState('');
  const [playbackError, setPlaybackError] = useState<string | null>(null);
  const {
    currentTrack,
    playlist,
    playlistIndex,
    playing,
    currentTime,
    duration,
    volume,
    pause,
    resume,
    seek,
    next,
    prev,
    setVolume,
    setCurrentTime,
    setDuration,
  } = usePlayerStore();

  const visible = mode === 'fullscreen' || !location.pathname.startsWith('/player/');
  const streamFileId = (currentTrack as StreamableTrack | null)?.file_id;

  useEffect(() => {
    let active = true;
    const releaseOwnedUrl = () => {
      const ownedUrl = ownedStreamUrlRef.current;
      if (!ownedUrl) return;
      ownedStreamUrlRef.current = null;
      URL.revokeObjectURL(ownedUrl);
    };

    releaseOwnedUrl();
    setStreamUrl('');
    setPlaybackError(null);

    if (!visible || !currentTrack) {
      return () => {
        active = false;
        releaseOwnedUrl();
      };
    }
    if (!streamFileId) {
      setPlaybackError('缺少可播放文件信息，请重新打开该音乐');
      return () => {
        active = false;
        releaseOwnedUrl();
      };
    }

    void getFileStreamUrl(streamFileId)
      .then((url) => {
        if (!active) {
          URL.revokeObjectURL(url);
          return;
        }
        releaseOwnedUrl();
        ownedStreamUrlRef.current = url;
        setStreamUrl(url);
      })
      .catch((error: unknown) => {
        if (active) setPlaybackError(errorMessage(error, '无法生成音频地址'));
      });

    return () => {
      active = false;
      releaseOwnedUrl();
    };
  }, [currentTrack, streamFileId, visible]);

  useEffect(() => {
    const audio = audioRef.current;
    if (!visible || !audio || !currentTrack || !streamUrl) return;

    if (playing) {
      void audio.play().catch((error: unknown) => {
        setPlaybackError(`播放失败：${errorMessage(error, '浏览器拒绝了播放请求')}`);
        pause();
      });
    } else {
      audio.pause();
    }
  }, [playing, currentTrack, pause, streamUrl, visible]);

  useEffect(() => {
    if (audioRef.current) audioRef.current.volume = volume;
  }, [volume, streamUrl]);

  if (!visible || !currentTrack) return null;

  const effectiveDuration = duration > 0 ? duration : Math.max(0, currentTrack.duration_sec);
  const seekMaximum = Math.max(effectiveDuration, 1);
  const seekValue = Math.min(Math.max(currentTime, 0), seekMaximum);
  const canGoPrevious = playlistIndex > 0;
  const canGoNext = playlistIndex >= 0 && playlistIndex < playlist.length - 1;

  const handleSeek = (value: number) => {
    const bounded = Math.min(Math.max(value, 0), effectiveDuration);
    const audio = audioRef.current;
    if (audio) audio.currentTime = bounded;
    seek(bounded);
  };

  const handleLoadedMetadata = () => {
    const audio = audioRef.current;
    if (!audio) return;
    const mediaDuration = Number.isFinite(audio.duration) ? audio.duration : 0;
    setDuration(mediaDuration);
    if (currentTime > 0 && currentTime < mediaDuration) audio.currentTime = currentTime;
  };

  const media = (
    <audio
      ref={audioRef}
      src={streamUrl}
      preload="metadata"
      onLoadedMetadata={handleLoadedMetadata}
      onTimeUpdate={(event) => setCurrentTime(event.currentTarget.currentTime)}
      onEnded={next}
      onError={() => {
        setPlaybackError('音频加载失败，请检查文件是否仍然可用');
        pause();
      }}
    />
  );

  const timeline = (
    <div className="flex min-w-0 flex-1 items-center gap-2 sm:gap-3">
      <span className="w-10 shrink-0 text-right text-xs text-text-muted">{formatTime(currentTime)}</span>
      <input
        type="range"
        aria-label="播放进度"
        aria-valuetext={`${formatTime(currentTime)} / ${formatTime(effectiveDuration)}`}
        min={0}
        max={seekMaximum}
        step={0.1}
        value={seekValue}
        disabled={effectiveDuration <= 0}
        onChange={(event) => handleSeek(Number(event.target.value))}
        className="min-w-20 flex-1 accent-primary disabled:opacity-50"
      />
      <span className="w-10 shrink-0 text-xs text-text-muted">{formatTime(effectiveDuration)}</span>
    </div>
  );

  const transport = (
    <div className="flex shrink-0 items-center gap-2 sm:gap-3">
      <button
        type="button"
        aria-label="上一首"
        disabled={!canGoPrevious}
        onClick={prev}
        className="icon-button text-text-muted hover:text-text disabled:opacity-30"
      >
        <SkipBack size={mode === 'mini' ? 18 : 24} />
      </button>
      <button
        type="button"
        aria-label={playing ? '暂停' : '播放'}
        disabled={!streamUrl || Boolean(playbackError)}
        onClick={() => (playing ? pause() : resume())}
        className={mode === 'mini'
          ? 'icon-button text-primary hover:opacity-80 disabled:opacity-30'
          : 'h-14 w-14 rounded-full bg-primary flex items-center justify-center hover:opacity-90 disabled:opacity-30'}
      >
        {playing ? <Pause size={mode === 'mini' ? 24 : 28} weight="fill" /> : <Play size={mode === 'mini' ? 24 : 28} weight="fill" />}
      </button>
      <button
        type="button"
        aria-label="下一首"
        disabled={!canGoNext}
        onClick={next}
        className="icon-button text-text-muted hover:text-text disabled:opacity-30"
      >
        <SkipForward size={mode === 'mini' ? 18 : 24} />
      </button>
    </div>
  );

  if (mode === 'mini') {
    return (
      <div className="frosted-bar fixed bottom-0 left-0 right-0 z-40 min-h-16 px-3 py-2 sm:px-5 lg:left-60" aria-label="迷你播放器">
        {media}
        <div className="flex min-w-0 items-center gap-3">
          <div className="min-w-0 flex-1 sm:w-40 sm:flex-none">
            <p className="truncate text-xs font-medium">{currentTrack.title}</p>
            <p className="truncate text-[11px] text-text-muted">{currentTrack.artist || '未知艺术家'}</p>
          </div>
          <div className="hidden min-w-0 flex-1 md:flex">{timeline}</div>
          {transport}
          <label className="hidden w-28 shrink-0 items-center gap-2 lg:flex">
            <SpeakerHigh size={16} className="text-text-muted" />
            <span className="sr-only">音量</span>
            <input
              type="range"
              aria-label="音量"
              aria-valuetext={`${Math.round(volume * 100)}%`}
              min={0}
              max={1}
              step={0.05}
              value={volume}
              onChange={(event) => setVolume(Number(event.target.value))}
              className="w-full accent-primary"
            />
          </label>
        </div>
        {playbackError && <p role="alert" className="mt-1 truncate text-xs text-destructive">{playbackError}</p>}
      </div>
    );
  }

  return (
    <section className="glass-card mx-auto mt-4 max-w-lg p-5 sm:p-8" aria-label="音乐播放器">
      {media}
      <div className="mx-auto mb-6 flex aspect-square w-40 items-center justify-center rounded-lg border border-primary/20 bg-primary/10 sm:w-48" aria-hidden="true">
        <div className="h-16 w-16 rounded-full border-2 border-primary/30" />
      </div>
      <h2 className="break-words text-center text-lg font-display">{currentTrack.title}</h2>
      <p className="mb-6 break-words text-center text-sm text-text-muted">{currentTrack.artist || '未知艺术家'}</p>
      {timeline}
      <div className="mt-6 flex items-center justify-center">{transport}</div>
      <label className="mx-auto mt-6 flex max-w-xs items-center gap-3">
        <SpeakerHigh size={18} className="shrink-0 text-text-muted" />
        <span className="sr-only">音量</span>
        <input
          type="range"
          aria-label="音量"
          aria-valuetext={`${Math.round(volume * 100)}%`}
          min={0}
          max={1}
          step={0.05}
          value={volume}
          onChange={(event) => setVolume(Number(event.target.value))}
          className="w-full accent-primary"
        />
      </label>
      {playbackError && <p role="alert" className="mt-5 text-center text-sm text-destructive">{playbackError}</p>}
    </section>
  );
}

function formatTime(value: number): string {
  const safeValue = Number.isFinite(value) ? Math.max(0, value) : 0;
  const minutes = Math.floor(safeValue / 60);
  const seconds = Math.floor(safeValue % 60);
  return `${minutes}:${seconds.toString().padStart(2, '0')}`;
}

function errorMessage(error: unknown, fallback: string): string {
  return error instanceof Error && error.message ? error.message : fallback;
}
