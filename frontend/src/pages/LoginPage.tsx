import { useState } from 'react';
import { Link, useNavigate, useSearchParams } from 'react-router-dom';
import { useAuthStore } from '../stores/auth';

export default function LoginPage() {
  const [username, setUsername] = useState('');
  const [password, setPassword] = useState('');
  const [error, setError] = useState('');
  const login = useAuthStore((s) => s.login);
  const navigate = useNavigate();
  const [searchParams] = useSearchParams();
  const redirect = searchParams.get('redirect') || '/files';

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    setError('');
    try {
      await login(username, password);
      navigate(redirect, { replace: true });
    } catch {
      setError('登录失败，请检查用户名和密码');
    }
  };

  return (
    <form onSubmit={handleSubmit} className="glass-card p-8 w-full max-w-sm flex flex-col gap-5">
      <h1 className="text-xl font-display text-center text-primary">登录</h1>
      {error && <p className="text-xs text-destructive text-center">{error}</p>}
      <input
        type="text"
        placeholder="用户名"
        value={username}
        onChange={(e) => setUsername(e.target.value)}
        className="glass-input"
        required
      />
      <input
        type="password"
        placeholder="密码"
        value={password}
        onChange={(e) => setPassword(e.target.value)}
        className="glass-input"
        required
      />
      <button type="submit" className="glass-button w-full">登录</button>
      <p className="text-xs text-text-muted text-center">
        没有账号？ <Link to="/register" className="text-primary hover:underline">注册</Link>
      </p>
    </form>
  );
}
