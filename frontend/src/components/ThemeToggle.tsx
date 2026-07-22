import { Moon, Sun } from '@phosphor-icons/react';
import { useEffect, useState } from 'react';

type Theme = 'light' | 'dark';

function getInitialTheme(): Theme {
  const stored = localStorage.getItem('theme');
  if (stored === 'light' || stored === 'dark') return stored;
  if (document.documentElement.classList.contains('dark')) return 'dark';
  if (typeof window.matchMedia === 'function' && window.matchMedia('(prefers-color-scheme: dark)').matches) {
    return 'dark';
  }
  return 'light';
}

export default function ThemeToggle() {
  const [theme, setTheme] = useState<Theme>(getInitialTheme);
  const dark = theme === 'dark';

  useEffect(() => {
    document.documentElement.classList.toggle('dark', dark);
    document.documentElement.style.colorScheme = theme;
    localStorage.setItem('theme', theme);
  }, [dark, theme]);

  const label = dark ? '切换到浅色主题' : '切换到深色主题';

  return (
    <button
      type="button"
      onClick={() => setTheme(dark ? 'light' : 'dark')}
      className="theme-toggle"
      aria-label={label}
      aria-pressed={dark}
      title={label}
    >
      {dark ? <Sun size={20} aria-hidden="true" /> : <Moon size={20} aria-hidden="true" />}
      <span>外观</span>
      <span className="ml-auto text-xs text-text-muted">{dark ? '深色' : '浅色'}</span>
    </button>
  );
}
