import { NavLink } from 'react-router-dom';
import { File, MusicNote, Playlist, Upload, User, MusicNotes } from '@phosphor-icons/react';
import { useAuthStore } from '../stores/auth';
import ThemeToggle from './ThemeToggle';

const links = [
  { to: '/files', label: '文件', icon: File },
  { to: '/music/library', label: '音乐库', icon: MusicNotes },
  { to: '/my/music', label: '我的歌单', icon: Playlist },
  { to: '/upload', label: '上传', icon: Upload },
];

export default function Sidebar() {
  const user = useAuthStore((s) => s.user);

  return (
    <aside className="frosted-bar fixed left-0 top-0 h-full w-60 flex flex-col py-6 px-4 z-40">
      <div className="flex items-center gap-2 mb-8 px-2">
        <MusicNote size={28} className="text-primary" weight="fill" />
        <span className="font-display text-xl text-primary">Crystal</span>
      </div>

      <nav className="flex-1 flex flex-col gap-1">
        {links.map((l) => (
          <NavLink
            key={l.to}
            to={l.to}
            className={({ isActive }) =>
              `flex items-center gap-3 px-3 py-2.5 rounded-xl transition-all text-sm ${
                isActive ? 'bg-primary/20 text-primary font-medium' : 'text-text-muted hover:text-text hover:bg-white/10'
              }`
            }
          >
            <l.icon size={20} />
            {l.label}
          </NavLink>
        ))}
        {user?.role === 'VIP' && (
          <NavLink
            to="/users"
            className={({ isActive }) =>
              `flex items-center gap-3 px-3 py-2.5 rounded-xl transition-all text-sm ${
                isActive ? 'bg-primary/20 text-primary font-medium' : 'text-text-muted hover:text-text hover:bg-white/10'
              }`
            }
          >
            <User size={20} />
            用户管理
          </NavLink>
        )}
      </nav>

      <div className="px-2">
        <ThemeToggle />
      </div>
    </aside>
  );
}
