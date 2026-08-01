import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import '@fontsource/inter/400.css';
import '@fontsource/inter/500.css';
import '@fontsource/inter/600.css';
import '@fontsource/righteous/400.css';
import './index.css';
import App from './App';
import { injectUnauthorizedHandler } from './api/client';
import { clearUserSession } from './session/clearUserSession';
import { createUnauthorizedHandler } from './session/createUnauthorizedHandler';
import { injectSessionClearer } from './stores/auth';

injectSessionClearer(clearUserSession);
injectUnauthorizedHandler(createUnauthorizedHandler(clearUserSession, () => {
  if (window.location.pathname !== '/login') window.location.assign('/login');
}));

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
