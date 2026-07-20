import { useToastStore } from '../stores/toast';

export default function Toast() {
  const messages = useToastStore((s) => s.messages);
  const remove = useToastStore((s) => s.remove);

  if (messages.length === 0) return null;

  return (
    <div className="fixed top-4 right-4 z-50 flex flex-col gap-2">
      {messages.map((m) => {
        const bg = m.type === 'error' ? 'bg-red-500/80' : m.type === 'success' ? 'bg-emerald-500/80' : 'bg-blue-500/80';
        return (
          <div
            key={m.id}
            className={`${bg} backdrop-blur-md text-white px-4 py-3 rounded-xl shadow-lg flex items-center gap-3 min-w-[280px] cursor-pointer`}
            onClick={() => remove(m.id)}
          >
            <span className="text-sm">{m.text}</span>
          </div>
        );
      })}
    </div>
  );
}
