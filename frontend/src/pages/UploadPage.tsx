import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import type { ChangeEvent, DragEvent } from 'react';
import {
  ArrowClockwise,
  CloudArrowUp,
  FileAudio,
  Trash,
  X,
} from '@phosphor-icons/react';
import { uploadFile } from '../api/files';
import {
  AUDIO_ACCEPT,
  AUDIO_EXTENSIONS,
  formatFileSize,
  getUploadLimit,
  validateUploadFile,
} from '../lib/uploadPolicy';
import { useAuthStore } from '../stores/auth';
import { useToastStore } from '../stores/toast';
import type { UploadResult } from '../types/api';

type UploadStatus = 'queued' | 'uploading' | 'done' | 'error' | 'cancelled';

interface UploadItem {
  id: string;
  batchId: string;
  file: File;
  progress: number;
  status: UploadStatus;
  error?: string;
  result?: UploadResult;
}

const UPLOAD_CONCURRENCY = 2;
const TERMINAL_STATUSES: UploadStatus[] = ['done', 'error', 'cancelled'];

function isTerminal(status: UploadStatus): boolean {
  return TERMINAL_STATUSES.includes(status);
}

function statusText(item: UploadItem): string {
  if (item.status === 'queued') return '等待上传';
  if (item.status === 'uploading') return `上传中 ${item.progress}%`;
  if (item.status === 'done') return item.result?.exists ? '文件已存在' : '上传成功';
  if (item.status === 'cancelled') return '已取消';
  return '上传失败';
}

export default function UploadPage() {
  const [items, setItems] = useState<UploadItem[]>([]);
  const [dragActive, setDragActive] = useState(false);
  const inputRef = useRef<HTMLInputElement>(null);
  const sequenceRef = useRef(0);
  const dragDepthRef = useRef(0);
  const controllersRef = useRef(new Map<string, AbortController>());
  const reportedBatchesRef = useRef(new Set<string>());
  const role = useAuthStore((state) => state.user?.role);
  const showSuccess = useToastStore((state) => state.success);
  const showError = useToastStore((state) => state.error);
  const showInfo = useToastStore((state) => state.info);

  const nextId = useCallback((prefix: string) => {
    sequenceRef.current += 1;
    return `${prefix}-${Date.now()}-${sequenceRef.current}`;
  }, []);

  const addFiles = useCallback((files: File[]) => {
    if (files.length === 0) return;

    const batchId = nextId('batch');
    const additions = files.map<UploadItem>((file) => {
      const validation = validateUploadFile(file, role);
      return {
        id: nextId('file'),
        batchId,
        file,
        progress: 0,
        status: validation.valid ? 'queued' : 'error',
        error: validation.error,
      };
    });
    setItems((current) => [...current, ...additions]);
  }, [nextId, role]);

  const startOne = useCallback((item: UploadItem) => {
    if (controllersRef.current.has(item.id)) return;

    const controller = new AbortController();
    controllersRef.current.set(item.id, controller);
    setItems((current) => current.map((candidate) => (
      candidate.id === item.id && candidate.status === 'queued'
        ? { ...candidate, status: 'uploading' }
        : candidate
    )));

    void uploadFile(
      item.file,
      (progress) => {
        setItems((current) => current.map((candidate) => (
          candidate.id === item.id && candidate.status === 'uploading'
            ? { ...candidate, progress }
            : candidate
        )));
      },
      controller.signal,
    ).then((result) => {
      setItems((current) => current.map((candidate) => (
        candidate.id === item.id
          ? { ...candidate, progress: 100, status: 'done', result, error: undefined }
          : candidate
      )));
    }).catch((error: unknown) => {
      const cancelled = error instanceof DOMException && error.name === 'AbortError';
      const message = error instanceof Error && error.message.trim()
        ? error.message
        : '上传失败，请重试';
      setItems((current) => current.map((candidate) => (
        candidate.id === item.id
          ? {
              ...candidate,
              status: cancelled ? 'cancelled' : 'error',
              error: cancelled ? '上传已取消' : message,
            }
          : candidate
      )));
    }).finally(() => {
      controllersRef.current.delete(item.id);
    });
  }, []);

  useEffect(() => {
    const freeSlots = UPLOAD_CONCURRENCY - controllersRef.current.size;
    if (freeSlots <= 0) return;

    items
      .filter((item) => item.status === 'queued' && !controllersRef.current.has(item.id))
      .slice(0, freeSlots)
      .forEach(startOne);
  }, [items, startOne]);

  useEffect(() => {
    const batchIds = new Set(items.map((item) => item.batchId));
    batchIds.forEach((batchId) => {
      if (reportedBatchesRef.current.has(batchId)) return;

      const batchItems = items.filter((item) => item.batchId === batchId);
      if (batchItems.length === 0 || !batchItems.every((item) => isTerminal(item.status))) return;

      reportedBatchesRef.current.add(batchId);
      const succeeded = batchItems.filter((item) => item.status === 'done').length;
      const failed = batchItems.filter((item) => item.status === 'error').length;
      const cancelled = batchItems.filter((item) => item.status === 'cancelled').length;

      if (succeeded === batchItems.length) {
        showSuccess(`${succeeded} 个文件上传成功`);
      } else if (failed === batchItems.length) {
        showError(`${failed} 个文件未能上传，请查看队列中的详细原因`);
      } else {
        showInfo(`上传结束：${succeeded} 成功，${failed} 失败，${cancelled} 取消`);
      }
    });
  }, [items, showError, showInfo, showSuccess]);

  useEffect(() => () => {
    controllersRef.current.forEach((controller) => controller.abort());
    controllersRef.current.clear();
  }, []);

  const handleFileChange = (event: ChangeEvent<HTMLInputElement>) => {
    addFiles(Array.from(event.target.files ?? []));
    event.target.value = '';
  };

  const handleDragEnter = (event: DragEvent<HTMLButtonElement>) => {
    event.preventDefault();
    dragDepthRef.current += 1;
    setDragActive(true);
  };

  const handleDragLeave = (event: DragEvent<HTMLButtonElement>) => {
    event.preventDefault();
    dragDepthRef.current = Math.max(0, dragDepthRef.current - 1);
    if (dragDepthRef.current === 0) setDragActive(false);
  };

  const handleDrop = (event: DragEvent<HTMLButtonElement>) => {
    event.preventDefault();
    dragDepthRef.current = 0;
    setDragActive(false);
    addFiles(Array.from(event.dataTransfer.files));
  };

  const cancelItem = (id: string) => {
    const controller = controllersRef.current.get(id);
    if (controller) {
      controller.abort();
      return;
    }
    setItems((current) => current.map((item) => (
      item.id === id && item.status === 'queued'
        ? { ...item, status: 'cancelled', error: '上传已取消' }
        : item
    )));
  };

  const retryItem = (id: string) => {
    const batchId = nextId('batch');
    setItems((current) => current.map((item) => {
      if (item.id !== id) return item;

      const validation = validateUploadFile(item.file, role);
      return {
        ...item,
        batchId,
        progress: 0,
        status: validation.valid ? 'queued' : 'error',
        error: validation.error,
        result: undefined,
      };
    }));
  };

  const removeItem = (id: string) => {
    controllersRef.current.get(id)?.abort();
    setItems((current) => current.filter((item) => item.id !== id));
  };

  const cancelAll = () => {
    controllersRef.current.forEach((controller) => controller.abort());
    setItems((current) => current.map((item) => (
      item.status === 'queued'
        ? { ...item, status: 'cancelled', error: '上传已取消' }
        : item
    )));
  };

  const clearFinished = () => {
    setItems((current) => current.filter((item) => !isTerminal(item.status)));
  };

  const summary = useMemo(() => ({
    queued: items.filter((item) => item.status === 'queued').length,
    uploading: items.filter((item) => item.status === 'uploading').length,
    done: items.filter((item) => item.status === 'done').length,
    failed: items.filter((item) => item.status === 'error').length,
  }), [items]);
  const hasPending = summary.queued + summary.uploading > 0;
  const hasFinished = items.some((item) => isTerminal(item.status));

  return (
    <div className="mx-auto max-w-3xl">
      <div className="mb-6">
        <h1 className="text-xl font-display text-primary">上传音频</h1>
        <p id="upload-policy" className="mt-1 text-sm text-text-muted">
          {AUDIO_EXTENSIONS.join(' / ')}，当前账号单文件上限 {formatFileSize(getUploadLimit(role))}
        </p>
      </div>

      <button
        type="button"
        onClick={() => inputRef.current?.click()}
        onDragEnter={handleDragEnter}
        onDragOver={(event) => event.preventDefault()}
        onDragLeave={handleDragLeave}
        onDrop={handleDrop}
        aria-describedby="upload-policy"
        className={`glass-card flex w-full flex-col items-center justify-center gap-3 border p-10 text-center transition-colors ${
          dragActive ? 'border-primary bg-primary/10' : 'border-transparent hover:border-primary/40'
        }`}
      >
        <CloudArrowUp size={44} className="text-primary" aria-hidden="true" />
        <span className="text-sm font-medium">选择音频文件</span>
        <span className="text-xs text-text-muted">也可以将多个文件拖放到这里</span>
      </button>
      <input
        ref={inputRef}
        type="file"
        multiple
        accept={AUDIO_ACCEPT}
        onChange={handleFileChange}
        className="sr-only"
        tabIndex={-1}
        aria-label="音频文件选择器"
      />

      {items.length > 0 && (
        <section className="mt-6" aria-labelledby="upload-queue-title">
          <div className="mb-3 flex flex-wrap items-center justify-between gap-3">
            <div>
              <h2 id="upload-queue-title" className="text-sm font-medium">上传队列</h2>
              <p className="mt-1 text-xs text-text-muted" aria-live="polite">
                {summary.uploading} 个上传中，{summary.queued} 个等待，{summary.done} 个成功，{summary.failed} 个失败
              </p>
            </div>
            <div className="flex items-center gap-2">
              {hasPending && (
                <button type="button" onClick={cancelAll} className="glass-button flex items-center gap-1.5 text-xs">
                  <X size={14} aria-hidden="true" />
                  取消全部
                </button>
              )}
              {hasFinished && (
                <button type="button" onClick={clearFinished} className="glass-button flex items-center gap-1.5 text-xs">
                  <Trash size={14} aria-hidden="true" />
                  清理已结束
                </button>
              )}
            </div>
          </div>

          <ul className="flex flex-col gap-2">
            {items.map((item) => (
              <li key={item.id} className="glass-card flex items-start gap-3 p-4">
                <FileAudio size={22} className="mt-0.5 shrink-0 text-primary" aria-hidden="true" />
                <div className="min-w-0 flex-1">
                  <div className="flex flex-wrap items-baseline justify-between gap-x-3 gap-y-1">
                    <p className="min-w-0 truncate text-sm font-medium" title={item.file.name}>{item.file.name}</p>
                    <span className="shrink-0 text-xs text-text-muted">{formatFileSize(item.file.size)}</span>
                  </div>
                  <div
                    className="mt-2 h-1.5 w-full overflow-hidden rounded-full bg-white/20"
                    role="progressbar"
                    aria-label={`${item.file.name} 上传进度`}
                    aria-valuemin={0}
                    aria-valuemax={100}
                    aria-valuenow={item.progress}
                  >
                    <div
                      className={`h-full rounded-full transition-[width] ${
                        item.status === 'error' ? 'bg-destructive' : 'bg-primary'
                      }`}
                      style={{ width: `${item.progress}%` }}
                    />
                  </div>
                  <p
                    className={`mt-2 text-xs ${
                      item.status === 'error' ? 'text-destructive' : 'text-text-muted'
                    }`}
                    role={item.status === 'error' ? 'alert' : undefined}
                  >
                    {item.error ?? statusText(item)}
                  </p>
                </div>
                <div className="flex shrink-0 items-center gap-1">
                  {(item.status === 'queued' || item.status === 'uploading') && (
                    <button
                      type="button"
                      onClick={() => cancelItem(item.id)}
                      className="grid h-8 w-8 place-items-center rounded text-text-muted hover:bg-white/10 hover:text-destructive"
                      aria-label={`取消 ${item.file.name}`}
                      title="取消上传"
                    >
                      <X size={16} aria-hidden="true" />
                    </button>
                  )}
                  {(item.status === 'error' || item.status === 'cancelled') && (
                    <button
                      type="button"
                      onClick={() => retryItem(item.id)}
                      className="grid h-8 w-8 place-items-center rounded text-text-muted hover:bg-white/10 hover:text-primary"
                      aria-label={`重试 ${item.file.name}`}
                      title="重试上传"
                    >
                      <ArrowClockwise size={16} aria-hidden="true" />
                    </button>
                  )}
                  {isTerminal(item.status) && (
                    <button
                      type="button"
                      onClick={() => removeItem(item.id)}
                      className="grid h-8 w-8 place-items-center rounded text-text-muted hover:bg-white/10 hover:text-destructive"
                      aria-label={`移除 ${item.file.name}`}
                      title="移除记录"
                    >
                      <Trash size={16} aria-hidden="true" />
                    </button>
                  )}
                </div>
              </li>
            ))}
          </ul>
        </section>
      )}
    </div>
  );
}
