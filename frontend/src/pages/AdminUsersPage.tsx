import { useEffect, useLayoutEffect, useRef, useState } from 'react';
import { MagnifyingGlass, X } from '@phosphor-icons/react';
import { getAdminUsers, grantUserVip, revokeUserVip } from '../api/admin';
import Pagination from '../components/Pagination';
import type { AdminUserSummary } from '../types/api';

const PAGE_SIZE = 20;

export default function AdminUsersPage() {
  const [query, setQuery] = useState('');
  const [debouncedQuery, setDebouncedQuery] = useState('');
  const [searchRevision, setSearchRevision] = useState(0);
  const [page, setPage] = useState(1);
  const [users, setUsers] = useState<AdminUserSummary[]>([]);
  const [total, setTotal] = useState(0);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [busyUsers, setBusyUsers] = useState<Set<number>>(() => new Set());
  const [revokeTarget, setRevokeTarget] = useState<AdminUserSummary | null>(null);
  const [revokePending, setRevokePending] = useState(false);
  const [focusRevision, setFocusRevision] = useState(0);
  const requestId = useRef(0);
  const activeController = useRef<AbortController | null>(null);
  const searchTimer = useRef<number | null>(null);
  const revokeTrigger = useRef<HTMLButtonElement | null>(null);
  const cancelRevokeButton = useRef<HTMLButtonElement | null>(null);
  const busyUsersRef = useRef(new Set<number>());

  const abortActiveRequest = () => {
    activeController.current?.abort();
    activeController.current = null;
    requestId.current += 1;
  };

  const scheduleSearch = (nextQuery: string) => {
    if (searchTimer.current !== null) window.clearTimeout(searchTimer.current);
    searchTimer.current = window.setTimeout(() => {
      searchTimer.current = null;
      setPage(1);
      setDebouncedQuery(nextQuery.trim());
      setSearchRevision((value) => value + 1);
    }, 300);
  };

  useEffect(() => () => {
    if (searchTimer.current !== null) window.clearTimeout(searchTimer.current);
  }, []);

  useEffect(() => {
    const controller = new AbortController();
    const id = ++requestId.current;
    activeController.current = controller;
    setLoading(true);
    setError(null);
    getAdminUsers({ q: debouncedQuery, offset: (page - 1) * PAGE_SIZE, limit: PAGE_SIZE }, controller.signal)
      .then((response) => {
        if (id !== requestId.current) return;
        const maxPage = Math.max(1, Math.ceil(response.total / PAGE_SIZE));
        if (page > maxPage) {
          setTotal(response.total);
          setPage(maxPage);
          return;
        }
        setUsers(response.items);
        setTotal(response.total);
      })
      .catch((reason: unknown) => {
        if (id !== requestId.current || isAbort(reason)) return;
        setError(reason instanceof Error && reason.message ? reason.message : '用户列表加载失败');
      })
      .finally(() => {
        if (id === requestId.current) {
          activeController.current = null;
          setLoading(false);
        }
      });
    return () => {
      controller.abort();
      if (activeController.current === controller) activeController.current = null;
      requestId.current += 1;
    };
  }, [debouncedQuery, page, searchRevision]);

  useEffect(() => {
    if (revokeTarget) cancelRevokeButton.current?.focus();
  }, [revokeTarget]);

  useLayoutEffect(() => {
    if (focusRevision > 0 && !revokeTarget) revokeTrigger.current?.focus();
  }, [focusRevision, revokeTarget]);

  const replaceUser = (updated: AdminUserSummary) => {
    setUsers((current) => current.map((item) => item.user_id === updated.user_id ? updated : item));
  };

  const startUserMutation = (userId: number) => {
    if (busyUsersRef.current.has(userId)) return false;
    busyUsersRef.current.add(userId);
    setBusyUsers(new Set(busyUsersRef.current));
    return true;
  };

  const finishUserMutation = (userId: number) => {
    busyUsersRef.current.delete(userId);
    setBusyUsers(new Set(busyUsersRef.current));
  };

  const grant = async (target: AdminUserSummary, duration: 30 | 90 | 365) => {
    if (!startUserMutation(target.user_id)) return;
    abortActiveRequest();
    setError(null);
    try { replaceUser(await grantUserVip(target.user_id, duration)); }
    catch (reason) { setError(reason instanceof Error ? reason.message : '会员操作失败'); }
    finally { finishUserMutation(target.user_id); }
  };

  const revoke = async (target: AdminUserSummary) => {
    if (!startUserMutation(target.user_id)) return;
    abortActiveRequest();
    setRevokePending(true);
    setError(null);
    try { replaceUser(await revokeUserVip(target.user_id)); }
    catch (reason) { setError(reason instanceof Error ? reason.message : '会员撤销失败'); }
    finally {
      finishUserMutation(target.user_id);
      setRevokePending(false);
      setRevokeTarget(null);
      setFocusRevision((value) => value + 1);
    }
  };

  const closeRevokeDialog = () => {
    if (revokePending) return;
    setRevokeTarget(null);
    setFocusRevision((value) => value + 1);
  };

  const handleDialogKeyDown = (event: React.KeyboardEvent<HTMLDivElement>) => {
    if (event.key === 'Escape') {
      event.preventDefault();
      closeRevokeDialog();
      return;
    }
    if (event.key !== 'Tab') return;
    const buttons = Array.from(event.currentTarget.querySelectorAll<HTMLButtonElement>('button:not(:disabled)'));
    if (buttons.length === 0) return;
    const first = buttons[0];
    const last = buttons[buttons.length - 1];
    if (event.shiftKey && document.activeElement === first) {
      event.preventDefault();
      last.focus();
    } else if (!event.shiftKey && document.activeElement === last) {
      event.preventDefault();
      first.focus();
    }
  };

  return <div className="min-w-0">
    <header className="mb-5"><h1 className="font-display text-xl text-primary">用户管理</h1></header>
    <div className="mb-5 flex items-end gap-2">
      <label className="grid min-w-0 flex-1 gap-1 text-sm">搜索用户
        <span className="admin-users-search-control relative"><MagnifyingGlass className="admin-users-search-icon absolute left-3 top-3.5 text-text-muted" size={18} aria-hidden="true" />
          <input className="admin-users-search-input glass-input min-h-11 w-full" value={query} onChange={(event) => { const nextQuery = event.target.value; abortActiveRequest(); setLoading(false); setQuery(nextQuery); scheduleSearch(nextQuery); }} />
          {query && <button type="button" className="icon-button absolute right-0 top-0" aria-label="清除搜索" title="清除搜索" onClick={() => { abortActiveRequest(); setLoading(false); setQuery(''); scheduleSearch(''); }}><X size={18} aria-hidden="true" /></button>}
        </span>
      </label>
    </div>
    {error && <p role="alert" className="mb-4 text-sm text-destructive">{error}</p>}
    <section aria-busy={loading} aria-label="用户目录">
      {loading && users.length === 0 ? <p role="status" className="py-12 text-center text-text-muted">正在加载用户...</p> : users.length === 0 ? <p className="py-12 text-center text-text-muted">未找到用户</p> : <>
        <div className="hidden min-w-0 md:block"><table className="admin-users-table w-full border-collapse text-left"><colgroup><col className="admin-users-width-identity" /><col className="admin-users-width-role" /><col className="admin-users-width-status" /><col className="admin-users-width-expiry" /><col className="admin-users-width-actions" /></colgroup><thead><tr className="border-b border-[var(--surface-border)] text-text-muted"><th>用户</th><th className="admin-users-col-role">角色</th><th className="admin-users-col-status">状态</th><th className="admin-users-col-expiry">UTC 到期</th><th className="admin-users-col-actions">操作</th></tr></thead><tbody>{users.map((user) => <UserRow key={user.user_id} user={user} busy={busyUsers.has(user.user_id)} grant={grant} requestRevoke={(target, trigger) => { if (busyUsersRef.current.has(target.user_id)) return; revokeTrigger.current = trigger; setRevokeTarget(target); }} />)}</tbody></table></div>
        <div className="grid gap-3 md:hidden">{users.map((user) => <UserItem key={user.user_id} user={user} busy={busyUsers.has(user.user_id)} grant={grant} requestRevoke={(target, trigger) => { if (busyUsersRef.current.has(target.user_id)) return; revokeTrigger.current = trigger; setRevokeTarget(target); }} />)}</div>
        <Pagination current={page} total={total} pageSize={PAGE_SIZE} onChange={(nextPage) => { abortActiveRequest(); setLoading(false); setPage(nextPage); }} />
      </>}
    </section>
    {revokeTarget && <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/50 p-4"><div role="dialog" aria-modal="true" aria-labelledby="revoke-title" className="glass-card w-full max-w-sm p-5" onKeyDown={handleDialogKeyDown}><h2 id="revoke-title" className="text-base font-semibold">撤销会员</h2><p className="mt-2 text-sm text-text-muted">确定撤销 {revokeTarget.username} 的会员吗？</p><div className="mt-5 flex justify-end gap-2"><button ref={cancelRevokeButton} type="button" className="glass-button min-h-11 text-sm" aria-disabled={revokePending} onClick={closeRevokeDialog}>取消撤销</button><button type="button" className="admin-users-danger-button glass-button min-h-11 text-sm" aria-disabled={revokePending} onClick={() => { if (!revokePending) void revoke(revokeTarget); }}>确认撤销</button></div></div></div>}
  </div>;
}

type ActionProps = { user: AdminUserSummary; busy: boolean; grant: (user: AdminUserSummary, duration: 30 | 90 | 365) => void; requestRevoke: (user: AdminUserSummary, trigger: HTMLButtonElement) => void };
function Actions({ user, busy, grant, requestRevoke }: ActionProps) {
  if (user.role === 'ADMIN') return <span className="text-xs text-text-muted">不可修改</span>;
  return <div className="admin-users-actions flex flex-wrap gap-1" aria-busy={busy}>{([30, 90, 365] as const).map((duration) => <button key={duration} type="button" aria-disabled={busy} className="glass-button min-h-11 px-3 text-xs" aria-label={`授予 ${user.username} ${duration} 天会员`} onClick={() => { if (!busy) grant(user, duration); }}>{duration} 天</button>)}<button type="button" aria-disabled={busy || user.vip_status === 'NONE'} className={`glass-button min-h-11 px-3 text-xs ${user.vip_status === 'NONE' ? 'admin-users-action-disabled' : 'admin-users-danger-button'}`} aria-label={user.vip_status === 'NONE' ? `${user.username} 会员已撤销` : `撤销 ${user.username} 的会员`} onClick={(event) => { if (!busy && user.vip_status !== 'NONE') requestRevoke(user, event.currentTarget); }}>{user.vip_status === 'NONE' ? '已撤销' : '撤销'}</button></div>;
}
function UserRow(props: ActionProps) { const { user } = props; return <tr className="border-b border-[var(--surface-border)]"><td><strong className="admin-users-identity block" title={user.username}>{user.username}</strong><span className="admin-users-identity block text-text-muted" title={user.email}>{user.email}</span></td><td className="admin-users-col-role">{role(user.role)}</td><td className="admin-users-col-status">{status(user.vip_status)}</td><td className="admin-users-col-expiry">{expiry(user)}</td><td className="admin-users-col-actions"><Actions {...props} /></td></tr>; }
function UserItem(props: ActionProps) { const { user } = props; return <article className="glass-card min-w-0 p-4"><div className="mb-3 flex justify-between gap-3"><div className="min-w-0"><h2 className="truncate font-medium">{user.username}</h2><p className="truncate text-xs text-text-muted">{user.email}</p></div><span className="text-sm">{role(user.role)}</span></div><dl className="mb-3 grid grid-cols-[minmax(0,1fr)_minmax(0,1fr)] gap-2 text-sm"><div><dt className="text-xs text-text-muted">状态</dt><dd>{status(user.vip_status)}</dd></div><div className="min-w-0"><dt className="text-xs text-text-muted">UTC 到期</dt><dd className="break-words">{expiry(user)}</dd></div></dl><Actions {...props} /></article>; }
function expiry(user: AdminUserSummary) { return user.vip_expires_at ? <time dateTime={user.vip_expires_at}>{utc(user.vip_expires_at)}</time> : '无'; }
function role(value: AdminUserSummary['role']) { return value === 'ADMIN' ? '管理员' : value === 'VIP' ? 'VIP' : '普通用户'; }
function status(value: AdminUserSummary['vip_status']) { return value === 'ACTIVE' ? '有效' : value === 'EXPIRED' ? '已过期' : '未开通'; }
function utc(value: string) { return new Date(value).toISOString().replace('T', ' ').replace(/\.000Z$/, ' UTC').replace('Z', ' UTC'); }
function isAbort(reason: unknown) { return reason instanceof DOMException && reason.name === 'AbortError'; }
