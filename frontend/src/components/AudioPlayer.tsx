import { useEffect, useRef, useState } from 'react';
import { Play, Pause, SkipBack, SkipForward, SpeakerHigh } from '@phosphor-icons/react';
import { usePlayerStore } from '../stores/player';
import { getFileStreamUrl } from '../api/files';

interface Props {
  mode: 'mini' | 'fullscreen';
}

export default function AudioPlayer({ mode }: Props) {
  const audioRef = useRef<HTMLAudioElement>(null);
  const [streamUrl, setStreamUrl] = useState('');
  const {
    currentTrack, playing, currentTime, duration, volume,
    pause, resume, seek, next, prev, setVolume, setCurrentTime, setDuration,
  } = usePlayerStore();

  useEffect(() => {
    if (currentTrack) {
      getFileStreamUrl(currentTrack.music_id).then(setStreamUrl);
    } else {
      setStreamUrl('');
    }
  }, [currentTrack]);

  useEffect(() => {
    const audio = audioRef.current;
    if (!audio || !currentTrack) return;

    if (playing) {
      audio.play().catch(() => {});
    } else {
      audio.pause();
    }
  }, [playing, currentTrack]);

  useEffect(() => {
    const audio = audioRef.current;
    if (!audio) return;

    const onTimeUpdate = () => setCurrentTime(audio.currentTime);
    const onDurationChange = () => setDuration(audio.duration || 0);
    const onEnded = () => next();

    audio.addEventListener('timeupdate', onTimeUpdate);
    audio.addEventListener('durationchange', onDurationChange);
    audio.addEventListener('ended', onEnded);

    return () => {
      audio.removeEventListener('timeupdate', onTimeUpdate);
      audio.removeEventListener('durationchange', onDurationChange);
      audio.removeEventListener('ended', onEnded);
    };
  }, [currentTrack, next, setCurrentTime, setDuration]);

  useEffect(() => {
    if (audioRef.current) {
      audioRef.current.volume = volume;
    }
  }, [volume]);

  if (!currentTrack) return null;

  const fmt = (t: number) => {
    const m = Math.floor(t / 60);
    const s = Math.floor(t % 60);
    return `${m}:${s.toString().padStart(2, '0')}`;
  };

  const handleSeek = (e: React.MouseEvent<HTMLDivElement>) => {
    const rect = e.currentTarget.getBoundingClientRect();
    const pct = (e.clientX - rect.left) / rect.width;
    const audio = audioRef.current;
    if (audio) {
      const t = pct * (audio.duration || 0);
      audio.currentTime = t;
      seek(t);
    }
  };

  const bar = (
    <div className="flex items-center gap-3 flex-1">
      <div className="text-xs text-text-muted w-10 text-right">{fmt(currentTime)}</div>
      <div className="flex-1 h-1.5 bg-white/20 rounded-full cursor-pointer relative" onClick={handleSeek}>
        <div
          className="absolute left-0 top-0 h-full bg-primary rounded-full"
          style={{ width: `${duration > 0 ? (currentTime / duration) * 100 : 0}%` }}
        />
      </div>
      <div className="text-xs text-text-muted w-10">{fmt(currentTime > 0 ? duration : currentTrack.duration_sec)}</div>
    </div>
  );

  if (mode === 'mini') {
    return (
      <>
        <audio ref={audioRef} src={streamUrl} preload="metadata" />
        <div className="frosted-bar fixed bottom-0 left-60 right-0 h-16 flex items-center px-6 gap-4 z-40">
          <div className="flex items-center gap-3 min-w-0 w-48">
            <div className="w-8 h-8 rounded-lg bg-primary/20 flex items-center justify-center shrink-0">
              <div className="w-4 h-4 rounded bg-primary/40" />
            </div>
            <div className="min-w-0">
              <p className="text-xs font-medium truncate">{currentTrack.title}</p>
              <p className="text-[10px] text-text-muted truncate">{currentTrack.artist}</p>
            </div>
          </div>
          {bar}
          <div className="flex items-center gap-3">
            <button onClick={prev} className="text-text-muted hover:text-text transition-colors">
              <SkipBack size={18} />
            </button>
            <button onClick={() => (playing ? pause() : resume())} className="text-primary hover:opacity-80 transition-opacity">
              {playing ? <Pause size={24} weight="fill" /> : <Play size={24} weight="fill" />}
            </button>
            <button onClick={next} className="text-text-muted hover:text-text transition-colors">
              <SkipForward size={18} />
            </button>
          </div>
          <div className="flex items-center gap-2 w-24">
            <SpeakerHigh size={16} className="text-text-muted" />
            <input
              type="range"
              min={0}
              max={1}
              step={0.05}
              value={volume}
              onChange={(e) => setVolume(Number(e.target.value))}
              className="w-full accent-primary"
            />
          </div>
        </div>
      </>
    );
  }

  return (
    <div className="glass-card p-8 max-w-lg mx-auto mt-8">
      <audio ref={audioRef} src={streamUrl} preload="metadata" />
      <div className="w-48 h-48 mx-auto rounded-2xl bg-gradient-to-br from-primary/40 to-primary/10 flex items-center justify-center mb-6">
        <div className="w-16 h-16 rounded-full bg-primary/30" />
      </div>
      <h2 className="text-lg font-display text-center">{currentTrack.title}</h2>
      <p className="text-sm text-text-muted text-center mb-6">{currentTrack.artist}</p>
      {bar}
      <div className="flex items-center justify-center gap-6 mt-6">
        <button onClick={prev} className="text-text-muted hover:text-text">
          <SkipBack size={24} />
        </button>
        <button
          onClick={() => (playing ? pause() : resume())}
          className="w-14 h-14 rounded-full bg-primary flex items-center justify-center hover:opacity-90 transition-opacity"
        >
          {playing ? <Pause size={28} weight="fill" /> : <Play size={28} weight="fill" />}
        </button>
        <button onClick={next} className="text-text-muted hover:text-text">
          <SkipForward size={24} />
        </button>
      </div>
    </div>
  );
}
