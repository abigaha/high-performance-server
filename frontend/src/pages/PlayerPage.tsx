import { useEffect, useRef, useState } from 'react';
import { useNavigate, useParams } from 'react-router-dom';
import { ArrowLeft } from '@phosphor-icons/react';
import { getMusicDetail } from '../api/music';
import AudioPlayer from '../components/AudioPlayer';
import { usePlayerStore } from '../stores/player';

export default function PlayerPage() {
  const { id } = useParams<{ id: string }>();
  const currentTrack = usePlayerStore((state) => state.currentTrack);
  const hydrateTrack = usePlayerStore((state) => state.hydrateTrack);
  const navigate = useNavigate();
  const requestIdRef = useRef(0);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);

  const musicId = Number(id);
  const hasMatchingTrack = Number.isSafeInteger(musicId)
    && musicId > 0
    && currentTrack?.music_id === musicId;

  useEffect(() => {
    if (!Number.isSafeInteger(musicId) || musicId <= 0) {
      setError('音乐编号无效');
      setLoading(false);
      return;
    }

    const requestId = ++requestIdRef.current;
    setLoading(true);
    setError(null);
    void getMusicDetail(musicId)
      .then((detail) => {
        if (requestId === requestIdRef.current) hydrateTrack(detail);
      })
      .catch((loadError: unknown) => {
        if (requestId === requestIdRef.current) {
          setError(errorMessage(loadError, '音乐详情加载失败，请稍后重试'));
        }
      })
      .finally(() => {
        if (requestId === requestIdRef.current) setLoading(false);
      });

    return () => {
      requestIdRef.current += 1;
    };
  }, [hydrateTrack, musicId]);

  return (
    <div className="min-w-0">
      <button
        type="button"
        onClick={() => navigate('/music/library')}
        className="mb-4 flex min-h-10 items-center gap-2 text-sm text-text-muted transition-colors hover:text-text"
      >
        <ArrowLeft size={18} /> 返回音乐库
      </button>

      {loading && !hasMatchingTrack ? (
        <div role="status" className="text-center text-text-muted py-12">正在加载音乐...</div>
      ) : error && !hasMatchingTrack ? (
        <div role="alert" className="text-center py-12">
          <p className="mb-4 text-sm text-destructive">{error}</p>
          <button type="button" onClick={() => navigate('/music/library')} className="glass-button text-sm">返回音乐库</button>
        </div>
      ) : currentTrack ? (
        <>
          {error && <p role="alert" className="mb-4 text-center text-sm text-destructive">{error}</p>}
          <AudioPlayer mode="fullscreen" />
        </>
      ) : (
        <div className="text-center text-text-muted py-12">未找到可播放的音乐</div>
      )}
    </div>
  );
}

function errorMessage(error: unknown, fallback: string): string {
  return error instanceof Error && error.message ? error.message : fallback;
}
