import { CheckCircle, Info, WarningCircle, X } from '@phosphor-icons/react';
import { useToastStore } from '../stores/toast';

const toastMeta = {
  success: { icon: CheckCircle, label: '成功' },
  error: { icon: WarningCircle, label: '错误' },
  info: { icon: Info, label: '提示' },
} as const;

export default function Toast() {
  const messages = useToastStore((state) => state.messages);
  const remove = useToastStore((state) => state.remove);

  if (messages.length === 0) return null;

  return (
    <div
      className="pointer-events-none fixed inset-x-3 top-3 z-[70] flex flex-col gap-2 sm:left-auto sm:right-4 sm:w-[22rem]"
      aria-label="通知"
    >
      {messages.map((message) => {
        const meta = toastMeta[message.type];
        const Icon = meta.icon;
        return (
          <div
            key={message.id}
            className={`toast-item pointer-events-auto ${message.type}`}
            role={message.type === 'error' ? 'alert' : 'status'}
            aria-live={message.type === 'error' ? 'assertive' : 'polite'}
            aria-atomic="true"
          >
            <Icon size={20} className="toast-icon" weight="fill" aria-hidden="true" />
            <div className="min-w-0 flex-1">
              <p className="text-xs font-semibold">{meta.label}</p>
              <p className="mt-0.5 break-words text-sm">{message.text}</p>
            </div>
            <button
              type="button"
              onClick={() => remove(message.id)}
              className="icon-button shrink-0"
              aria-label={`关闭通知：${message.text}`}
              title="关闭通知"
            >
              <X size={17} aria-hidden="true" />
            </button>
          </div>
        );
      })}
    </div>
  );
}
