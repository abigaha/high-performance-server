import { useState } from 'react';
import { Link, useNavigate } from 'react-router-dom';
import { useAuthStore } from '../stores/auth';

export default function RegisterPage() {
  const [username, setUsername] = useState('');
  const [password, setPassword] = useState('');
  const [email, setEmail] = useState('');
  const [error, setError] = useState('');
  const register = useAuthStore((s) => s.register);
  const navigate = useNavigate();

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    setError('');
    try {
      await register(username, password, email);
      navigate('/files', { replace: true });
    } catch {
      setError('注册失败，请重试');
    }
  };

  return (
    <form onSubmit={handleSubmit} className="glass-card p-8 w-full max-w-sm flex flex-col gap-5">
      <h1 className="text-xl font-display text-center text-primary">注册</h1>
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
        type="email"
        placeholder="邮箱"
        value={email}
        onChange={(e) => setEmail(e.target.value)}
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
      <button type="submit" className="glass-button w-full">注册</button>
      <p className="text-xs text-text-muted text-center">
        已有账号？ <Link to="/login" className="text-primary hover:underline">登录</Link>
      </p>
    </form>
  );
}
