import { useEffect, useState, useCallback } from 'react';
import { useNavigate } from 'react-router-dom';
import { getFiles, deleteFile, getFileDownloadUrl } from '../api/files';
import { useAuthStore } from '../stores/auth';
import { useToastStore } from '../stores/toast';
import FileCard from '../components/FileCard';
import Pagination from '../components/Pagination';
import type { FileRecord } from '../types/api';

const PAGE_SIZE = 20;

export default function FileListPage() {
  const [files, setFiles] = useState<FileRecord[]>([]);
  const [total, setTotal] = useState(0);
  const [page, setPage] = useState(1);
  const [loading, setLoading] = useState(true);
  const user = useAuthStore((s) => s.user);
  const toast = useToastStore();
  const navigate = useNavigate();

  const fetch = useCallback(async (pageNum: number) => {
    setLoading(true);
    try {
      const offset = (pageNum - 1) * PAGE_SIZE;
      const res = await getFiles({ offset, limit: PAGE_SIZE });
      setFiles(res.items);
      setTotal(res.total);
    } catch {
      toast.error('加载文件列表失败');
    } finally {
      setLoading(false);
    }
  }, [toast]);

  useEffect(() => { fetch(page); }, [page, fetch]);

  const handleDownload = (id: number) => {
    getFileDownloadUrl(id).then((url) => {
      window.open(url, '_blank');
    });
  };

  const handleDelete = async (id: number) => {
    try {
      await deleteFile(id);
      toast.success('文件已删除');
      fetch(page);
    } catch {
      toast.error('删除失败');
    }
  };

  return (
    <div>
      <div className="flex items-center justify-between mb-6">
        <h1 className="text-xl font-display text-primary">文件列表</h1>
        <button onClick={() => navigate('/upload')} className="glass-button text-sm">上传文件</button>
      </div>

      {loading ? (
        <div className="text-center text-text-muted py-12">加载中...</div>
      ) : files.length === 0 ? (
        <div className="text-center text-text-muted py-12">暂无文件</div>
      ) : (
        <>
          <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-3 xl:grid-cols-4 gap-4">
            {files.map((f) => (
              <div key={f.file_id} onClick={() => navigate(`/files/${f.file_id}`)} className="cursor-pointer">
                <FileCard file={f} onDownload={handleDownload} onDelete={user?.role === 'VIP' ? handleDelete : undefined} />
              </div>
            ))}
          </div>
          <Pagination current={page} total={total} pageSize={PAGE_SIZE} onChange={setPage} />
        </>
      )}
    </div>
  );
}
