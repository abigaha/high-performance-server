import { ArrowRight, Shield } from '@phosphor-icons/react';
import { useNavigate } from 'react-router-dom';

export default function UserManagePage() {
  const navigate = useNavigate();

  return (
    <div className="mx-auto max-w-2xl">
      <div className="mb-6 flex flex-wrap items-center gap-2">
        <Shield size={24} className="text-primary" />
        <h1 className="text-xl font-display text-primary">用户管理</h1>
        <span className="rounded-full bg-amber-500/20 px-2 py-0.5 text-xs text-amber-700 dark:text-amber-300">VIP 专属</span>
      </div>

      <section
        className="glass-card relative overflow-hidden p-5 pl-6 before:absolute before:inset-y-0 before:left-0 before:w-1 before:bg-primary sm:p-6 sm:pl-7"
        aria-labelledby="user-management-status"
      >
        <h2 id="user-management-status" className="mb-2 text-base font-medium">暂无用户目录</h2>
        <p className="text-sm leading-6 text-text-muted">
          服务端当前未开放用户列表和管理接口，此页面不会使用文件数据推断用户状态。
        </p>
        <button
          type="button"
          onClick={() => navigate('/files')}
          className="glass-button mt-5 flex items-center gap-2 text-sm"
        >
          返回文件列表 <ArrowRight size={16} />
        </button>
      </section>
    </div>
  );
}
