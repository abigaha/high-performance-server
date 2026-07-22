import { Outlet } from 'react-router-dom';
import { MusicNote } from '@phosphor-icons/react';
import ThemeToggle from './ThemeToggle';

export default function GuestLayout() {
  return (
    <div className="relative flex min-h-screen flex-col items-center justify-center px-4 py-16">
      <div className="absolute right-3 top-3 w-36 sm:right-5 sm:top-5">
        <ThemeToggle />
      </div>
      <div className="mb-8 flex items-center gap-2" aria-label="Crystal Music">
        <MusicNote size={34} className="text-primary" weight="fill" aria-hidden="true" />
        <span className="font-display text-2xl text-text sm:text-3xl">Crystal Music</span>
      </div>
      <Outlet />
    </div>
  );
}
