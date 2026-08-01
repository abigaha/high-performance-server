import { useEffect, useRef, useState } from 'react';
import { useNavigate, useParams } from 'react-router-dom';
import { ArrowLeft } from '@phosphor-icons/react';
import { getMusicDetail } from '../api/music';
import { captureSessionSnapshot, isSessionSnapshotCurrent } from '../api/client';
import AudioPlayer from '../components/AudioPlayer';
import {
  capturePlayerGeneration,
  capturePlayerStateRevision,
  isPlayerGenerationCurrent,
  usePlayerStore,
} from '../stores/player';
import { useAuthStore } from '../stores/auth';

export default function PlayerPage() {
  const { id } = useParams<{ id: string }>();
  const currentTrack = usePlayerStore((state) => state.currentTrack);
  const hydrateTrack = usePlayerStore((state) => state.hydrateTrack);
  const playerRevision = usePlayerStore((state) => state.stateRevision);
  const sessionRevision = useAuthStore((state) => state.sessionRevision);
  const navigate = useNavigate();
  const requestIdRef = useRef(0);
  const activeRequestRef = useRef<{
    id: number;
    playerRevision: number;
    sessionRevision: number;
  } | null>(null);
  const observedRevisionsRef = useRef({ playerRevision, sessionRevision });
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
    const session = captureSessionSnapshot();
    const playerGeneration = capturePlayerGeneration();
    const playerStateRevision = capturePlayerStateRevision();
    activeRequestRef.current = {
      id: requestId,
      playerRevision: playerStateRevision,
      sessionRevision: session.revision,
    };
    const isActiveRequest = () => requestId === requestIdRef.current;
    const canApplyResult = () => isActiveRequest()
      && isSessionSnapshotCurrent(session)
      && isPlayerGenerationCurrent(playerGeneration)
      && capturePlayerStateRevision() === playerStateRevision;
    setLoading(true);
    setError(null);
    void getMusicDetail(musicId)
      .then((detail) => {
        if (canApplyResult()) {
          hydrateTrack(detail);
        }
      })
      .catch((loadError: unknown) => {
        if (canApplyResult()) {
          setError(errorMessage(loadError, '音乐详情加载失败，请稍后重试'));
        }
      })
      .finally(() => {
        if (isActiveRequest()) {
          activeRequestRef.current = null;
          setLoading(false);
        }
      });

    return () => {
      if (requestId === requestIdRef.current) {
        requestIdRef.current += 1;
        activeRequestRef.current = null;
      }
    };
  }, [hydrateTrack, musicId]);

  useEffect(() => {
    const previous = observedRevisionsRef.current;
    observedRevisionsRef.current = { playerRevision, sessionRevision };
    if (previous.playerRevision === playerRevision && previous.sessionRevision === sessionRevision) return;

    const request = activeRequestRef.current;
    if (!request || (request.playerRevision === playerRevision && request.sessionRevision === sessionRevision)) return;

    requestIdRef.current += 1;
    activeRequestRef.current = null;
    setLoading(false);
  }, [playerRevision, sessionRevision]);

  return (
    <div className="min-w-0">
      <button
        type="button"
        onClick={() => navigate('/music/library')}
        className="mb-4 flex min-h-11 min-w-11 items-center gap-2 text-sm text-text-muted transition-colors hover:text-text"
      >
        <ArrowLeft size={18} /> 返回音乐库
      </button>

      {loading && !hasMatchingTrack ? (
        <div role="status" className="text-center text-text-muted py-12">正在加载音乐...</div>
      ) : error && !hasMatchingTrack ? (
        <div role="alert" className="text-center py-12">
          <p className="mb-4 text-sm text-destructive">{error}</p>
          <button type="button" onClick={() => navigate('/music/library')} className="glass-button min-h-11 min-w-11 text-sm">返回音乐库</button>
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
