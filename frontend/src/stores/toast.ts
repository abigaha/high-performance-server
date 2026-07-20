import { create } from 'zustand';

interface ToastMessage {
  id: number;
  type: 'success' | 'error' | 'info';
  text: string;
}

interface ToastState {
  messages: ToastMessage[];
  success: (text: string) => void;
  error: (text: string) => void;
  info: (text: string) => void;
  remove: (id: number) => void;
}

let nextId = 0;

export const useToastStore = create<ToastState>((set) => ({
  messages: [],
  success: (text) => {
    const id = nextId++;
    set((s) => ({ messages: [...s.messages, { id, type: 'success', text }] }));
    setTimeout(() => set((s) => ({ messages: s.messages.filter((m) => m.id !== id) })), 3000);
  },
  error: (text) => {
    const id = nextId++;
    set((s) => ({ messages: [...s.messages, { id, type: 'error', text }] }));
    setTimeout(() => set((s) => ({ messages: s.messages.filter((m) => m.id !== id) })), 4000);
  },
  info: (text) => {
    const id = nextId++;
    set((s) => ({ messages: [...s.messages, { id, type: 'info', text }] }));
    setTimeout(() => set((s) => ({ messages: s.messages.filter((m) => m.id !== id) })), 3000);
  },
  remove: (id) => set((s) => ({ messages: s.messages.filter((m) => m.id !== id) })),
}));
