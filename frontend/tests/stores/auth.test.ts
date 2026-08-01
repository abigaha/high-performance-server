import { beforeEach, describe, expect, it, vi } from 'vitest';
import type { AuthResponse, AuthUser } from '../../src/types/api';

const mocks = vi.hoisted(() => ({
  login: vi.fn(),
  register: vi.fn(),
  logout: vi.fn(),
  getMe: vi.fn(),
}));

vi.mock('../../src/api/auth', () => mocks);

import { captureSessionSnapshot, markSessionChanged } from '../../src/api/client';
import {
  clearAuthSession,
  injectSessionClearer,
  useAuthStore,
} from '../../src/stores/auth';

interface Deferred<T> {
  promise: Promise<T>;
  resolve: (value: T) => void;
  reject: (reason: unknown) => void;
}

function deferred<T>(): Deferred<T> {
  let resolvePromise!: (value: T) => void;
  let rejectPromise!: (reason: unknown) => void;
  const promise = new Promise<T>((resolve, reject) => {
    resolvePromise = resolve;
    rejectPromise = reject;
  });
  return { promise, resolve: resolvePromise, reject: rejectPromise };
}

function user(userId: number, username: string): AuthUser {
  return {
    user_id: userId,
    username,
    email: `${username}@example.com`,
    role: 'NORMAL',
    vip_status: 'NONE',
    vip_expires_at: null,
    capabilities: ['USE_AUTHENTICATED_FEATURES'],
    created_at: '2026-01-02T03:04:05.000000Z',
  };
}

describe('auth store', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    localStorage.clear();
    mocks.logout.mockResolvedValue(undefined);
    useAuthStore.setState({
      token: null,
      user: null,
      loading: false,
      restored: true,
      sessionRevision: captureSessionSnapshot().revision,
    });
    injectSessionClearer(null);
  });

  it('镜像 client session revision，并在同 token 新会话时递增', async () => {
    const before = useAuthStore.getState().sessionRevision;
    mocks.login.mockResolvedValueOnce({ token: 'same-token', user: user(2, 'new-user') });

    await useAuthStore.getState().login('new-user', 'password');

    const after = useAuthStore.getState().sessionRevision;
    expect(after).toBeGreaterThan(before);
    expect(after).toBe(captureSessionSnapshot().revision);
  });

  it('logout 递增镜像，401 已推进 client revision 时只同步一次', () => {
    localStorage.setItem('token', 'active-token');
    useAuthStore.setState({ token: 'active-token' });
    const beforeLogout = useAuthStore.getState().sessionRevision;

    useAuthStore.getState().logout();

    expect(useAuthStore.getState().sessionRevision).toBeGreaterThan(beforeLogout);
    expect(useAuthStore.getState().sessionRevision).toBe(captureSessionSnapshot().revision);

    const unauthorizedRevision = markSessionChanged();
    clearAuthSession();
    expect(useAuthStore.getState().sessionRevision).toBe(unauthorizedRevision);
    expect(captureSessionSnapshot().revision).toBe(unauthorizedRevision);
  });

  it('restore 的非 401 失败清理会话时推进 revision 镜像', async () => {
    localStorage.setItem('token', 'network-failure-token');
    useAuthStore.setState({ token: 'network-failure-token', restored: false });
    mocks.getMe.mockRejectedValueOnce(new Error('network failed'));
    const before = useAuthStore.getState().sessionRevision;

    await useAuthStore.getState().restore();

    expect(useAuthStore.getState().token).toBeNull();
    expect(useAuthStore.getState().sessionRevision).toBeGreaterThan(before);
    expect(useAuthStore.getState().sessionRevision).toBe(captureSessionSnapshot().revision);
  });

  it('主动 logout 复用注入的集中清理函数', () => {
    const clearSession = vi.fn();
    injectSessionClearer(clearSession);

    useAuthStore.getState().logout();

    expect(clearSession).toHaveBeenCalledTimes(1);
    expect(mocks.logout).toHaveBeenCalledTimes(1);
  });

  it('reset 后旧 login Promise 不能覆盖新会话', async () => {
    const oldLogin = deferred<AuthResponse>();
    mocks.login.mockReturnValueOnce(oldLogin.promise);
    const loggingIn = useAuthStore.getState().login('old-user', 'password');

    useAuthStore.getState().reset();
    const replacement = user(9, 'replacement');
    localStorage.setItem('token', 'new-token');
    useAuthStore.setState({ token: 'new-token', user: replacement });
    oldLogin.resolve({ token: 'old-token', user: user(1, 'old-user') });
    await loggingIn;

    expect(localStorage.getItem('token')).toBe('new-token');
    expect(useAuthStore.getState().user).toEqual(replacement);
  });

  it('旧 restore 成功不能覆盖 logout 后复用同值 token 的新会话', async () => {
    const oldRestore = deferred<AuthUser>();
    mocks.getMe.mockReturnValueOnce(oldRestore.promise);
    localStorage.setItem('token', 'same-token');
    useAuthStore.setState({ token: 'same-token', user: null, loading: false, restored: false });
    const restoring = useAuthStore.getState().restore();

    useAuthStore.getState().logout();
    const newResponse: AuthResponse = { token: 'same-token', user: user(2, 'new-user') };
    mocks.login.mockResolvedValueOnce(newResponse);
    await useAuthStore.getState().login('new-user', 'password');
    oldRestore.resolve(user(1, 'old-user'));
    await restoring;

    expect(useAuthStore.getState().user).toEqual(newResponse.user);
    expect(localStorage.getItem('token')).toBe('same-token');
  });

  it('旧 restore 失败不能清除 logout 后复用同值 token 的新会话', async () => {
    const oldRestore = deferred<AuthUser>();
    mocks.getMe.mockReturnValueOnce(oldRestore.promise);
    localStorage.setItem('token', 'same-token');
    useAuthStore.setState({ token: 'same-token', user: null, loading: false, restored: false });
    const restoring = useAuthStore.getState().restore();

    useAuthStore.getState().logout();
    const newResponse: AuthResponse = { token: 'same-token', user: user(3, 'replacement') };
    mocks.login.mockResolvedValueOnce(newResponse);
    await useAuthStore.getState().login('replacement', 'password');
    oldRestore.reject(new Error('旧会话失败'));
    await restoring;

    expect(useAuthStore.getState().user).toEqual(newResponse.user);
    expect(localStorage.getItem('token')).toBe('same-token');
  });

  it('登录后立即注册时，旧登录成功不覆盖后发注册会话', async () => {
    const oldLogin = deferred<AuthResponse>();
    const registration = deferred<AuthResponse>();
    const registrationResponse: AuthResponse = { token: 'register-token', user: user(2, 'registered') };
    mocks.login.mockReturnValueOnce(oldLogin.promise);
    mocks.register.mockReturnValueOnce(registration.promise);
    window.history.replaceState({}, '', '/register');

    const loggingIn = useAuthStore.getState().login('old-login', 'password');
    const registering = useAuthStore.getState().register('registered', 'password', 'registered@example.com');
    registration.resolve(registrationResponse);
    await expect(registering).resolves.toBe(true);
    const currentRevision = captureSessionSnapshot().revision;

    oldLogin.resolve({ token: 'login-token', user: user(1, 'old-login') });

    await expect(loggingIn).resolves.toBe(false);
    expect(localStorage.getItem('token')).toBe('register-token');
    expect(useAuthStore.getState().user).toEqual(registrationResponse.user);
    expect(useAuthStore.getState().sessionRevision).toBe(currentRevision);
    expect(window.location.pathname).toBe('/register');
  });

  it('登录后立即注册时，旧登录失败不影响后发注册会话', async () => {
    const oldLogin = deferred<AuthResponse>();
    const registration = deferred<AuthResponse>();
    const registrationResponse: AuthResponse = { token: 'register-token', user: user(2, 'registered') };
    mocks.login.mockReturnValueOnce(oldLogin.promise);
    mocks.register.mockReturnValueOnce(registration.promise);
    window.history.replaceState({}, '', '/register');

    const loggingIn = useAuthStore.getState().login('old-login', 'password');
    const registering = useAuthStore.getState().register('registered', 'password', 'registered@example.com');
    registration.resolve(registrationResponse);
    await expect(registering).resolves.toBe(true);
    const currentRevision = captureSessionSnapshot().revision;

    oldLogin.reject(new Error('旧登录失败'));

    await expect(loggingIn).resolves.toBe(false);
    expect(localStorage.getItem('token')).toBe('register-token');
    expect(useAuthStore.getState().user).toEqual(registrationResponse.user);
    expect(useAuthStore.getState().sessionRevision).toBe(currentRevision);
    expect(window.location.pathname).toBe('/register');
  });

  it('注册后立即登录时，旧注册成功不覆盖后发登录会话', async () => {
    const oldRegistration = deferred<AuthResponse>();
    const login = deferred<AuthResponse>();
    const loginResponse: AuthResponse = { token: 'login-token', user: user(2, 'logged-in') };
    mocks.register.mockReturnValueOnce(oldRegistration.promise);
    mocks.login.mockReturnValueOnce(login.promise);
    window.history.replaceState({}, '', '/login');

    const registering = useAuthStore.getState().register('old-register', 'password', 'old-register@example.com');
    const loggingIn = useAuthStore.getState().login('logged-in', 'password');
    login.resolve(loginResponse);
    await expect(loggingIn).resolves.toBe(true);
    const currentRevision = captureSessionSnapshot().revision;

    oldRegistration.resolve({ token: 'register-token', user: user(1, 'old-register') });

    await expect(registering).resolves.toBe(false);
    expect(localStorage.getItem('token')).toBe('login-token');
    expect(useAuthStore.getState().user).toEqual(loginResponse.user);
    expect(useAuthStore.getState().sessionRevision).toBe(currentRevision);
    expect(window.location.pathname).toBe('/login');
  });

  it('后发 restore 会使先发 login 完全惰性', async () => {
    const oldLogin = deferred<AuthResponse>();
    const restoredUser = deferred<AuthUser>();
    mocks.login.mockReturnValueOnce(oldLogin.promise);
    mocks.getMe.mockReturnValueOnce(restoredUser.promise);
    localStorage.setItem('token', 'restored-token');
    useAuthStore.setState({ token: 'restored-token', user: null, loading: false, restored: false });

    const loggingIn = useAuthStore.getState().login('old-login', 'password');
    const restoring = useAuthStore.getState().restore();
    oldLogin.resolve({ token: 'login-token', user: user(1, 'old-login') });

    const loginResult = await loggingIn;
    restoredUser.resolve(user(2, 'restored'));
    await restoring;

    expect(loginResult).toBe(false);
    expect(localStorage.getItem('token')).toBe('restored-token');
    expect(useAuthStore.getState().user).toEqual(user(2, 'restored'));
  });

  it('旧 restore 的 finally 不会释放后发恢复或回退当前会话', async () => {
    const oldRestore = deferred<AuthUser>();
    const replacementRestore = deferred<AuthUser>();
    mocks.getMe.mockReturnValueOnce(oldRestore.promise).mockReturnValueOnce(replacementRestore.promise);
    localStorage.setItem('token', 'same-token');
    useAuthStore.setState({ token: 'same-token', user: null, loading: false, restored: false });
    window.history.replaceState({}, '', '/restore');

    const firstRestoring = useAuthStore.getState().restore();
    useAuthStore.getState().logout();
    localStorage.setItem('token', 'same-token');
    useAuthStore.setState({ token: 'same-token', user: null, loading: false, restored: false });
    const secondRestoring = useAuthStore.getState().restore();
    oldRestore.resolve(user(1, 'old-restore'));
    await firstRestoring;

    const repeatedRestore = useAuthStore.getState().restore();

    expect(mocks.getMe).toHaveBeenCalledTimes(2);
    expect(localStorage.getItem('token')).toBe('same-token');
    expect(useAuthStore.getState().user).toBeNull();
    expect(window.location.pathname).toBe('/restore');

    replacementRestore.resolve(user(2, 'replacement'));
    await Promise.all([secondRestoring, repeatedRestore]);

    expect(useAuthStore.getState().user).toEqual(user(2, 'replacement'));
  });

  it.each(['login', 'register'] as const)(
    '后发 %s 失败后不会遗留已失效的 restorePromise',
    async (operationName) => {
      const staleRestore = deferred<AuthUser>();
      const recoveredRestore = deferred<AuthUser>();
      mocks.getMe.mockReturnValueOnce(staleRestore.promise).mockReturnValueOnce(recoveredRestore.promise);
      localStorage.setItem('token', 'same-token');
      useAuthStore.setState({ token: 'same-token', user: null, loading: false, restored: false });

      const initialRestore = useAuthStore.getState().restore();
      if (operationName === 'login') {
        mocks.login.mockRejectedValueOnce(new Error('后发认证失败'));
        await expect(useAuthStore.getState().login('later-login', 'password')).rejects.toThrow('后发认证失败');
      } else {
        mocks.register.mockRejectedValueOnce(new Error('后发认证失败'));
        await expect(
          useAuthStore.getState().register('later-register', 'password', 'later@example.com'),
        ).rejects.toThrow('后发认证失败');
      }

      staleRestore.resolve(user(1, 'stale'));
      await initialRestore;

      const retryRestore = useAuthStore.getState().restore();
      expect(mocks.getMe).toHaveBeenCalledTimes(2);

      const recoveredUser = user(2, 'recovered');
      recoveredRestore.resolve(recoveredUser);
      await retryRestore;

      expect(useAuthStore.getState()).toMatchObject({
        token: 'same-token',
        user: recoveredUser,
        loading: false,
        restored: true,
      });
    },
  );

  it('session revision 变化后的后发 restore 取代旧请求且旧请求保持惰性', async () => {
    const staleRestore = deferred<AuthUser>();
    const currentRestore = deferred<AuthUser>();
    mocks.getMe.mockReturnValueOnce(staleRestore.promise).mockReturnValueOnce(currentRestore.promise);
    localStorage.setItem('token', 'same-token');
    useAuthStore.setState({ token: 'same-token', user: null, loading: false, restored: false });

    const firstRestore = useAuthStore.getState().restore();
    markSessionChanged();
    const secondRestore = useAuthStore.getState().restore();
    expect(mocks.getMe).toHaveBeenCalledTimes(2);

    staleRestore.resolve(user(1, 'stale'));
    await firstRestore;
    const recoveredUser = user(2, 'current');
    currentRestore.resolve(recoveredUser);
    await secondRestore;

    expect(useAuthStore.getState()).toMatchObject({
      token: 'same-token',
      user: recoveredUser,
      loading: false,
      restored: true,
    });
  });

  it('后发 login 成功后，先发 login 的成功不能覆盖新会话', async () => {
    const first = deferred<AuthResponse>();
    const second = deferred<AuthResponse>();
    mocks.login.mockReturnValueOnce(first.promise).mockReturnValueOnce(second.promise);

    const firstLogin = useAuthStore.getState().login('first', 'password');
    const secondLogin = useAuthStore.getState().login('second', 'password');
    second.resolve({ token: 'second-token', user: user(2, 'second') });
    await secondLogin;
    first.resolve({ token: 'first-token', user: user(1, 'first') });
    await firstLogin;

    expect(localStorage.getItem('token')).toBe('second-token');
    expect(useAuthStore.getState().user).toEqual(user(2, 'second'));
  });
});
