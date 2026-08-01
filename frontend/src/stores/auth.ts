import { create } from 'zustand';
import type { AuthUser } from '../types/api';
import * as authApi from '../api/auth';
import {
  captureSessionSnapshot,
  getSessionRevision,
  isSessionSnapshotCurrent,
  markSessionChanged,
} from '../api/client';
import type { SessionSnapshot } from '../api/client';

interface AuthState {
  token: string | null;
  user: AuthUser | null;
  loading: boolean;
  restored: boolean;
  sessionRevision: number;
  setUser: (user: AuthUser) => void;
  login: (username: string, password: string) => Promise<boolean>;
  register: (username: string, password: string, email: string) => Promise<boolean>;
  logout: () => void;
  restore: () => Promise<void>;
  reset: () => void;
}

type SessionClearer = () => void;

const initialToken = localStorage.getItem('token');
let authGeneration = 0;
let restorePromise: Promise<void> | null = null;
let restoreSession: SessionSnapshot | null = null;
let sessionClearer: SessionClearer | null = null;

function invalidateRestore(): void {
  restorePromise = null;
  restoreSession = null;
}

export const useAuthStore = create<AuthState>((set, get) => ({
  token: initialToken,
  user: null,
  loading: initialToken !== null,
  restored: initialToken === null,
  sessionRevision: getSessionRevision(),
  setUser: (user) => set({ user }),
  login: async (username, password) => {
    const generation = ++authGeneration;
    invalidateRestore();
    let response;
    try {
      response = await authApi.login(username, password);
    } catch (error) {
      if (generation !== authGeneration) return false;
      throw error;
    }
    if (generation !== authGeneration) return false;
    localStorage.setItem('token', response.token);
    const sessionRevision = markSessionChanged();
    set({
      token: response.token,
      user: response.user,
      loading: false,
      restored: true,
      sessionRevision,
    });
    return true;
  },
  register: async (username, password, email) => {
    const generation = ++authGeneration;
    invalidateRestore();
    let response;
    try {
      response = await authApi.register(username, password, email);
    } catch (error) {
      if (generation !== authGeneration) return false;
      throw error;
    }
    if (generation !== authGeneration) return false;
    localStorage.setItem('token', response.token);
    const sessionRevision = markSessionChanged();
    set({
      token: response.token,
      user: response.user,
      loading: false,
      restored: true,
      sessionRevision,
    });
    return true;
  },
  logout: () => {
    authApi.logout().catch(() => {});
    (sessionClearer ?? clearAuthSession)();
  },
  restore: async () => {
    const token = localStorage.getItem('token');
    if (!token) {
      invalidateRestore();
      if (get().token || get().user) (sessionClearer ?? clearAuthSession)();
      else {
        set({
          token: null,
          user: null,
          loading: false,
          restored: true,
          sessionRevision: getSessionRevision(),
        });
      }
      return;
    }

    const session = captureSessionSnapshot();
    if (restorePromise && restoreSession && isSessionSnapshotCurrent(restoreSession)) {
      return restorePromise;
    }

    const generation = ++authGeneration;
    invalidateRestore();
    set({ loading: true });
    const pendingRestore = (async () => {
      try {
        const user = await authApi.getMe();
        if (generation === authGeneration && isSessionSnapshotCurrent(session)) {
          set({
            token,
            user,
            loading: false,
            restored: true,
            sessionRevision: getSessionRevision(),
          });
        }
      } catch {
        if (generation === authGeneration && isSessionSnapshotCurrent(session)) {
          (sessionClearer ?? clearAuthSession)();
        }
      }
    })();
    restorePromise = pendingRestore;
    restoreSession = session;
    const clearPendingRestore = () => {
      if (restorePromise === pendingRestore) invalidateRestore();
    };
    void pendingRestore.then(clearPendingRestore, clearPendingRestore);
    return pendingRestore;
  },
  reset: clearAuthSession,
}));

export function injectSessionClearer(clearer: SessionClearer | null): void {
  sessionClearer = clearer;
}

export function clearAuthSession(): void {
  authGeneration += 1;
  invalidateRestore();
  localStorage.removeItem('token');
  const stateRevision = useAuthStore.getState().sessionRevision;
  const clientRevision = getSessionRevision();
  const sessionRevision = stateRevision === clientRevision ? markSessionChanged() : clientRevision;
  useAuthStore.setState({
    token: null,
    user: null,
    loading: false,
    restored: true,
    sessionRevision,
  });
}
