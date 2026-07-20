import { useAuthStore } from '../stores/auth';
import { useNavigate } from 'react-router-dom';
import { SignOut, UserCircle } from '@phosphor-icons/react';

export default function Header() {
  const user = useAuthStore((s) => s.user);
  const logout = useAuthStore((s) => s.logout);
  const navigate = useNavigate();

  const handleLogout = () => {
    logout();
    navigate('/login');
  };

  return (
    <header className="frosted-bar fixed top-0 left-60 right-0 h-16 flex items-center justify-between px-6 z-30">
      <div />
      <div className="flex items-center gap-4">
        {user && (
          <>
            <div className="flex items-center gap-2 text-sm">
              <UserCircle size={22} className="text-primary" />
              <span>{user.username}</span>
              <span className={`text-xs px-2 py-0.5 rounded-full ${
                user.role === 'VIP' ? 'bg-amber-500/20 text-amber-400' : 'bg-primary/20 text-primary'
              }`}>
                {user.role}
              </span>
            </div>
            <button
              onClick={handleLogout}
              className="flex items-center gap-1 text-sm text-text-muted hover:text-destructive transition-colors"
            >
              <SignOut size={18} />
              登出
            </button>
          </>
        )}
      </div>
    </header>
  );
}
