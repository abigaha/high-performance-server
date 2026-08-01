import { readFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';

const css = await readFile(resolve(process.cwd(), 'src/index.css'), 'utf8');
const html = await readFile(resolve(process.cwd(), 'index.html'), 'utf8');

function cssBlock(selector: string): string {
  const escaped = selector.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  return css.match(new RegExp(`${escaped}\\s*\\{([^}]*)\\}`))?.[1] ?? '';
}

function token(block: string, name: string): string {
  return block.match(new RegExp(`${name}:\\s*(#[0-9a-f]{6})`, 'i'))?.[1] ?? '';
}

function relativeLuminance(hex: string): number {
  const channels = hex.slice(1).match(/.{2}/g)?.map((channel) => Number.parseInt(channel, 16) / 255) ?? [];
  const [red, green, blue] = channels.map((channel) => channel <= 0.04045 ? channel / 12.92 : ((channel + 0.055) / 1.055) ** 2.4);
  return 0.2126 * red + 0.7152 * green + 0.0722 * blue;
}

function contrast(foreground: string, background: string): number {
  const lighter = Math.max(relativeLuminance(foreground), relativeLuminance(background));
  const darker = Math.min(relativeLuminance(foreground), relativeLuminance(background));
  return (lighter + 0.05) / (darker + 0.05);
}

describe('Crystal Music 设计令牌', () => {
  it.each([
    ['--color-bg', '#F0FDF4'], ['--color-bg-end', '#ECFDF5'],
    ['--color-primary', '#059669'], ['--color-accent', '#10B981'],
    ['--color-secondary', '#34D399'], ['--color-text', '#0F172A'],
    ['--color-text-muted', '#52645B'], ['--color-destructive', '#DC2626'],
    ['--color-info', '#2563EB'], ['--color-warning', '#B45309'],
  ])('浅色 %s 使用 %s', (token, value) => expect(css).toContain(`${token}: ${value}`));

  it('包含 12px/20px 模糊、16px/12px 圆角及两类可访问性回退', () => {
    expect(css).toMatch(/--blur-glass:\s*12px/);
    expect(css).toMatch(/--blur-fixed:\s*20px/);
    expect(css).toMatch(/--radius-card:\s*16px/);
    expect(css).toMatch(/--radius-control:\s*12px/);
    expect(css).toContain('@supports not ((-webkit-backdrop-filter: blur(1px)) or (backdrop-filter: blur(1px)))');
    expect(css).toContain('@media (forced-colors: active)');
  });

  it('不从远程 stylesheet 加载字体', () => {
    expect(html).not.toContain('fonts.googleapis.com');
    expect(html).not.toContain('fonts.gstatic.com');
    expect(html).not.toMatch(/<link\b(?=[^>]*\brel=["']stylesheet["'])(?=[^>]*\bhref=["']https?:\/\/)[^>]*>/i);
  });

  it('中宽用户表格保持角色状态完整、用户省略、UTC 两行和 44px 操作目标', () => {
    expect(css).toMatch(/\.admin-users-table\s+\.admin-users-col-(?:role|status)[^{]*\{[^}]*white-space:\s*nowrap/s);
    expect(css).toMatch(/\.admin-users-identity[^{]*\{[^}]*overflow:\s*hidden[^}]*text-overflow:\s*ellipsis[^}]*white-space:\s*nowrap/s);
    expect(css).toMatch(/\.admin-users-col-expiry\s+time[^{]*\{[^}]*white-space:\s*normal/s);
    expect(css).toMatch(/@media\s*\(min-width:\s*768px\)\s*and\s*\(max-width:\s*1279px\)[\s\S]*\.admin-users-actions[^{]*\{[^}]*grid-template-columns:\s*repeat\(2,\s*minmax\(0,\s*1fr\)\)[^}]*\}[\s\S]*\.admin-users-actions\s+\.glass-button[^{]*\{[^}]*min-height:\s*44px/s);
  });

  it('1280px 起四项操作强制单行且每个目标至少 44px', () => {
    expect(css).toMatch(/@media\s*\(min-width:\s*1280px\)[\s\S]*\.admin-users-actions[^{]*\{[^}]*flex-wrap:\s*nowrap[^}]*\}[\s\S]*\.admin-users-actions\s+\.glass-button[^{]*\{[^}]*min-height:\s*44px[^}]*min-width:\s*44px[^}]*white-space:\s*nowrap/s);
  });

  it('已撤销 aria-disabled 使用现有中性 token', () => {
    const declarations = cssBlock('.admin-users-action-disabled[aria-disabled="true"]');
    expect(declarations).toMatch(/background:\s*var\(--surface-hover\)/);
    expect(declarations).toMatch(/border-color:\s*var\(--surface-border\)/);
    expect(declarations).toMatch(/color:\s*var\(--color-text\)/);
  });

  it.each([['浅色', cssBlock(':root')], ['深色', cssBlock('.dark')]])(
    '%s已撤销文字与中性背景对比度至少 4.5',
    (_theme, block) => {
      expect(contrast(token(block, '--color-text'), token(block, '--surface-hover'))).toBeGreaterThanOrEqual(4.5);
    },
  );

  it('Admin 搜索专用输入为前缀图标和清除按钮保留稳定内容区域', () => {
    const declarations = cssBlock('.admin-users-search-input');
    expect(declarations).toMatch(/padding-left:\s*2\.75rem/);
    expect(declarations).toMatch(/padding-right:\s*2\.75rem/);
  });

  it.each([['浅色', cssBlock(':root')], ['深色', cssBlock('.dark')]])(
    '%s危险按钮背景与文字对比度至少 4.5',
    (_theme, block) => {
      expect(contrast(token(block, '--danger-button-text'), token(block, '--danger-button-bg'))).toBeGreaterThanOrEqual(4.5);
    },
  );

  it('活动撤销使用实心危险按钮且不继承绿色背景', () => {
    const declarations = cssBlock('.admin-users-danger-button');
    expect(declarations).toMatch(/border-color:\s*var\(--danger-button-bg\)/);
    expect(declarations).toMatch(/background:\s*var\(--danger-button-bg\)/);
    expect(declarations).toMatch(/color:\s*var\(--danger-button-text\)/);
  });

  it('Pagination 所有交互按钮至少 44x44 且容器允许窄屏换行', () => {
    const nav = cssBlock('.pagination');
    const button = cssBlock('.pagination-button');
    expect(nav).toMatch(/max-width:\s*100%/);
    expect(nav).toMatch(/flex-wrap:\s*wrap/);
    expect(button).toMatch(/min-width:\s*44px/);
    expect(button).toMatch(/min-height:\s*44px/);
  });
});
