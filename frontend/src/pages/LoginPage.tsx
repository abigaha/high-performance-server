import { useEffect, useRef, useState } from 'react';
import { Link, useLocation, useNavigate } from 'react-router-dom';
import { useAuthStore } from '../stores/auth';

interface RedirectState {
  from?: {
    pathname?: unknown;
    search?: unknown;
    hash?: unknown;
  };
}

function getSafeInternalRedirect(state: unknown): string {
  const from = (state as RedirectState | null)?.from;
  if (!from || typeof from.pathname !== 'string' || !from.pathname.startsWith('/')) return '/files';

  const search = typeof from.search === 'string' && from.search.startsWith('?') ? from.search : '';
  const hash = typeof from.hash === 'string' && from.hash.startsWith('#') ? from.hash : '';
  try {
    const target = new URL(`${from.pathname}${search}${hash}`, window.location.origin);
    return target.origin === window.location.origin ? `${target.pathname}${target.search}${target.hash}` : '/files';
  } catch {
    return '/files';
  }
}

export default function LoginPage() {
  const [username, setUsername] = useState('');
  const [password, setPassword] = useState('');
  const [error, setError] = useState('');
  const [submitting, setSubmitting] = useState(false);
  const login = useAuthStore((state) => state.login);
  const navigate = useNavigate();
  const location = useLocation();
  const redirect = getSafeInternalRedirect(location.state);
  const operationIdRef = useRef(0);
  const mountedRef = useRef(true);

  useEffect(() => {
    mountedRef.current = true;
    return () => {
      mountedRef.current = false;
      operationIdRef.current += 1;
    };
  }, []);

  const handleSubmit = async (event: React.FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    if (submitting) return;

    setError('');
    if (username.length < 2) {
      setError('用户名至少 2 个字符');
      return;
    }
    if (password.length < 6) {
      setError('密码至少 6 个字符');
      return;
    }
    if (!event.currentTarget.checkValidity()) {
      event.currentTarget.reportValidity();
      return;
    }

    setSubmitting(true);
    const operationId = ++operationIdRef.current;
    const isCurrent = () => mountedRef.current && operationId === operationIdRef.current;
    try {
      const committed = await login(username, password);
      if (committed !== false && isCurrent()) navigate(redirect, { replace: true });
    } catch (loginError) {
      if (isCurrent()) setError(
        loginError instanceof Error && loginError.message
          ? loginError.message
          : '登录失败，请检查用户名和密码',
      );
    } finally {
      if (isCurrent()) setSubmitting(false);
    }
  };

  return (
    <form
      noValidate
      onSubmit={handleSubmit}
      className="guest-form-surface glass-card flex w-full max-w-md flex-col gap-5 p-5 sm:p-7"
      aria-busy={submitting}
      aria-labelledby="login-title"
    >
      <h1 id="login-title" className="text-center font-display text-2xl text-text">登录</h1>
      {error && <p role="alert" className="text-center text-xs text-destructive">{error}</p>}
      <div>
        <label htmlFor="login-username" className="sr-only">用户名</label>
        <input
          id="login-username"
          name="username"
          type="text"
          placeholder="用户名"
          autoComplete="username"
          value={username}
          onChange={(event) => setUsername(event.target.value)}
          className="glass-input"
          minLength={2}
          disabled={submitting}
          required
        />
      </div>
      <div>
        <label htmlFor="login-password" className="sr-only">密码</label>
        <input
          id="login-password"
          name="password"
          type="password"
          placeholder="密码"
          autoComplete="current-password"
          value={password}
          onChange={(event) => setPassword(event.target.value)}
          className="glass-input"
          minLength={6}
          disabled={submitting}
          required
        />
      </div>
      <button type="submit" className="glass-button w-full" disabled={submitting}>
        {submitting ? '登录中...' : '登录'}
      </button>
      <p className="text-center text-xs text-text-muted">
        没有账号？ <Link to="/register" className="text-primary hover:underline">注册</Link>
      </p>
    </form>
  );
}
