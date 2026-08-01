import { useCallback, useEffect, useRef, useState } from 'react';
import { Crown } from '@phosphor-icons/react';
import { activateVipMembership, getVipMembership, getVipPlans } from '../api/vip';
import { getUser } from '../api/users';
import { captureSessionSnapshot, isSessionSnapshotCurrent } from '../api/client';
import { useAuthStore } from '../stores/auth';
import type { VipMembership, VipPlan } from '../types/api';

type LoadStatus = 'loading' | 'ready' | 'error';
type ClockAnchor = { baseServerEpoch: number; basePerformanceNow: number };

export default function VipCenterPage() {
  const user = useAuthStore((state) => state.user);
  const sessionRevision = useAuthStore((state) => state.sessionRevision);
  const setUser = useAuthStore((state) => state.setUser);
  const [plans, setPlans] = useState<VipPlan[]>([]);
  const [membership, setMembership] = useState<VipMembership | null>(null);
  const [remaining, setRemaining] = useState(0);
  const [busy, setBusy] = useState<number | null>(null);
  const [loadStatus, setLoadStatus] = useState<LoadStatus>('loading');
  const [error, setError] = useState<string | null>(null);
  const clockAnchorRef = useRef<ClockAnchor | null>(null);
  const operationRef = useRef(0);
  const userId = user?.user_id;

  const load = useCallback(async () => {
    if (userId === undefined) return;
    const operation = ++operationRef.current;
    const session = captureSessionSnapshot();
    setLoadStatus('loading');
    setError(null);
    try {
      const [nextPlans, measuredMembership] = await Promise.all([
        getVipPlans(),
        measureMembership(getVipMembership),
      ]);
      if (operation !== operationRef.current || !isSessionSnapshotCurrent(session)) return;
      const { membership: nextMembership, anchor } = measuredMembership;
      clockAnchorRef.current = anchor;
      setPlans(nextPlans);
      setMembership(nextMembership);
      setRemaining(calibratedRemaining(nextMembership, anchor));
      setLoadStatus('ready');
    } catch (reason) {
      if (operation !== operationRef.current || !isSessionSnapshotCurrent(session)) return;
      setError(message(reason, '会员信息加载失败'));
      setLoadStatus('error');
    }
  }, [userId]);

  useEffect(() => {
    operationRef.current += 1;
    setBusy(null);
    setError(null);
    setMembership(null);
    setRemaining(0);
    clockAnchorRef.current = null;
    void load();
    return () => { operationRef.current += 1; };
  }, [load, sessionRevision]);

  const hasRemaining = remaining > 0;
  useEffect(() => {
    if (!membership || !hasRemaining || clockAnchorRef.current === null) return;
    const interval = window.setInterval(() => {
      setRemaining(calibratedRemaining(membership, clockAnchorRef.current));
    }, 1_000);
    return () => window.clearInterval(interval);
  }, [membership, hasRemaining]);

  const activate = async (duration: 30 | 90 | 365) => {
    if (!user || busy !== null) return;
    const operation = ++operationRef.current;
    const session = captureSessionSnapshot();
    setBusy(duration);
    setError(null);
    try {
      const { membership: nextMembership, anchor } = await measureMembership(
        () => activateVipMembership(duration),
      );
      if (operation !== operationRef.current || !isSessionSnapshotCurrent(session)) return;
      const refreshedUser = await getUser(user.user_id);
      if (operation !== operationRef.current || !isSessionSnapshotCurrent(session)) return;
      clockAnchorRef.current = anchor;
      setMembership(nextMembership);
      setRemaining(calibratedRemaining(nextMembership, anchor));
      setUser(refreshedUser);
    } catch (reason) {
      if (operation === operationRef.current && isSessionSnapshotCurrent(session)) {
        setError(message(reason, '会员操作失败'));
      }
    } finally {
      if (operation === operationRef.current && isSessionSnapshotCurrent(session)) setBusy(null);
    }
  };

  return (
    <div className="mx-auto max-w-4xl">
      <header className="mb-6 flex items-center gap-3">
        <Crown size={26} className="text-warning" aria-hidden="true" />
        <div><h1 className="font-display text-xl text-primary">会员中心</h1><p className="text-sm text-text-muted">选择固定时长激活或续期</p></div>
      </header>
      {loadStatus === 'loading' && <p role="status" className="py-12 text-center text-text-muted">正在加载会员信息...</p>}
      {loadStatus === 'error' && <div className="py-8 text-center"><p role="alert" className="text-sm text-destructive">{error}</p><button type="button" className="glass-button mt-4 min-h-11 text-sm" onClick={() => void load()}>重试会员信息</button></div>}
      {loadStatus === 'ready' && membership && <>
        {error && <p role="alert" className="mb-4 text-sm text-destructive">{error}</p>}
        <section className="mb-6 border-y border-[var(--surface-border)] py-5">
          <div className="grid gap-4 sm:grid-cols-3">
            <div><p className="text-xs text-text-muted">会员状态</p><p className="mt-1 font-medium">{statusText(effectiveStatus(membership, remaining))}</p></div>
            <div><p className="text-xs text-text-muted">UTC 到期</p>{membership.vip_expires_at ? <time dateTime={membership.vip_expires_at} className="mt-1 block text-sm">{utc(membership.vip_expires_at)}</time> : <p className="mt-1">未开通</p>}</div>
            <div><p className="text-xs text-text-muted">剩余时间</p><p className="mt-1 font-mono text-lg" aria-live="polite">{countdown(remaining)}</p></div>
          </div>
        </section>
        <section aria-labelledby="plans-title">
          <h2 id="plans-title" className="mb-3 text-base font-semibold">会员计划</h2>
          <div className="grid gap-3 sm:grid-cols-3">
            {plans.map((plan) => <article key={plan.duration_days} className="glass-card p-4">
              <h3 className="font-medium">{plan.label}</h3>
              <button type="button" className="glass-button mt-4 min-h-11 w-full text-sm" disabled={busy !== null} aria-busy={busy === plan.duration_days} onClick={() => void activate(plan.duration_days)}>
                {busy === plan.duration_days ? '处理中' : `${membership.vip_status === 'NONE' ? '激活' : '续期'} ${plan.label}会员`}
              </button>
            </article>)}
          </div>
        </section>
      </>}
    </div>
  );
}

function statusText(status?: VipMembership['vip_status']) { return status === 'ACTIVE' ? '有效' : status === 'EXPIRED' ? '已过期' : '未开通'; }
function countdown(seconds: number) {
  const hours = Math.floor(seconds / 3600).toString().padStart(2, '0');
  const minutes = Math.floor((seconds % 3600) / 60).toString().padStart(2, '0');
  const rest = Math.floor(seconds % 60).toString().padStart(2, '0');
  return `${hours}:${minutes}:${rest}`;
}
async function measureMembership(request: () => Promise<VipMembership>) {
  const epochStart = Date.now();
  const performanceStart = performance.now();
  const membership = await request();
  const epochEnd = Date.now();
  const performanceEnd = performance.now();
  return {
    membership,
    anchor: clockAnchor(membership, epochStart, performanceStart, epochEnd, performanceEnd),
  };
}
function clockAnchor(
  membership: VipMembership,
  epochStart: number,
  performanceStart: number,
  epochEnd: number,
  performanceEnd: number,
): ClockAnchor | null {
  const serverNow = Date.parse(membership.server_now);
  if (!Number.isFinite(serverNow)) return null;
  const monotonicRoundTrip = performanceEnd - performanceStart;
  const roundTrip = Number.isFinite(monotonicRoundTrip) && monotonicRoundTrip >= 0
    ? monotonicRoundTrip
    : Math.max(0, epochEnd - epochStart);
  const localReceiveEpoch = epochStart + roundTrip;
  const localMidpointEpoch = localReceiveEpoch - roundTrip / 2;
  const serverOffset = serverNow - localMidpointEpoch;
  return {
    baseServerEpoch: localReceiveEpoch + serverOffset,
    basePerformanceNow: performanceEnd,
  };
}
function calibratedRemaining(membership: VipMembership, anchor: ClockAnchor | null) {
  if (!membership.vip_expires_at) return 0;
  const expiresAt = Date.parse(membership.vip_expires_at);
  if (!Number.isFinite(expiresAt) || anchor === null) return membership.remaining_seconds;
  const serverNow = anchor.baseServerEpoch + performance.now() - anchor.basePerformanceNow;
  return Math.max(0, Math.floor((expiresAt - serverNow) / 1_000));
}
function effectiveStatus(membership: VipMembership, remaining: number) {
  return membership.vip_status === 'ACTIVE' && remaining <= 0 ? 'EXPIRED' : membership.vip_status;
}
function utc(value: string) { return `${new Date(value).toISOString().replace('T', ' ').replace(/\.000Z$/, ' UTC').replace('Z', ' UTC')}`; }
function message(reason: unknown, fallback: string) { return reason instanceof Error && reason.message ? reason.message : fallback; }
