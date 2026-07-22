import { create } from 'zustand';
import type { AuthUser } from '../types/api';
import * as authApi from '../api/auth';

interface AuthState {
  token: string | null;
  user: AuthUser | null;
  loading: boolean;
  restored: boolean;
  login: (username: string, password: string) => Promise<void>;
  register: (username: string, password: string, email: string) => Promise<void>;
  logout: () => void;
  restore: () => Promise<void>;
}

const initialToken = localStorage.getItem('token');
let restorePromise: Promise<void> | null = null;

export const useAuthStore = create<AuthState>((set) => ({
  token: initialToken,
  user: null,
  loading: initialToken !== null,
  restored: initialToken === null,
  login: async (username, password) => {
    const res = await authApi.login(username, password);
    localStorage.setItem('token', res.token);
    set({
      token: res.token,
      user: { user_id: res.user_id, username, email: '', role: res.role },
      loading: false,
      restored: true,
    });
  },
  register: async (username, password, email) => {
    const res = await authApi.register(username, password, email);
    localStorage.setItem('token', res.token);
    set({
      token: res.token,
      user: { user_id: res.user_id, username, email, role: res.role },
      loading: false,
      restored: true,
    });
  },
  logout: () => {
    authApi.logout().catch(() => {});
    localStorage.removeItem('token');
    set({ token: null, user: null, loading: false, restored: true });
  },
  restore: async () => {
    const token = localStorage.getItem('token');
    if (!token) {
      set({ token: null, user: null, loading: false, restored: true });
      return;
    }
    if (restorePromise) return restorePromise;

    set({ loading: true });
    restorePromise = (async () => {
      try {
        const user = await authApi.getMe();
        if (localStorage.getItem('token') === token) {
          set({ token, user, loading: false, restored: true });
        }
      } catch {
        if (localStorage.getItem('token') === token) {
          localStorage.removeItem('token');
          set({ token: null, user: null, loading: false, restored: true });
        }
      } finally {
        restorePromise = null;
      }
    })();
    return restorePromise;
  },
}));
