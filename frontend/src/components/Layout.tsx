import { useCallback, useEffect, useRef, useState } from 'react';
import { Outlet, useLocation } from 'react-router-dom';
import Sidebar from './Sidebar';
import Header from './Header';
import AudioPlayer from './AudioPlayer';

export default function AppLayout() {
  const [menuOpen, setMenuOpen] = useState(false);
  const menuOpenRef = useRef(false);
  const menuButtonRef = useRef<HTMLButtonElement>(null);
  const sidebarRef = useRef<HTMLElement>(null);
  const location = useLocation();
  const isFullScreenPlayer = /^\/player\/[^/]+$/.test(location.pathname);

  const restoreMenuFocus = useCallback(() => {
    const focusTrigger = () => menuButtonRef.current?.focus();
    if (typeof window.requestAnimationFrame === 'function') {
      window.requestAnimationFrame(focusTrigger);
    } else {
      window.setTimeout(focusTrigger, 0);
    }
  }, []);

  const openMenu = useCallback(() => {
    menuOpenRef.current = true;
    setMenuOpen(true);
  }, []);

  const closeMenu = useCallback((restoreFocus = true) => {
    if (!menuOpenRef.current) return;
    menuOpenRef.current = false;
    setMenuOpen(false);
    if (restoreFocus) restoreMenuFocus();
  }, [restoreMenuFocus]);

  useEffect(() => {
    if (!menuOpen) return;

    const previousOverflow = document.body.style.overflow;
    document.body.style.overflow = 'hidden';
    const focusDrawer = () => {
      sidebarRef.current?.querySelector<HTMLButtonElement>('[data-drawer-close]')?.focus();
    };
    const frame = typeof window.requestAnimationFrame === 'function'
      ? window.requestAnimationFrame(focusDrawer)
      : undefined;
    if (frame === undefined) window.setTimeout(focusDrawer, 0);

    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') closeMenu();
    };
    document.addEventListener('keydown', handleKeyDown);

    return () => {
      if (frame !== undefined) window.cancelAnimationFrame(frame);
      document.body.style.overflow = previousOverflow;
      document.removeEventListener('keydown', handleKeyDown);
    };
  }, [closeMenu, menuOpen]);

  useEffect(() => {
    closeMenu();
  }, [closeMenu, location.pathname]);

  useEffect(() => {
    if (typeof window.matchMedia !== 'function') return;

    const media = window.matchMedia('(min-width: 1024px)');
    const handleChange = (event: MediaQueryListEvent) => {
      if (event.matches) closeMenu(false);
    };
    media.addEventListener('change', handleChange);
    return () => media.removeEventListener('change', handleChange);
  }, [closeMenu]);

  return (
    <div className="min-h-screen">
      <button
        type="button"
        aria-label="关闭导航遮罩"
        tabIndex={menuOpen ? 0 : -1}
        onClick={() => closeMenu()}
        className={`drawer-backdrop fixed inset-0 z-40 lg:hidden ${menuOpen ? 'is-open' : ''}`}
      />
      <Sidebar open={menuOpen} onClose={() => closeMenu()} sidebarRef={sidebarRef} />
      <Header
        menuOpen={menuOpen}
        onMenuOpen={openMenu}
        menuButtonRef={menuButtonRef}
      />
      <main className="min-w-0 px-4 pb-24 pt-20 sm:px-6 lg:ml-60 lg:px-8">
        <Outlet />
      </main>
      {!isFullScreenPlayer && <AudioPlayer mode="mini" />}
    </div>
  );
}
