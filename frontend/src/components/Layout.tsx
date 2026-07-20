import { Outlet } from 'react-router-dom';
import Sidebar from './Sidebar';
import Header from './Header';
import AudioPlayer from './AudioPlayer';

export default function AppLayout() {
  return (
    <div className="min-h-screen">
      <Sidebar />
      <Header />
      <main className="ml-60 pt-16 pb-20 p-6">
        <Outlet />
      </main>
      <AudioPlayer mode="mini" />
    </div>
  );
}
