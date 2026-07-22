import { useEffect, useState } from 'react';
import type { RefObject } from 'react';
import { NavLink } from 'react-router-dom';
import {
  File,
  MusicNote,
  MusicNotes,
  Playlist,
  Upload,
  User,
  X,
} from '@phosphor-icons/react';
import { useAuthStore } from '../stores/auth';
import ThemeToggle from './ThemeToggle';

const links = [
  { to: '/files', label: '文件', icon: File },
  { to: '/music/library', label: '音乐库', icon: MusicNotes },
  { to: '/my/music', label: '我的歌单', icon: Playlist },
  { to: '/upload', label: '上传', icon: Upload },
];

interface SidebarProps {
  open: boolean;
  onClose: () => void;
  sidebarRef: RefObject<HTMLElement | null>;
}

function useDesktopLayout(): boolean {
  const [desktop, setDesktop] = useState(() => (
    typeof window.matchMedia === 'function'
      ? window.matchMedia('(min-width: 1024px)').matches
      : false
  ));

  useEffect(() => {
    if (typeof window.matchMedia !== 'function') return;

    const media = window.matchMedia('(min-width: 1024px)');
    const handleChange = (event: MediaQueryListEvent) => setDesktop(event.matches);
    media.addEventListener('change', handleChange);
    return () => media.removeEventListener('change', handleChange);
  }, []);

  return desktop;
}

export default function Sidebar({ open, onClose, sidebarRef }: SidebarProps) {
  const user = useAuthStore((state) => state.user);
  const desktop = useDesktopLayout();
  const hidden = !desktop && !open;
  const linkClassName = ({ isActive }: { isActive: boolean }) => (
    `sidebar-link ${isActive ? 'is-active' : ''}`
  );

  return (
    <aside
      id="app-sidebar"
      ref={sidebarRef}
      role={desktop ? undefined : 'dialog'}
      aria-modal={desktop ? undefined : true}
      aria-label="主导航"
      aria-hidden={hidden}
      inert={hidden}
      className={`app-sidebar fixed inset-y-0 left-0 z-50 flex w-60 flex-col px-4 py-5 transition-transform duration-200 lg:translate-x-0 ${
        open ? 'translate-x-0' : '-translate-x-full'
      }`}
    >
      <div className="mb-7 flex min-h-11 items-center justify-between gap-2 px-2">
        <NavLink to="/files" onClick={onClose} className="flex min-w-0 items-center gap-2" aria-label="Crystal 首页">
          <MusicNote size={26} className="shrink-0 text-primary" weight="fill" aria-hidden="true" />
          <span className="truncate font-display text-lg text-text">Crystal</span>
        </NavLink>
        <button
          type="button"
          onClick={onClose}
          className="icon-button lg:hidden"
          aria-label="关闭导航菜单"
          title="关闭导航菜单"
          data-drawer-close
        >
          <X size={20} aria-hidden="true" />
        </button>
      </div>

      <nav className="flex flex-1 flex-col gap-1" aria-label="功能导航">
        {links.map((link) => (
          <NavLink key={link.to} to={link.to} onClick={onClose} className={linkClassName}>
            <link.icon size={20} aria-hidden="true" />
            <span>{link.label}</span>
          </NavLink>
        ))}
        {user?.role === 'VIP' && (
          <NavLink to="/users" onClick={onClose} className={linkClassName}>
            <User size={20} aria-hidden="true" />
            <span>用户管理</span>
          </NavLink>
        )}
      </nav>

      <div className="border-t border-[var(--surface-border)] px-1 pt-4">
        <ThemeToggle />
      </div>
    </aside>
  );
}
