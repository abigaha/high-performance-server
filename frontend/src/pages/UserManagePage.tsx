import { useEffect, useState } from 'react';
import { getFiles } from '../api/files';
import { useToastStore } from '../stores/toast';
import { Shield } from '@phosphor-icons/react';
import type { FileRecord } from '../types/api';

export default function UserManagePage() {
  const [files, setFiles] = useState<FileRecord[]>([]);
  const toast = useToastStore();

  useEffect(() => {
    getFiles({ limit: 50 })
      .then((res) => setFiles(res.items))
      .catch(() => toast.error('加载数据失败'));
  }, [toast]);

  return (
    <div>
      <div className="flex items-center gap-2 mb-6">
        <Shield size={24} className="text-primary" />
        <h1 className="text-xl font-display text-primary">用户管理</h1>
        <span className="text-xs bg-amber-500/20 text-amber-400 px-2 py-0.5 rounded-full">VIP 专属</span>
      </div>

      <div className="glass-card p-6">
        <p className="text-sm text-text-muted mb-4">用户管理功能开发中...</p>
        <div className="text-sm">
          <p className="text-text-muted mb-2">系统文件总数: <span className="text-text font-medium">{files.length}</span></p>
        </div>
      </div>
    </div>
  );
}
