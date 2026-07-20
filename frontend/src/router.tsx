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
import UserManagePage from './pages/UserManagePage';

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
        ],
      },
    ],
  },
  {
    element: <ProtectedRoute requiredRole="VIP" />,
    children: [
      {
        element: <AppLayout />,
        children: [
          { path: '/users', element: <UserManagePage /> },
        ],
      },
    ],
  },
  { path: '/', element: <LoginPage /> },
]);
