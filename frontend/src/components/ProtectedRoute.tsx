import { Navigate, Outlet } from 'react-router-dom';
import { useAuthStore } from '../stores/auth';
import type { UserRole } from '../types/api';

interface Props {
  requiredRole?: Exclude<UserRole, 'GUEST'>;
}

export default function ProtectedRoute({ requiredRole }: Props) {
  const token = useAuthStore((s) => s.token);
  const user = useAuthStore((s) => s.user);
  const loading = useAuthStore((s) => s.loading);
  const restored = useAuthStore((s) => s.restored);

  if (!token) return <Navigate to="/login" replace />;

  if (loading || !restored || !user) {
    return <div role="status" aria-live="polite">正在恢复会话...</div>;
  }

  if (requiredRole === 'VIP' && user?.role !== 'VIP') {
    return <Navigate to="/files" replace />;
  }

  return <Outlet />;
}
