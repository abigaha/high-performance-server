import { request } from './client';
import type { VipMembership, VipPlan } from '../types/api';

export async function getVipPlans(): Promise<VipPlan[]> {
  const response = await request<{ plans: VipPlan[] }>('/api/vip/plans');
  return response.plans;
}

export function getVipMembership(): Promise<VipMembership> {
  return request<VipMembership>('/api/vip/membership');
}

export function activateVipMembership(durationDays: 30 | 90 | 365): Promise<VipMembership> {
  return request<VipMembership>('/api/vip/membership/activate', {
    method: 'POST',
    body: JSON.stringify({ duration_days: durationDays }),
  });
}
