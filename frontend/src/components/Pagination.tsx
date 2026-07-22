import { CaretLeft, CaretRight } from '@phosphor-icons/react';

interface Props {
  current: number;
  total: number;
  pageSize: number;
  onChange: (page: number) => void;
}

export default function Pagination({ current, total, pageSize, onChange }: Props) {
  const totalPages = Math.ceil(total / pageSize);
  if (totalPages <= 1) return null;

  const pages: number[] = [];
  for (let i = Math.max(1, current - 2); i <= Math.min(totalPages, current + 2); i++) {
    pages.push(i);
  }

  return (
    <nav aria-label="分页导航" className="flex flex-wrap items-center justify-center gap-2 mt-6">
      <button
        type="button"
        aria-label="上一页"
        disabled={current <= 1}
        onClick={() => onChange(current - 1)}
        className="glass-button !p-0 !bg-transparent !text-text disabled:opacity-30 h-9 w-9"
      >
        <CaretLeft size={16} />
      </button>
      {pages.map((p) => (
        <button
          key={p}
          type="button"
          aria-label={`第 ${p} 页`}
          aria-current={p === current ? 'page' : undefined}
          onClick={() => onChange(p)}
          className={`glass-button !p-0 !text-sm h-9 min-w-9 ${
            p === current ? '!bg-primary !text-white' : '!bg-transparent !text-text'
          }`}
        >
          {p}
        </button>
      ))}
      <button
        type="button"
        aria-label="下一页"
        disabled={current >= totalPages}
        onClick={() => onChange(current + 1)}
        className="glass-button !p-0 !bg-transparent !text-text disabled:opacity-30 h-9 w-9"
      >
        <CaretRight size={16} />
      </button>
    </nav>
  );
}
