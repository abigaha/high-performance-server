import { render, screen } from '@testing-library/react';
import { MemoryRouter, Route, Routes } from 'react-router-dom';
import { describe, expect, it } from 'vitest';
import GuestLayout from '../../src/components/GuestLayout';

describe('GuestLayout', () => {
  it('展示访客入口、品牌、主题按钮和子页面', () => {
    render(
      <MemoryRouter initialEntries={['/login']}>
        <Routes>
          <Route element={<GuestLayout />}>
            <Route path="/login" element={<div>登录表单</div>} />
          </Route>
        </Routes>
      </MemoryRouter>,
    );

    expect(screen.getByRole('main', { name: '访客入口' })).toBeInTheDocument();
    expect(screen.getByLabelText('Crystal Music')).toHaveTextContent('Crystal Music');
    expect(screen.getByRole('button', { name: /切换到.*主题/ })).toBeInTheDocument();
    expect(screen.getByText('登录表单')).toBeVisible();
  });
});
