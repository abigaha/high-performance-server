import AudioPlayer from '../components/AudioPlayer';
import { usePlayerStore } from '../stores/player';
import { useNavigate } from 'react-router-dom';
import { ArrowLeft } from '@phosphor-icons/react';

export default function PlayerPage() {
  const currentTrack = usePlayerStore((s) => s.currentTrack);
  const navigate = useNavigate();

  if (!currentTrack) {
    return (
      <div className="text-center text-text-muted py-12">
        <p>未选择播放曲目</p>
        <button onClick={() => navigate('/music/library')} className="glass-button mt-4">前往音乐库</button>
      </div>
    );
  }

  return (
    <div>
      <button
        onClick={() => navigate(-1)}
        className="flex items-center gap-2 text-sm text-text-muted hover:text-text mb-4 transition-colors"
      >
        <ArrowLeft size={18} /> 返回
      </button>

      <AudioPlayer mode="fullscreen" />
    </div>
  );
}
