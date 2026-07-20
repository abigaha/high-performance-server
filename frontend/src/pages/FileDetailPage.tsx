import { useEffect, useState } from 'react';
import { useParams, useNavigate } from 'react-router-dom';
import { getFile, deleteFile, getFileDownloadUrl } from '../api/files';
import { useAuthStore } from '../stores/auth';
import { useToastStore } from '../stores/toast';
import type { FileRecord } from '../types/api';

function formatSize(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

export default function FileDetailPage() {
  const { id } = useParams<{ id: string }>();
  const [file, setFile] = useState<FileRecord | null>(null);
  const [loading, setLoading] = useState(true);
  const user = useAuthStore((s) => s.user);
  const toast = useToastStore();
  const navigate = useNavigate();

  useEffect(() => {
    if (!id) return;
    setLoading(true);
    getFile(Number(id))
      .then(setFile)
      .catch(() => toast.error('加载文件详情失败'))
      .finally(() => setLoading(false));
  }, [id, toast]);

  const handleDownload = () => {
    if (!file) return;
    getFileDownloadUrl(file.file_id).then((url) => window.open(url, '_blank'));
  };

  const handleDelete = async () => {
    if (!file) return;
    try {
      await deleteFile(file.file_id);
      toast.success('文件已删除');
      navigate('/files');
    } catch {
      toast.error('删除失败');
    }
  };

  if (loading) return <div className="text-center text-text-muted py-12">加载中...</div>;
  if (!file) return <div className="text-center text-text-muted py-12">文件不存在</div>;

  return (
    <div className="max-w-lg mx-auto">
      <div className="glass-card p-6 flex flex-col gap-4">
        <h1 className="text-lg font-display text-primary">文件详情</h1>
        <div className="grid grid-cols-2 gap-3 text-sm">
          <span className="text-text-muted">文件名</span><span>{file.file_name}</span>
          <span className="text-text-muted">大小</span><span>{formatSize(file.file_size)}</span>
          <span className="text-text-muted">类型</span><span>{file.content_type}</span>
          <span className="text-text-muted">哈希</span><span className="font-mono text-xs truncate">{file.file_hash}</span>
          <span className="text-text-muted">上传时间</span><span>{new Date(file.created_at).toLocaleString('zh-CN')}</span>
        </div>
        <div className="flex gap-3 pt-2">
          <button onClick={handleDownload} className="glass-button flex-1">下载</button>
          {user?.role === 'VIP' && (
            <button onClick={handleDelete} className="glass-button flex-1 !bg-red-500/80">删除</button>
          )}
        </div>
      </div>
    </div>
  );
}
