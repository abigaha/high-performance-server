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
    <div className="flex items-center justify-center gap-2 mt-6">
      <button
        disabled={current <= 1}
        onClick={() => onChange(current - 1)}
        className="glass-button !py-1.5 !px-3 !bg-transparent !text-text disabled:opacity-30"
      >
        <CaretLeft size={16} />
      </button>
      {pages.map((p) => (
        <button
          key={p}
          onClick={() => onChange(p)}
          className={`glass-button !py-1.5 !px-3 !text-sm ${
            p === current ? '!bg-primary !text-white' : '!bg-transparent !text-text'
          }`}
        >
          {p}
        </button>
      ))}
      <button
        disabled={current >= totalPages}
        onClick={() => onChange(current + 1)}
        className="glass-button !py-1.5 !px-3 !bg-transparent !text-text disabled:opacity-30"
      >
        <CaretRight size={16} />
      </button>
    </div>
  );
}
