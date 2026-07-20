import { create } from 'zustand';
import type { AuthUser } from '../types/api';
import * as authApi from '../api/auth';

interface AuthState {
  token: string | null;
  user: AuthUser | null;
  loading: boolean;
  login: (username: string, password: string) => Promise<void>;
  register: (username: string, password: string, email: string) => Promise<void>;
  logout: () => void;
  restore: () => Promise<void>;
}

export const useAuthStore = create<AuthState>((set) => ({
  token: localStorage.getItem('token'),
  user: null,
  loading: false,
  login: async (username, password) => {
    const res = await authApi.login(username, password);
    localStorage.setItem('token', res.token);
    set({ token: res.token, user: { user_id: res.user_id, username, email: '', role: res.role } });
  },
  register: async (username, password, email) => {
    const res = await authApi.register(username, password, email);
    localStorage.setItem('token', res.token);
    set({ token: res.token, user: { user_id: res.user_id, username, email, role: res.role } });
  },
  logout: () => {
    authApi.logout().catch(() => {});
    localStorage.removeItem('token');
    set({ token: null, user: null });
  },
  restore: async () => {
    const token = localStorage.getItem('token');
    if (!token) return;
    set({ loading: true });
    try {
      const user = await authApi.getMe();
      set({ user, loading: false });
    } catch {
      localStorage.removeItem('token');
      set({ token: null, user: null, loading: false });
    }
  },
}));
