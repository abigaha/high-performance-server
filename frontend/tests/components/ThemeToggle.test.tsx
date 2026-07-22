import { render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it } from 'vitest';
import ThemeToggle from '../../src/components/ThemeToggle';

describe('ThemeToggle', () => {
  beforeEach(() => {
    localStorage.removeItem('theme');
    document.documentElement.classList.remove('dark');
    document.documentElement.style.colorScheme = '';
  });

  it('切换深浅主题并持久化选择', async () => {
    const user = userEvent.setup();
    render(<ThemeToggle />);
    const toggle = screen.getByRole('button', { name: '切换到深色主题' });

    await user.click(toggle);

    expect(document.documentElement).toHaveClass('dark');
    expect(document.documentElement.style.colorScheme).toBe('dark');
    expect(localStorage.getItem('theme')).toBe('dark');
    expect(screen.getByRole('button', { name: '切换到浅色主题' })).toHaveAttribute('aria-pressed', 'true');
  });

  it('优先恢复已保存的深色主题', () => {
    localStorage.setItem('theme', 'dark');
    render(<ThemeToggle />);

    expect(screen.getByRole('button', { name: '切换到浅色主题' })).toBeInTheDocument();
    expect(document.documentElement).toHaveClass('dark');
  });
});
