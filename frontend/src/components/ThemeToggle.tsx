import { Sun, Moon } from '@phosphor-icons/react';
import { useEffect, useState } from 'react';

export default function ThemeToggle() {
  const [dark, setDark] = useState(() => document.documentElement.classList.contains('dark'));

  useEffect(() => {
    if (dark) {
      document.documentElement.classList.add('dark');
    } else {
      document.documentElement.classList.remove('dark');
    }
  }, [dark]);

  return (
    <button
      onClick={() => setDark(!dark)}
      className="glass-button !p-2 !rounded-full !bg-transparent !text-text hover:!bg-white/10"
      aria-label="主题切换"
    >
      {dark ? <Sun size={20} /> : <Moon size={20} />}
    </button>
  );
}
