import { useState } from 'react';
import { Link, useNavigate } from 'react-router-dom';
import { useAuthStore } from '../stores/auth';

export default function RegisterPage() {
  const [username, setUsername] = useState('');
  const [password, setPassword] = useState('');
  const [email, setEmail] = useState('');
  const [error, setError] = useState('');
  const [submitting, setSubmitting] = useState(false);
  const register = useAuthStore((state) => state.register);
  const navigate = useNavigate();

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
      await register(username, password, email);
      navigate('/files', { replace: true });
    } catch (registerError) {
      setError(registerError instanceof Error && registerError.message ? registerError.message : '注册失败，请重试');
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
      <h1 className="text-center font-display text-xl text-primary">注册</h1>
      {error && <p role="alert" className="text-center text-xs text-destructive">{error}</p>}
      <div>
        <label htmlFor="register-username" className="sr-only">用户名</label>
        <input
          id="register-username"
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
        <label htmlFor="register-email" className="sr-only">邮箱</label>
        <input
          id="register-email"
          name="email"
          type="email"
          placeholder="邮箱"
          autoComplete="email"
          value={email}
          onChange={(event) => setEmail(event.target.value)}
          className="glass-input"
          disabled={submitting}
          required
        />
      </div>
      <div>
        <label htmlFor="register-password" className="sr-only">密码</label>
        <input
          id="register-password"
          name="password"
          type="password"
          placeholder="密码"
          autoComplete="new-password"
          value={password}
          onChange={(event) => setPassword(event.target.value)}
          className="glass-input"
          minLength={6}
          disabled={submitting}
          required
        />
      </div>
      <button type="submit" className="glass-button w-full" disabled={submitting}>
        {submitting ? '注册中...' : '注册'}
      </button>
      <p className="text-center text-xs text-text-muted">
        已有账号？ <Link to="/login" className="text-primary hover:underline">登录</Link>
      </p>
    </form>
  );
}
