import { useAuthStore } from '../stores/auth';
import { useMusicStore } from '../stores/music';
import { usePlayerStore } from '../stores/player';
import { useToastStore } from '../stores/toast';

export function clearUserSession(): void {
  useAuthStore.getState().reset();
  useMusicStore.getState().reset();
  usePlayerStore.getState().reset();
  useToastStore.getState().reset();
}
