import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { getSessionRevision } from '../../src/api/client';
import { clearUserSession } from '../../src/session/clearUserSession';
import { createUnauthorizedHandler } from '../../src/session/createUnauthorizedHandler';
import { injectSessionClearer, useAuthStore } from '../../src/stores/auth';
import { useMusicStore } from '../../src/stores/music';
import { usePlayerStore } from '../../src/stores/player';
import { useToastStore } from '../../src/stores/toast';
import type { AuthUser, MusicMeta } from '../../src/types/api';

const currentUser: AuthUser = {
  user_id: 1,
  username: 'session-user',
  email: 'session@example.com',
  role: 'NORMAL',
  vip_status: 'NONE',
  vip_expires_at: null,
  capabilities: ['USE_AUTHENTICATED_FEATURES'],
  created_at: '2026-01-02T03:04:05.000000Z',
};

const currentTrack: MusicMeta = {
  music_id: 3,
  title: 'Session track',
  artist: '',
  album: '',
  genre: '',
  duration_sec: 30,
  file_hash: 'hash',
  file_size: 10,
  content_type: 'audio/mpeg',
};

describe('clearUserSession', () => {
  beforeEach(() => {
    localStorage.clear();
    vi.useFakeTimers();
    useAuthStore.getState().reset();
    useMusicStore.getState().reset();
    usePlayerStore.getState().reset();
    useToastStore.getState().reset();
  });

  afterEach(() => {
    injectSessionClearer(null);
    vi.useRealTimers();
  });

  it('一次清除 token 和四 Store，并保留主题', () => {
    localStorage.setItem('token', 'active-token');
    localStorage.setItem('theme', 'dark');
    useAuthStore.setState({ token: 'active-token', user: currentUser, loading: true, restored: false });
    useMusicStore.setState({
      library: [currentTrack],
      libraryTotal: 1,
      libraryLoading: true,
      libraryError: 'old error',
      currentPlaylist: { id: 8, name: 'Old', items: [currentTrack] },
      userPlaylists: [{ id: 8, name: 'Old', itemCount: 1 }],
    });
    usePlayerStore.getState().play(currentTrack, [{
      track: currentTrack,
      source: { kind: 'PLAYLIST', id: 8 },
    }]);
    useToastStore.getState().error('old toast');
    const revision = getSessionRevision();

    clearUserSession();

    expect(localStorage.getItem('token')).toBeNull();
    expect(localStorage.getItem('theme')).toBe('dark');
    expect(getSessionRevision()).toBe(revision + 1);
    expect(useAuthStore.getState()).toMatchObject({
      token: null,
      user: null,
      loading: false,
      restored: true,
    });
    expect(useMusicStore.getState()).toMatchObject({
      library: [],
      libraryTotal: 0,
      libraryLoading: false,
      libraryError: null,
      currentPlaylist: null,
      userPlaylists: [],
    });
    expect(usePlayerStore.getState()).toMatchObject({ currentTrack: null, queue: [], playing: false });
    expect(useToastStore.getState().messages).toEqual([]);
  });

  it('client 和 auth 不反向 import Store 聚合模块，依赖保持无环', () => {
    const root = resolve(process.cwd(), 'src');
    const clientSource = readFileSync(resolve(root, 'api/client.ts'), 'utf8');
    const authSource = readFileSync(resolve(root, 'stores/auth.ts'), 'utf8');
    const sessionSource = readFileSync(resolve(root, 'session/clearUserSession.ts'), 'utf8');
    const mainSource = readFileSync(resolve(root, 'main.tsx'), 'utf8');

    expect(clientSource).not.toMatch(/from ['"].*(?:stores|clearUserSession)/);
    expect(authSource).not.toMatch(/from ['"].*clearUserSession/);
    expect(sessionSource).toMatch(/from ['"].*stores\/auth/);
    expect(sessionSource).toMatch(/from ['"].*stores\/music/);
    expect(sessionSource).toMatch(/from ['"].*stores\/player/);
    expect(sessionSource).toMatch(/from ['"].*stores\/toast/);
    expect(mainSource).toMatch(/createUnauthorizedHandler\(clearUserSession,/);
  });

  it('reset 后旧 toast 计时器不能删除新会话消息', () => {
    useToastStore.getState().success('old session');
    useToastStore.getState().reset();
    useToastStore.getState().error('new session');

    vi.advanceTimersByTime(3000);

    expect(useToastStore.getState().messages).toEqual([
      expect.objectContaining({ type: 'error', text: 'new session' }),
    ]);
  });

  it('主动 logout 复用集中清理并保留主题', () => {
    injectSessionClearer(clearUserSession);
    localStorage.setItem('token', 'active-token');
    localStorage.setItem('theme', 'light');
    useAuthStore.setState({ token: 'active-token', user: currentUser });
    useMusicStore.setState({ library: [currentTrack], libraryTotal: 1 });
    usePlayerStore.getState().play(currentTrack, [{
      track: currentTrack,
      source: { kind: 'LIBRARY', id: null },
    }]);
    useToastStore.getState().success('old session');

    useAuthStore.getState().logout();

    expect(localStorage.getItem('token')).toBeNull();
    expect(localStorage.getItem('theme')).toBe('light');
    expect(useAuthStore.getState().user).toBeNull();
    expect(useMusicStore.getState().library).toEqual([]);
    expect(usePlayerStore.getState().queue).toEqual([]);
    expect(useToastStore.getState().messages).toEqual([]);
  });

  it('生产 401 handler 清理真实会话一次并导航一次', () => {
    const navigate = vi.fn();
    localStorage.setItem('token', 'expired-token');
    useAuthStore.setState({ token: 'expired-token', user: currentUser });
    useMusicStore.setState({ library: [currentTrack], libraryTotal: 1 });
    usePlayerStore.getState().play(currentTrack, [{
      track: currentTrack,
      source: { kind: 'LIBRARY', id: null },
    }]);
    useToastStore.getState().error('old session');
    const revision = getSessionRevision();
    const handler = createUnauthorizedHandler(clearUserSession, navigate);

    handler();

    expect(getSessionRevision()).toBe(revision + 1);
    expect(localStorage.getItem('token')).toBeNull();
    expect(useAuthStore.getState().user).toBeNull();
    expect(useMusicStore.getState().library).toEqual([]);
    expect(usePlayerStore.getState().queue).toEqual([]);
    expect(useToastStore.getState().messages).toEqual([]);
    expect(navigate).toHaveBeenCalledTimes(1);
  });
});
