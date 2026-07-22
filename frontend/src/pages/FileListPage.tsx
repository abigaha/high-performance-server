import { useCallback, useEffect, useRef, useState } from 'react';
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
  const [error, setError] = useState<string | null>(null);
  const [actionError, setActionError] = useState<string | null>(null);
  const [downloadingId, setDownloadingId] = useState<number | null>(null);
  const [deletingId, setDeletingId] = useState<number | null>(null);
  const requestIdRef = useRef(0);
  const user = useAuthStore((s) => s.user);
  const showSuccess = useToastStore((state) => state.success);
  const navigate = useNavigate();

  const loadFiles = useCallback(async (pageNum: number) => {
    const requestId = ++requestIdRef.current;
    setLoading(true);
    setError(null);
    try {
      const offset = (pageNum - 1) * PAGE_SIZE;
      const res = await getFiles({ offset, limit: PAGE_SIZE });
      if (requestId === requestIdRef.current) {
        setFiles(res.items);
        setTotal(res.total);
      }
    } catch (loadError) {
      if (requestId === requestIdRef.current) {
        setError(errorMessage(loadError, '文件列表加载失败，请稍后重试'));
      }
    } finally {
      if (requestId === requestIdRef.current) setLoading(false);
    }
  }, []);

  useEffect(() => {
    void loadFiles(page);
    return () => {
      requestIdRef.current += 1;
    };
  }, [page, loadFiles]);

  const handleDownload = async (id: number) => {
    if (downloadingId !== null) return;
    setActionError(null);
    setDownloadingId(id);
    try {
      const url = await getFileDownloadUrl(id);
      const fileName = files.find((file) => file.file_id === id)?.file_name ?? `file-${id}`;
      triggerDownload(url, fileName);
    } catch (downloadError) {
      setActionError(errorMessage(downloadError, '文件下载失败，请稍后重试'));
    } finally {
      setDownloadingId(null);
    }
  };

  const handleDelete = async (id: number) => {
    if (deletingId !== null) return;
    const target = files.find((file) => file.file_id === id);
    if (!window.confirm(`确定删除“${target?.file_name ?? `文件 ${id}`}”吗？此操作无法撤销。`)) return;

    setActionError(null);
    setDeletingId(id);
    try {
      await deleteFile(id);
      showSuccess('文件已删除');
      if (files.length === 1 && page > 1) {
        setPage((current) => current - 1);
      } else {
        await loadFiles(page);
      }
    } catch (deleteError) {
      setActionError(errorMessage(deleteError, '文件删除失败，请稍后重试'));
    } finally {
      setDeletingId(null);
    }
  };

  return (
    <div className="min-w-0">
      <div className="flex flex-wrap items-center justify-between gap-3 mb-6">
        <h1 className="text-xl font-display text-primary">文件列表</h1>
        <button type="button" onClick={() => navigate('/upload')} className="glass-button text-sm">上传文件</button>
      </div>

      {actionError && <p role="alert" className="mb-4 text-sm text-destructive">{actionError}</p>}

      {loading ? (
        <div role="status" className="text-center text-text-muted py-12">正在加载文件...</div>
      ) : error ? (
        <div role="alert" className="text-center py-12">
          <p className="text-sm text-destructive mb-4">{error}</p>
          <button type="button" onClick={() => void loadFiles(page)} className="glass-button text-sm">重试</button>
        </div>
      ) : files.length === 0 ? (
        <div className="text-center text-text-muted py-12">
          <p className="mb-4">暂无文件</p>
          <button type="button" onClick={() => navigate('/upload')} className="glass-button text-sm">上传第一个文件</button>
        </div>
      ) : (
        <>
          <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-3 xl:grid-cols-4 gap-4">
            {files.map((f) => (
              <article
                key={f.file_id}
                role="link"
                tabIndex={0}
                aria-label={`查看 ${f.file_name} 的详情`}
                onClick={() => navigate(`/files/${f.file_id}`)}
                onKeyDown={(event) => {
                  if (event.key === 'Enter') navigate(`/files/${f.file_id}`);
                }}
                className="cursor-pointer focus-visible:outline-2 focus-visible:outline-primary focus-visible:outline-offset-2"
              >
                <FileCard
                  file={f}
                  onDownload={handleDownload}
                  onDelete={user?.role === 'VIP' ? handleDelete : undefined}
                  downloading={downloadingId === f.file_id}
                  deleting={deletingId === f.file_id}
                />
              </article>
            ))}
          </div>
          <Pagination current={page} total={total} pageSize={PAGE_SIZE} onChange={setPage} />
        </>
      )}
    </div>
  );
}

function errorMessage(error: unknown, fallback: string): string {
  return error instanceof Error && error.message ? error.message : fallback;
}

function triggerDownload(url: string, fileName: string): void {
  const anchor = document.createElement('a');
  anchor.href = url;
  anchor.download = fileName;
  anchor.rel = 'noopener';
  document.body.append(anchor);
  anchor.click();
  anchor.remove();
}
