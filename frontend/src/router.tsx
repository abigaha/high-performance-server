import { createBrowserRouter } from 'react-router-dom';
import GuestLayout from './components/GuestLayout';
import AppLayout from './components/Layout';
import ProtectedRoute from './components/ProtectedRoute';
import LoginPage from './pages/LoginPage';
import RegisterPage from './pages/RegisterPage';
import FileListPage from './pages/FileListPage';
import FileDetailPage from './pages/FileDetailPage';
import UploadPage from './pages/UploadPage';
import MusicLibraryPage from './pages/MusicLibraryPage';
import UserPlaylistPage from './pages/UserPlaylistPage';
import PlayerPage from './pages/PlayerPage';
import VipCenterPage from './pages/VipCenterPage';
import ProfilePage from './pages/ProfilePage';
import AdminUsersPage from './pages/AdminUsersPage';
import RoleUsersRedirect from './components/RoleUsersRedirect';

export const router = createBrowserRouter([
  {
    element: <GuestLayout />,
    children: [
      { path: '/login', element: <LoginPage /> },
      { path: '/register', element: <RegisterPage /> },
    ],
  },
  {
    element: <ProtectedRoute />,
    children: [
      {
        element: <AppLayout />,
        children: [
          { path: '/files', element: <FileListPage /> },
          { path: '/files/:id', element: <FileDetailPage /> },
          { path: '/upload', element: <UploadPage /> },
          { path: '/music/library', element: <MusicLibraryPage /> },
          { path: '/my/music', element: <UserPlaylistPage /> },
          { path: '/player/:id', element: <PlayerPage /> },
          { path: '/profile', element: <ProfilePage /> },
          { path: '/users', element: <RoleUsersRedirect /> },
        ],
      },
    ],
  },
  {
    element: <ProtectedRoute allowedRoles={['NORMAL', 'VIP']} />,
    children: [
      {
        element: <AppLayout />,
        children: [
          { path: '/vip', element: <VipCenterPage /> },
        ],
      },
    ],
  },
  {
    element: <ProtectedRoute requiredCapability="MANAGE_USERS" />,
    children: [
      {
        element: <AppLayout />,
        children: [{ path: '/admin/users', element: <AdminUsersPage /> }],
      },
    ],
  },
  { path: '/', element: <LoginPage /> },
]);
