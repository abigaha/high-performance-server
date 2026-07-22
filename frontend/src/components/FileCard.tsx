import type { FileRecord } from '../types/api';
import { File as FileIcon, Download, Trash } from '@phosphor-icons/react';

interface Props {
  file: FileRecord;
  onDownload?: (id: number) => void;
  onDelete?: (id: number) => void;
  downloading?: boolean;
  deleting?: boolean;
}

function formatSize(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

function formatDate(dateStr: string): string {
  return new Date(dateStr).toLocaleDateString('zh-CN');
}

export default function FileCard({
  file,
  onDownload,
  onDelete,
  downloading = false,
  deleting = false,
}: Props) {
  return (
    <div className="glass-card p-4 flex flex-col gap-3 hover:shadow-lg transition-shadow">
      <div className="flex items-center gap-3">
        <div className="w-10 h-10 rounded-lg bg-primary/20 flex items-center justify-center">
          <FileIcon size={22} className="text-primary" />
        </div>
        <div className="flex-1 min-w-0">
          <p className="text-sm font-medium truncate">{file.file_name}</p>
          <p className="text-xs text-text-muted">{formatSize(file.file_size)}</p>
        </div>
      </div>
      <div className="flex items-center justify-between text-xs text-text-muted">
        <span>{formatDate(file.created_at)}</span>
        <span className="truncate max-w-[120px]">{file.content_type}</span>
      </div>
      <div className="flex gap-2 pt-1" onClick={(event) => event.stopPropagation()}>
        {onDownload && (
          <button
            type="button"
            aria-label={`下载 ${file.file_name}`}
            disabled={downloading || deleting}
            onClick={(event) => {
              event.stopPropagation();
              onDownload(file.file_id);
            }}
            className="glass-button !py-1.5 !px-3 !text-xs flex items-center gap-1"
          >
            <Download size={14} /> {downloading ? '准备中' : '下载'}
          </button>
        )}
        {onDelete && (
          <button
            type="button"
            aria-label={`删除 ${file.file_name}`}
            disabled={deleting || downloading}
            onClick={(event) => {
              event.stopPropagation();
              onDelete(file.file_id);
            }}
            className="glass-button !py-1.5 !px-3 !text-xs flex items-center gap-1 !bg-red-500/80"
          >
            <Trash size={14} /> {deleting ? '删除中' : '删除'}
          </button>
        )}
      </div>
    </div>
  );
}
