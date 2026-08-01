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
  reset: () => void;
}

let nextId = 0;
let toastGeneration = 0;

function scheduleRemoval(id: number, delay: number, generation: number): void {
  setTimeout(() => {
    if (generation === toastGeneration) {
      useToastStore.setState((state) => ({
        messages: state.messages.filter((message) => message.id !== id),
      }));
    }
  }, delay);
}

export const useToastStore = create<ToastState>((set) => ({
  messages: [],
  success: (text) => {
    const id = nextId++;
    const generation = toastGeneration;
    set((s) => ({ messages: [...s.messages, { id, type: 'success', text }] }));
    scheduleRemoval(id, 3000, generation);
  },
  error: (text) => {
    const id = nextId++;
    const generation = toastGeneration;
    set((s) => ({ messages: [...s.messages, { id, type: 'error', text }] }));
    scheduleRemoval(id, 4000, generation);
  },
  info: (text) => {
    const id = nextId++;
    const generation = toastGeneration;
    set((s) => ({ messages: [...s.messages, { id, type: 'info', text }] }));
    scheduleRemoval(id, 3000, generation);
  },
  remove: (id) => set((s) => ({ messages: s.messages.filter((m) => m.id !== id) })),
  reset: () => {
    toastGeneration += 1;
    nextId = 0;
    set({ messages: [] });
  },
}));
