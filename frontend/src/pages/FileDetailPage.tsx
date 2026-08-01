import { useCallback, useEffect, useRef, useState } from 'react';
import { useParams, useNavigate } from 'react-router-dom';
import { getFile, deleteFile, getFileDownloadUrl } from '../api/files';
import { captureSessionSnapshot, isSessionSnapshotCurrent } from '../api/client';
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
  const [error, setError] = useState<string | null>(null);
  const [actionError, setActionError] = useState<string | null>(null);
  const [downloading, setDownloading] = useState(false);
  const [deleting, setDeleting] = useState(false);
  const requestIdRef = useRef(0);
  const downloadRequestIdRef = useRef(0);
  const downloadControllerRef = useRef<AbortController | null>(null);
  const sessionRevision = useAuthStore((state) => state.sessionRevision);
  const showSuccess = useToastStore((state) => state.success);
  const navigate = useNavigate();

  const loadFile = useCallback(async () => {
    const fileId = Number(id);
    if (!Number.isSafeInteger(fileId) || fileId <= 0) {
      setFile(null);
      setError('文件编号无效');
      setLoading(false);
      return;
    }

    const requestId = ++requestIdRef.current;
    const session = captureSessionSnapshot();
    setLoading(true);
    setError(null);
    try {
      const record = await getFile(fileId);
      if (requestId === requestIdRef.current && isSessionSnapshotCurrent(session)) setFile(record);
    } catch (loadError) {
      if (requestId === requestIdRef.current && isSessionSnapshotCurrent(session)) {
        setFile(null);
        setError(errorMessage(loadError, '文件详情加载失败，请稍后重试'));
      }
    } finally {
      if (requestId === requestIdRef.current && isSessionSnapshotCurrent(session)) setLoading(false);
    }
  }, [id]);

  useEffect(() => {
    void loadFile();
    return () => {
      requestIdRef.current += 1;
      downloadRequestIdRef.current += 1;
      downloadControllerRef.current?.abort();
      downloadControllerRef.current = null;
    };
  }, [loadFile]);

  useEffect(() => {
    downloadRequestIdRef.current += 1;
    downloadControllerRef.current?.abort();
    downloadControllerRef.current = null;
    setDownloading(false);
  }, [id, sessionRevision]);

  const handleDownload = async () => {
    if (!file || downloading) return;
    const requestId = ++downloadRequestIdRef.current;
    downloadControllerRef.current?.abort();
    const controller = new AbortController();
    downloadControllerRef.current = controller;
    const session = captureSessionSnapshot();
    const isCurrent = () => requestId === downloadRequestIdRef.current
      && downloadControllerRef.current === controller
      && !controller.signal.aborted
      && isSessionSnapshotCurrent(session);
    setActionError(null);
    setDownloading(true);
    try {
      const url = await getFileDownloadUrl(file.file_id, controller.signal);
      if (isCurrent()) triggerDownload(url, file.file_name);
    } catch (downloadError) {
      if (isCurrent()) {
        setActionError(errorMessage(downloadError, '文件下载失败，请稍后重试'));
      }
    } finally {
      if (requestId === downloadRequestIdRef.current && downloadControllerRef.current === controller) {
        downloadControllerRef.current = null;
        if (isSessionSnapshotCurrent(session) && !controller.signal.aborted) setDownloading(false);
      }
    }
  };

  const handleDelete = async () => {
    if (!file || !file.can_delete || deleting) return;
    if (!window.confirm(`确定删除“${file.file_name}”吗？此操作无法撤销。`)) return;

    const session = captureSessionSnapshot();
    setActionError(null);
    setDeleting(true);
    try {
      await deleteFile(file.file_id);
      if (!isSessionSnapshotCurrent(session)) return;
      showSuccess('文件已删除');
      navigate('/files');
    } catch (deleteError) {
      if (isSessionSnapshotCurrent(session)) {
        setActionError(errorMessage(deleteError, '文件删除失败，请稍后重试'));
      }
    } finally {
      if (isSessionSnapshotCurrent(session)) setDeleting(false);
    }
  };

  if (loading) {
    return (
      <section role="status" className="flex min-h-64 items-center justify-center text-center text-text-muted">
        正在加载文件详情...
      </section>
    );
  }
  if (error || !file) {
    return (
      <section role="alert" className="flex min-h-64 flex-col items-center justify-center gap-4 text-center">
        <p className="text-sm text-destructive">{error ?? '文件不存在'}</p>
        <div className="flex flex-wrap justify-center gap-3">
          <button type="button" onClick={() => void loadFile()} className="glass-button min-h-11 min-w-11 text-sm">重试</button>
          <button type="button" onClick={() => navigate('/files')} className="glass-button min-h-11 min-w-11 text-sm !bg-transparent !text-text">返回列表</button>
        </div>
      </section>
    );
  }

  return (
    <div className="max-w-lg mx-auto">
      <div className="glass-card p-4 sm:p-6 flex flex-col gap-4">
        <h1 className="text-lg font-display text-primary">文件详情</h1>
        <dl className="grid grid-cols-[minmax(0,7rem)_minmax(0,1fr)] gap-3 text-sm">
          <dt className="text-text-muted">文件名</dt><dd className="break-words">{file.file_name}</dd>
          <dt className="text-text-muted">大小</dt><dd>{formatSize(file.file_size)}</dd>
          <dt className="text-text-muted">类型</dt><dd className="break-words">{file.content_type}</dd>
          <dt className="text-text-muted">哈希</dt><dd className="font-mono text-xs break-all">{file.file_hash}</dd>
          <dt className="text-text-muted">上传时间</dt><dd>{new Date(file.created_at).toLocaleString('zh-CN')}</dd>
        </dl>
        {actionError && <p role="alert" className="text-sm text-destructive">{actionError}</p>}
        <div className="flex flex-col sm:flex-row gap-3 pt-2">
          <button
            type="button"
            aria-busy={downloading}
            disabled={downloading || deleting}
            onClick={() => void handleDownload()}
            className="glass-button min-h-11 min-w-24 flex-1"
          >
            {downloading ? '准备下载...' : '下载'}
          </button>
          {file.can_delete && (
            <button
              type="button"
              aria-busy={deleting}
              disabled={deleting || downloading}
              onClick={() => void handleDelete()}
              className="glass-button min-h-11 min-w-24 flex-1 !bg-red-500/80"
            >
              {deleting ? '删除中...' : '删除'}
            </button>
          )}
        </div>
      </div>
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
