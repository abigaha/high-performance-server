import { Navigate, Outlet } from 'react-router-dom';
import { useAuthStore } from '../stores/auth';

interface Props {
  requiredRole?: 'NORMAL' | 'VIP';
}

export default function ProtectedRoute({ requiredRole }: Props) {
  const token = useAuthStore((s) => s.token);
  const user = useAuthStore((s) => s.user);

  if (!token) return <Navigate to="/login" replace />;

  if (requiredRole === 'VIP' && user?.role !== 'VIP') {
    return <Navigate to="/files" replace />;
  }

  return <Outlet />;
}
