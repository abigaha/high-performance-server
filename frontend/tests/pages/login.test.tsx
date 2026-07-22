import { act, render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { MemoryRouter } from 'react-router-dom';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import LoginPage from '../../src/pages/LoginPage';
import { useAuthStore } from '../../src/stores/auth';

const loginMock = vi.fn();

beforeEach(() => {
  loginMock.mockReset();
  useAuthStore.setState({ login: loginMock });
});

async function submitLogin() {
  const user = userEvent.setup();
  render(
    <MemoryRouter>
      <LoginPage />
    </MemoryRouter>,
  );

  await user.type(screen.getByLabelText('用户名'), 'testuser');
  await user.type(screen.getByLabelText('密码'), 'pass123');
  await user.click(screen.getByRole('button', { name: '登录' }));
}

describe('登录页', () => {
  it('用户名不足 2 个字符时阻止登录', async () => {
    const user = userEvent.setup();
    render(
      <MemoryRouter>
        <LoginPage />
      </MemoryRouter>,
    );

    await user.type(screen.getByLabelText('用户名'), 'a');
    await user.type(screen.getByLabelText('密码'), '123456');
    await user.click(screen.getByRole('button', { name: '登录' }));

    expect(screen.getByRole('alert')).toHaveTextContent('用户名至少 2 个字符');
    expect(loginMock).not.toHaveBeenCalled();
  });

  it('密码不足 6 个字符时阻止登录', async () => {
    const user = userEvent.setup();
    render(
      <MemoryRouter>
        <LoginPage />
      </MemoryRouter>,
    );

    await user.type(screen.getByLabelText('用户名'), 'ab');
    await user.type(screen.getByLabelText('密码'), '12345');
    await user.click(screen.getByRole('button', { name: '登录' }));

    expect(screen.getByRole('alert')).toHaveTextContent('密码至少 6 个字符');
    expect(loginMock).not.toHaveBeenCalled();
  });

  it('显示 API 返回的详细错误', async () => {
    loginMock.mockRejectedValueOnce(new Error('用户名或密码错误'));

    await submitLogin();

    expect(await screen.findByRole('alert')).toHaveTextContent('用户名或密码错误');
    expect(loginMock).toHaveBeenCalledWith('testuser', 'pass123');
  });

  it('非 Error 异常时显示通用错误', async () => {
    loginMock.mockRejectedValueOnce('未知异常');

    await submitLogin();

    expect(await screen.findByRole('alert')).toHaveTextContent('登录失败，请检查用户名和密码');
  });

  it('输入框具有关联标签、最小长度和登录自动填充语义', () => {
    render(
      <MemoryRouter>
        <LoginPage />
      </MemoryRouter>,
    );

    expect(screen.getByLabelText('用户名')).toHaveAttribute('autocomplete', 'username');
    expect(screen.getByLabelText('用户名')).toHaveAttribute('minlength', '2');
    expect(screen.getByLabelText('密码')).toHaveAttribute('autocomplete', 'current-password');
    expect(screen.getByLabelText('密码')).toHaveAttribute('minlength', '6');
  });

  it('登录请求进行中禁用表单，结束后恢复', async () => {
    let rejectRequest: ((reason?: unknown) => void) | undefined;
    loginMock.mockImplementationOnce(() => new Promise<void>((_, reject) => {
      rejectRequest = reject;
    }));

    await submitLogin();

    expect(screen.getByRole('button', { name: '登录中...' })).toBeDisabled();
    expect(screen.getByLabelText('用户名')).toBeDisabled();
    expect(screen.getByLabelText('密码')).toBeDisabled();

    await act(async () => rejectRequest?.(new Error('登录服务繁忙')));
    expect(await screen.findByRole('button', { name: '登录' })).toBeEnabled();
  });
});
