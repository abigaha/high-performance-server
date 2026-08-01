import { Navigate } from 'react-router-dom';
import { useAuthStore } from '../stores/auth';

export default function RoleUsersRedirect() {
  const user = useAuthStore((state) => state.user);
  return <Navigate to={user?.role === 'ADMIN' ? '/admin/users' : '/files'} replace />;
}
