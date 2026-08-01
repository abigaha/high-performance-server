import { Navigate, Outlet, useLocation } from 'react-router-dom';
import { useAuthStore } from '../stores/auth';
import type { Capability, UserRole } from '../types/api';

interface Props {
  requiredRole?: Exclude<UserRole, 'GUEST'>;
  requiredCapability?: Capability;
  allowedRoles?: Array<Exclude<UserRole, 'GUEST'>>;
}

export default function ProtectedRoute({ requiredRole, requiredCapability, allowedRoles }: Props) {
  const token = useAuthStore((s) => s.token);
  const user = useAuthStore((s) => s.user);
  const loading = useAuthStore((s) => s.loading);
  const restored = useAuthStore((s) => s.restored);
  const location = useLocation();

  if (!token) return <Navigate to="/login" replace state={{ from: location }} />;

  if (loading || !restored || !user) {
    return <div role="status" aria-live="polite">正在恢复会话...</div>;
  }

  if (requiredRole && user.role !== requiredRole) {
    return <Navigate to="/files" replace />;
  }

  if (requiredCapability && !user.capabilities.includes(requiredCapability)) {
    return <Navigate to="/files" replace />;
  }

  if (allowedRoles && !allowedRoles.includes(user.role as Exclude<UserRole, 'GUEST'>)) {
    return <Navigate to="/files" replace />;
  }

  return <Outlet />;
}
