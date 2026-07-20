import { useState, useRef } from 'react';
import { uploadFile } from '../api/files';
import { useToastStore } from '../stores/toast';
import { CloudArrowUp, File } from '@phosphor-icons/react';

interface UploadItem {
  name: string;
  progress: number;
  status: 'pending' | 'uploading' | 'done' | 'error';
  error?: string;
}

export default function UploadPage() {
  const [items, setItems] = useState<UploadItem[]>([]);
  const [uploading, setUploading] = useState(false);
  const inputRef = useRef<HTMLInputElement>(null);
  const toast = useToastStore();

  const addFiles = (files: FileList) => {
    const newItems: UploadItem[] = Array.from(files).map((f) => ({
      name: f.name,
      progress: 0,
      status: 'pending' as const,
    }));
    setItems((prev) => [...prev, ...newItems]);
    return Array.from(files);
  };

  const handleDrop = (e: React.DragEvent) => {
    e.preventDefault();
    if (uploading) return;
    const files = e.dataTransfer.files;
    if (files.length > 0) {
      const fileList = addFiles(files);
      startUpload(fileList);
    }
  };

  const handleSelect = () => {
    inputRef.current?.click();
  };

  const handleFileChange = (e: React.ChangeEvent<HTMLInputElement>) => {
    const files = e.target.files;
    if (files && files.length > 0) {
      const fileList = addFiles(files);
      startUpload(fileList);
    }
  };

  const startUpload = async (files: File[]) => {
    if (uploading) return;
    setUploading(true);

    for (let i = 0; i < files.length; i++) {
      const idx = items.length + i;
      setItems((prev) => prev.map((item, j) => j === idx ? { ...item, status: 'uploading' } : item));

      try {
        await uploadFile(files[i], (pct) => {
          setItems((prev) => prev.map((item, j) => j === idx ? { ...item, progress: pct } : item));
        });
        setItems((prev) => prev.map((item, j) => j === idx ? { ...item, status: 'done', progress: 100 } : item));
      } catch {
        setItems((prev) => prev.map((item, j) => j === idx ? { ...item, status: 'error', error: '上传失败' } : item));
      }
    }

    setUploading(false);
    toast.success('上传完成');
  };

  return (
    <div className="max-w-2xl mx-auto">
      <h1 className="text-xl font-display text-primary mb-6">上传文件</h1>

      <div
        onDrop={handleDrop}
        onDragOver={(e) => e.preventDefault()}
        onClick={handleSelect}
        className="glass-card p-12 flex flex-col items-center justify-center gap-4 cursor-pointer hover:shadow-lg transition-shadow"
      >
        <CloudArrowUp size={48} className="text-primary/60" />
        <p className="text-sm text-text-muted">拖拽文件到此处，或点击选择文件</p>
        <input
          ref={inputRef}
          type="file"
          multiple
          onChange={handleFileChange}
          className="hidden"
        />
      </div>

      {items.length > 0 && (
        <div className="mt-6 flex flex-col gap-3">
          {items.map((item, i) => (
            <div key={i} className="glass-card p-4 flex items-center gap-4">
              <File size={20} className="text-primary" />
              <div className="flex-1 min-w-0">
                <p className="text-sm truncate">{item.name}</p>
                <div className="w-full h-1.5 bg-white/20 rounded-full mt-2">
                  <div
                    className={`h-full rounded-full transition-all ${
                      item.status === 'error' ? 'bg-destructive' : 'bg-primary'
                    }`}
                    style={{ width: `${item.progress}%` }}
                  />
                </div>
              </div>
              <span className={`text-xs shrink-0 ${
                item.status === 'done' ? 'text-primary' : item.status === 'error' ? 'text-destructive' : 'text-text-muted'
              }`}>
                {item.status === 'done' ? '完成' : item.status === 'error' ? '失败' : `${item.progress}%`}
              </span>
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
