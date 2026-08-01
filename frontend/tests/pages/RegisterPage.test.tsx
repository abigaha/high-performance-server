import { StrictMode, useState } from 'react';
import { act, cleanup, render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { MemoryRouter, useLocation } from 'react-router-dom';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import RegisterPage from '../../src/pages/RegisterPage';

const mocks = vi.hoisted(() => ({
  register: vi.fn(),
}));

vi.mock('../../src/stores/auth', () => ({
  useAuthStore: (selector: (state: { register: typeof mocks.register }) => unknown) =>
    selector({ register: mocks.register }),
}));

async function submitRegistration(username: string, password: string) {
  const user = userEvent.setup();
  await user.type(screen.getByLabelText('用户名'), username);
  await user.type(screen.getByLabelText('邮箱'), 'user@example.com');
  await user.type(screen.getByLabelText('密码'), password);
  await user.click(screen.getByRole('button', { name: '注册' }));
}

function LocationDisplay() {
  const location = useLocation();
  return <output data-testid="location">{`${location.pathname}${location.search}${location.hash}`}</output>;
}

function RegisterUnmountHarness() {
  const [mounted, setMounted] = useState(true);

  return (
    <>
      <button type="button" onClick={() => setMounted(false)}>卸载注册页</button>
      {mounted && <RegisterPage />}
      <LocationDisplay />
    </>
  );
}

describe('RegisterPage', () => {
  beforeEach(() => {
    mocks.register.mockReset();
    render(
      <StrictMode>
        <MemoryRouter>
          <RegisterPage />
        </MemoryRouter>
      </StrictMode>,
    );
  });

  it('保留注册字段、唯一提交按钮和警告区域', async () => {
    const user = userEvent.setup();

    expect(screen.getByLabelText('用户名')).toBeRequired();
    expect(screen.getByLabelText('邮箱')).toBeRequired();
    expect(screen.getByLabelText('密码')).toBeRequired();
    expect(screen.getAllByRole('button', { name: '注册' })).toHaveLength(1);

    await user.click(screen.getByRole('button', { name: '注册' }));
    expect(screen.getByRole('alert')).toHaveTextContent('用户名至少 2 个字符');
  });

  it('用户名不足 2 个字符时阻止注册', async () => {
    await submitRegistration('a', '123456');

    expect(screen.getByRole('alert')).toHaveTextContent('用户名至少 2 个字符');
    expect(mocks.register).not.toHaveBeenCalled();
  });

  it('密码不足 6 个字符时阻止注册', async () => {
    await submitRegistration('ab', '12345');

    expect(screen.getByRole('alert')).toHaveTextContent('密码至少 6 个字符');
    expect(mocks.register).not.toHaveBeenCalled();
  });

  it('注册接口失败时展示后端详细错误', async () => {
    mocks.register.mockRejectedValueOnce(new Error('用户名已存在'));

    await submitRegistration('ab', '123456');

    expect(await screen.findByRole('alert')).toHaveTextContent('用户名已存在');
    expect(mocks.register).toHaveBeenCalledWith('ab', '123456', 'user@example.com');
  });

  it('注册错误消息为空时展示通用提示', async () => {
    mocks.register.mockRejectedValueOnce(new Error(''));

    await submitRegistration('ab', '123456');

    expect(await screen.findByRole('alert')).toHaveTextContent('注册失败，请重试');
  });

  it('StrictMode 重放后成功注册导航到文件页', async () => {
    cleanup();
    mocks.register.mockResolvedValueOnce(true);
    render(
      <StrictMode>
        <MemoryRouter initialEntries={['/register']}>
          <RegisterPage />
          <LocationDisplay />
        </MemoryRouter>
      </StrictMode>,
    );

    await submitRegistration('testuser', '123456');

    expect(await screen.findByTestId('location')).toHaveTextContent('/files');
  });

  it('输入框具有关联标签、最小长度和注册自动填充语义', () => {
    expect(screen.getByLabelText('用户名')).toHaveAttribute('autocomplete', 'username');
    expect(screen.getByLabelText('用户名')).toHaveAttribute('minlength', '2');
    expect(screen.getByLabelText('邮箱')).toHaveAttribute('autocomplete', 'email');
    expect(screen.getByLabelText('密码')).toHaveAttribute('autocomplete', 'new-password');
    expect(screen.getByLabelText('密码')).toHaveAttribute('minlength', '6');
  });

  it('注册请求进行中禁用表单，结束后恢复', async () => {
    let rejectRequest: ((reason?: unknown) => void) | undefined;
    mocks.register.mockImplementationOnce(() => new Promise<void>((_, reject) => {
      rejectRequest = reject;
    }));

    await submitRegistration('testuser', '123456');

    expect(screen.getByRole('button', { name: '注册中...' })).toBeDisabled();
    expect(screen.getByLabelText('用户名')).toBeDisabled();
    expect(screen.getByLabelText('邮箱')).toBeDisabled();
    expect(screen.getByLabelText('密码')).toBeDisabled();

    await act(async () => rejectRequest?.(new Error('注册服务繁忙')));
    expect(await screen.findByRole('button', { name: '注册' })).toBeEnabled();
  });

  it('卸载后使旧注册结果保持惰性', async () => {
    cleanup();
    let resolveRegistration: ((value: boolean) => void) | undefined;
    mocks.register.mockImplementationOnce(() => new Promise<boolean>((resolve) => {
      resolveRegistration = resolve;
    }));
    render(
      <MemoryRouter initialEntries={['/register']}>
        <RegisterUnmountHarness />
      </MemoryRouter>,
    );

    await submitRegistration('testuser', '123456');
    const user = userEvent.setup();
    await user.click(screen.getByRole('button', { name: '卸载注册页' }));
    await act(async () => resolveRegistration?.(true));

    expect(screen.getByTestId('location')).toHaveTextContent('/register');
  });
});
