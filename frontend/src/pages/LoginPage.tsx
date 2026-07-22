import { useState } from 'react';
import { Link, useNavigate, useSearchParams } from 'react-router-dom';
import { useAuthStore } from '../stores/auth';

export default function LoginPage() {
  const [username, setUsername] = useState('');
  const [password, setPassword] = useState('');
  const [error, setError] = useState('');
  const [submitting, setSubmitting] = useState(false);
  const login = useAuthStore((state) => state.login);
  const navigate = useNavigate();
  const [searchParams] = useSearchParams();
  const redirect = searchParams.get('redirect') || '/files';

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
    try {
      await login(username, password);
      navigate(redirect, { replace: true });
    } catch (loginError) {
      setError(
        loginError instanceof Error && loginError.message
          ? loginError.message
          : '登录失败，请检查用户名和密码',
      );
    } finally {
      setSubmitting(false);
    }
  };

  return (
    <form
      noValidate
      onSubmit={handleSubmit}
      className="glass-card flex w-full max-w-sm flex-col gap-5 p-8"
      aria-busy={submitting}
    >
      <h1 className="text-center font-display text-xl text-primary">登录</h1>
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
