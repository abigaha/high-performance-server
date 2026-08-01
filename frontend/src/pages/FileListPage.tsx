import { useCallback, useEffect, useRef, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { getFiles, deleteFile, getFileDownloadUrl } from '../api/files';
import { captureSessionSnapshot, isSessionSnapshotCurrent } from '../api/client';
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
  const [name, setName] = useState('');
  const [type, setType] = useState('');
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [actionError, setActionError] = useState<string | null>(null);
  const [downloadingId, setDownloadingId] = useState<number | null>(null);
  const [deletingId, setDeletingId] = useState<number | null>(null);
  const loadRequestIdRef = useRef(0);
  const downloadControllerRef = useRef<AbortController | null>(null);
  const downloadOperationIdRef = useRef(0);
  const deleteOperationIdRef = useRef(0);
  const sessionScopeRef = useRef(0);
  const mountedRef = useRef(false);
  const sessionRevision = useAuthStore((state) => state.sessionRevision);
  const showSuccess = useToastStore((state) => state.success);
  const navigate = useNavigate();

  useEffect(() => {
    mountedRef.current = true;
    return () => {
      mountedRef.current = false;
      loadRequestIdRef.current += 1;
      downloadControllerRef.current?.abort();
      downloadControllerRef.current = null;
      downloadOperationIdRef.current += 1;
      deleteOperationIdRef.current += 1;
    };
  }, []);

  useEffect(() => {
    sessionScopeRef.current += 1;
    loadRequestIdRef.current += 1;
    downloadControllerRef.current?.abort();
    downloadControllerRef.current = null;
    downloadOperationIdRef.current += 1;
    deleteOperationIdRef.current += 1;
    setDownloadingId(null);
    setDeletingId(null);
    setActionError(null);
  }, [sessionRevision]);

  const loadFiles = useCallback(async (pageNum: number, signal?: AbortSignal) => {
    const requestId = ++loadRequestIdRef.current;
    const sessionScope = sessionScopeRef.current;
    const session = captureSessionSnapshot();
    const isCurrentRequest = () => mountedRef.current
      && requestId === loadRequestIdRef.current
      && sessionScope === sessionScopeRef.current
      && sessionRevision === useAuthStore.getState().sessionRevision
      && isSessionSnapshotCurrent(session);
    setLoading(true);
    setError(null);
    try {
      const offset = (pageNum - 1) * PAGE_SIZE;
      const res = await getFiles({ name: name || undefined, type: type || undefined, offset, limit: PAGE_SIZE }, signal);
      if (isCurrentRequest()) {
        setFiles(res.items);
        setTotal(res.total);
      }
    } catch (loadError) {
      if (isCurrentRequest() && !(loadError instanceof DOMException && loadError.name === 'AbortError')) {
        setError(errorMessage(loadError, '文件列表加载失败，请稍后重试'));
      }
    } finally {
      if (isCurrentRequest()) setLoading(false);
    }
  }, [name, sessionRevision, type]);

  useEffect(() => {
    const controller = new AbortController();
    void loadFiles(page, controller.signal);
    return () => {
      controller.abort();
      loadRequestIdRef.current += 1;
    };
  }, [page, loadFiles]);

  const handleDownload = async (id: number) => {
    if (downloadingId !== null) return;
    const controller = new AbortController();
    downloadControllerRef.current?.abort();
    downloadControllerRef.current = controller;
    const operationId = ++downloadOperationIdRef.current;
    const sessionScope = sessionScopeRef.current;
    const session = captureSessionSnapshot();
    const fileName = files.find((file) => file.file_id === id)?.file_name ?? `file-${id}`;
    const isCurrentOperation = () => mountedRef.current
      && operationId === downloadOperationIdRef.current
      && downloadControllerRef.current === controller
      && !controller.signal.aborted
      && sessionScope === sessionScopeRef.current
      && sessionRevision === useAuthStore.getState().sessionRevision
      && isSessionSnapshotCurrent(session);
    setActionError(null);
    setDownloadingId(id);
    try {
      const url = await getFileDownloadUrl(id, controller.signal);
      if (!isCurrentOperation()) return;
      triggerDownload(url, fileName);
    } catch (downloadError) {
      if (isCurrentOperation()) setActionError(errorMessage(downloadError, '文件下载失败，请稍后重试'));
    } finally {
      if (isCurrentOperation()) {
        downloadControllerRef.current = null;
        setDownloadingId((current) => current === id ? null : current);
      }
    }
  };

  const handleDelete = async (id: number) => {
    if (deletingId !== null) return;
    const target = files.find((file) => file.file_id === id);
    if (!window.confirm(`确定删除“${target?.file_name ?? `文件 ${id}`}”吗？此操作无法撤销。`)) return;

    setActionError(null);
    setDeletingId(id);
    const operationId = ++deleteOperationIdRef.current;
    const loadRequestId = loadRequestIdRef.current;
    const sessionScope = sessionScopeRef.current;
    const session = captureSessionSnapshot();
    const isCurrentOperation = () => mountedRef.current
      && operationId === deleteOperationIdRef.current
      && sessionScope === sessionScopeRef.current
      && sessionRevision === useAuthStore.getState().sessionRevision
      && isSessionSnapshotCurrent(session);
    const isCurrentView = () => isCurrentOperation() && loadRequestId === loadRequestIdRef.current;
    try {
      await deleteFile(id);
      if (!isCurrentView()) return;
      showSuccess('文件已删除');
      if (files.length === 1 && page > 1) {
        setPage((current) => current - 1);
      } else {
        await loadFiles(page);
      }
    } catch (deleteError) {
      if (isCurrentView()) setActionError(errorMessage(deleteError, '文件删除失败，请稍后重试'));
    } finally {
      if (isCurrentOperation()) {
        setDeletingId((current) => current === id ? null : current);
      }
    }
  };

  return (
    <div className="min-w-0">
      <div className="flex flex-wrap items-center justify-between gap-3 mb-6">
        <h1 className="text-xl font-display text-primary">文件列表</h1>
        <button type="button" onClick={() => navigate('/upload')} className="glass-button min-h-11 min-w-11 text-sm">上传文件</button>
      </div>

      {actionError && <p role="alert" className="mb-4 text-sm text-destructive">{actionError}</p>}

      <div className="mb-5 grid gap-3 sm:grid-cols-2">
        <label className="grid gap-1 text-sm">文件名称<input className="glass-input min-h-11" value={name} onChange={(event) => { setName(event.target.value); setPage(1); }} /></label>
        <label className="grid gap-1 text-sm">文件类型<select className="glass-input min-h-11" value={type} onChange={(event) => { setType(event.target.value); setPage(1); }}><option value="">全部类型</option><option value="audio">音频</option><option value="image">图片</option><option value="video">视频</option><option value="other">其他</option></select></label>
      </div>

      {loading ? (
        <section role="status" className="flex min-h-64 items-center justify-center text-center text-text-muted">
          正在加载文件...
        </section>
      ) : error ? (
        <section role="alert" className="flex min-h-64 flex-col items-center justify-center gap-4 text-center">
          <p className="text-sm text-destructive">{error}</p>
          <button type="button" onClick={() => void loadFiles(page)} className="glass-button min-h-11 min-w-11 text-sm">重试</button>
        </section>
      ) : files.length === 0 ? (
        <section className="flex min-h-64 flex-col items-center justify-center gap-4 text-center text-text-muted">
          <p>暂无文件</p>
          <button type="button" onClick={() => navigate('/upload')} className="glass-button min-h-11 min-w-11 text-sm">上传第一个文件</button>
        </section>
      ) : (
        <>
          <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-3 xl:grid-cols-4 gap-4">
            {files.map((f) => (
              <article key={f.file_id}>
                <FileCard
                  file={f}
                  onDownload={handleDownload}
                  onDelete={f.can_delete ? handleDelete : undefined}
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
