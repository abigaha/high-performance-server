import { useEffect } from 'react';
import { RouterProvider } from 'react-router-dom';
import { router } from './router';
import Toast from './components/Toast';
import { useAuthStore } from './stores/auth';
import { useToastStore } from './stores/toast';
import { injectToast } from './api/client';

export default function App() {
  const restore = useAuthStore((s) => s.restore);
  const toast = useToastStore();

  useEffect(() => {
    injectToast({ error: toast.error, success: toast.success });
    restore();
  }, [restore, toast.error, toast.success]);

  return (
    <>
      <Toast />
      <RouterProvider router={router} />
    </>
  );
}
