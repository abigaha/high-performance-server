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
    <nav aria-label="分页导航" className="pagination mt-6 flex items-center justify-center gap-2">
      <button
        type="button"
        aria-label="上一页"
        disabled={current <= 1}
        onClick={() => onChange(current - 1)}
        className="pagination-button glass-button !bg-transparent !p-0 !text-text disabled:opacity-30"
      >
        <CaretLeft size={16} aria-hidden="true" />
      </button>
      {pages.map((p) => (
        <button
          key={p}
          type="button"
          aria-label={`第 ${p} 页`}
          aria-current={p === current ? 'page' : undefined}
          onClick={() => onChange(p)}
          className={`pagination-button glass-button !p-0 !text-sm ${
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
        className="pagination-button glass-button !bg-transparent !p-0 !text-text disabled:opacity-30"
      >
        <CaretRight size={16} aria-hidden="true" />
      </button>
    </nav>
  );
}
