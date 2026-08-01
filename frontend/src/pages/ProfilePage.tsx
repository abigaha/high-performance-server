import { useCallback, useEffect, useRef, useState } from 'react';
import { Link } from 'react-router-dom';
import { UserCircle } from '@phosphor-icons/react';
import { getUser, updateUser } from '../api/users';
import { captureSessionSnapshot, isSessionSnapshotCurrent } from '../api/client';
import { useAuthStore } from '../stores/auth';
import type { AuthUser } from '../types/api';

type LoadStatus = 'loading' | 'ready' | 'error';

export default function ProfilePage() {
  const authUser = useAuthStore((state) => state.user);
  const sessionRevision = useAuthStore((state) => state.sessionRevision);
  const setUser = useAuthStore((state) => state.setUser);
  const [profile, setProfile] = useState<AuthUser | null>(null);
  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [saving, setSaving] = useState(false);
  const [loadStatus, setLoadStatus] = useState<LoadStatus>('loading');
  const [error, setError] = useState<string | null>(null);
  const operationRef = useRef(0);
  const authUserId = authUser?.user_id;

  const load = useCallback(async () => {
    if (authUserId === undefined) return;
    const operation = ++operationRef.current;
    const session = captureSessionSnapshot();
    setLoadStatus('loading');
    setError(null);
    try {
      const value = await getUser(authUserId);
      if (operation !== operationRef.current || !isSessionSnapshotCurrent(session)) return;
      setProfile(value);
      setEmail(value.email);
      setLoadStatus('ready');
    } catch (reason) {
      if (operation !== operationRef.current || !isSessionSnapshotCurrent(session)) return;
      setError(reason instanceof Error ? reason.message : '资料加载失败');
      setLoadStatus('error');
    }
  }, [authUserId]);

  useEffect(() => {
    operationRef.current += 1;
    setSaving(false);
    setError(null);
    setProfile(null);
    setEmail('');
    setPassword('');
    void load();
    return () => { operationRef.current += 1; };
  }, [load, sessionRevision]);

  const save = async (event: React.FormEvent) => {
    event.preventDefault();
    if (!profile || saving) return;
    const operation = ++operationRef.current;
    const session = captureSessionSnapshot();
    setSaving(true);
    setError(null);
    try {
      const data: { email: string; password?: string } = { email };
      if (password) data.password = password;
      const updated = await updateUser(profile.user_id, data);
      if (operation !== operationRef.current || !isSessionSnapshotCurrent(session)) return;
      setProfile(updated);
      setUser(updated);
      setPassword('');
    } catch (reason) {
      if (operation === operationRef.current && isSessionSnapshotCurrent(session)) {
        setError(reason instanceof Error ? reason.message : '资料保存失败');
      }
    } finally {
      if (operation === operationRef.current && isSessionSnapshotCurrent(session)) setSaving(false);
    }
  };

  return <div className="mx-auto max-w-3xl">
    <header className="mb-6 flex items-center gap-3"><UserCircle size={28} className="text-primary" /><h1 className="font-display text-xl text-primary">个人资料</h1></header>
    {loadStatus === 'loading' && <p role="status" className="py-12 text-center text-text-muted">正在加载个人资料...</p>}
    {loadStatus === 'error' && <div className="py-8 text-center"><p role="alert" className="text-sm text-destructive">{error}</p><button type="button" className="glass-button mt-4 min-h-11 text-sm" onClick={() => void load()}>重试个人资料</button></div>}
    {loadStatus === 'ready' && profile && <>
      {error && <p role="alert" className="mb-4 text-sm text-destructive">{error}</p>}
      <section className="mb-6 grid gap-4 border-y border-[var(--surface-border)] py-5 sm:grid-cols-2">
        <Fact label="用户名" value={profile.username} />
        <Fact label="有效角色" value={roleText(profile.role)} />
        <Fact label="会员状态" value={vipText(profile.vip_status)} />
        <div><p className="text-xs text-text-muted">创建时间</p><time dateTime={profile.created_at} className="mt-1 block text-sm">{utc(profile.created_at)}</time></div>
        {profile.vip_expires_at && <div><p className="text-xs text-text-muted">会员到期</p><time dateTime={profile.vip_expires_at} className="mt-1 block text-sm">{utc(profile.vip_expires_at)}</time></div>}
        {(profile.role === 'NORMAL' || profile.role === 'VIP') && <div className="flex items-end"><Link to="/vip" className="text-sm font-medium text-primary underline">前往会员中心</Link></div>}
      </section>
      <form onSubmit={save} className="grid gap-4" aria-busy={saving}>
        <label className="grid gap-1 text-sm">邮箱<input className="glass-input min-h-11" type="email" value={email} onChange={(event) => setEmail(event.target.value)} required /></label>
        <label className="grid gap-1 text-sm">新密码<input className="glass-input min-h-11" type="password" value={password} onChange={(event) => setPassword(event.target.value)} autoComplete="new-password" /></label>
        <button className="glass-button min-h-11 justify-self-start text-sm" type="submit" disabled={saving}>{saving ? '保存中' : '保存资料'}</button>
      </form>
    </>}
  </div>;
}

function Fact({ label, value }: { label: string; value: string }) { return <div><p className="text-xs text-text-muted">{label}</p><p className="mt-1 font-medium">{value}</p></div>; }
function roleText(role: AuthUser['role']) { return role === 'ADMIN' ? '管理员' : role === 'VIP' ? 'VIP 用户' : role === 'NORMAL' ? '普通用户' : '访客'; }
function vipText(status: AuthUser['vip_status']) { return status === 'ACTIVE' ? '有效' : status === 'EXPIRED' ? '已过期' : '未开通'; }
function utc(value: string) { return new Date(value).toISOString().replace('T', ' ').replace(/\.000Z$/, ' UTC').replace('Z', ' UTC'); }
