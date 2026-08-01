import type { RefObject } from 'react';
import { useLocation, useNavigate } from 'react-router-dom';
import { List, SignOut, UserCircle } from '@phosphor-icons/react';
import { useAuthStore } from '../stores/auth';

interface HeaderProps {
  menuOpen: boolean;
  onMenuOpen: () => void;
  menuButtonRef: RefObject<HTMLButtonElement | null>;
}

function getPageTitle(pathname: string): string {
  if (pathname === '/files') return '文件';
  if (/^\/files\/[^/]+$/.test(pathname)) return '文件详情';
  if (pathname === '/music/library') return '音乐库';
  if (pathname === '/my/music') return '我的歌单';
  if (pathname === '/upload') return '上传音频';
  if (/^\/player\/[^/]+$/.test(pathname)) return '播放器';
  if (pathname === '/users' || pathname === '/admin/users') return '用户管理';
  if (pathname === '/profile') return '个人资料';
  if (pathname === '/vip') return '会员中心';
  return 'Crystal';
}

export default function Header({ menuOpen, onMenuOpen, menuButtonRef }: HeaderProps) {
  const user = useAuthStore((state) => state.user);
  const logout = useAuthStore((state) => state.logout);
  const location = useLocation();
  const navigate = useNavigate();

  const handleLogout = () => {
    logout();
    navigate('/login', { replace: true });
  };

  return (
    <header className="app-header fixed inset-x-0 top-0 z-30 flex h-16 items-center justify-between gap-3 px-3 sm:px-6 lg:left-60">
      <div className="flex min-w-0 items-center gap-2">
        <button
          ref={menuButtonRef}
          type="button"
          onClick={onMenuOpen}
          className="icon-button lg:hidden"
          aria-label="打开导航菜单"
          aria-controls="app-sidebar"
          aria-expanded={menuOpen}
          title="打开导航菜单"
        >
          <List size={22} aria-hidden="true" />
        </button>
        <h1 className="app-header-title truncate text-base font-semibold sm:text-lg">
          {getPageTitle(location.pathname)}
        </h1>
      </div>

      {user && (
        <div className="flex min-w-0 items-center gap-1 sm:gap-3">
          <div
            className="flex min-w-0 items-center gap-2 text-sm"
            aria-label={`当前用户：${user.username}，角色：${user.role}`}
          >
            <UserCircle size={22} className="shrink-0 text-primary" aria-hidden="true" />
            <span className="max-w-20 truncate sm:max-w-40">{user.username}</span>
            <span className={`role-badge hidden sm:inline-flex ${user.role === 'VIP' ? 'is-vip' : ''}`}>
              {user.role === 'VIP' ? 'VIP' : '普通用户'}
            </span>
          </div>
          <button
            type="button"
            onClick={handleLogout}
            className="icon-button sm:w-auto sm:px-3"
            aria-label="退出登录"
            title="退出登录"
          >
            <SignOut size={19} aria-hidden="true" />
            <span className="hidden sm:inline">退出</span>
          </button>
        </div>
      )}
    </header>
  );
}
