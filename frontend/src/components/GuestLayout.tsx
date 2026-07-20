import { Outlet } from 'react-router-dom';
import { MusicNote } from '@phosphor-icons/react';

export default function GuestLayout() {
  return (
    <div className="min-h-screen flex flex-col items-center justify-center p-4">
      <div className="flex items-center gap-2 mb-8">
        <MusicNote size={36} className="text-primary" weight="fill" />
        <span className="font-display text-3xl text-primary">Crystal Music</span>
      </div>
      <Outlet />
    </div>
  );
}
