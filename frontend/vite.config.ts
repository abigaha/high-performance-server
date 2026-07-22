import { defineConfig } from 'vite'
import { configDefaults } from 'vitest/config'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'

export default defineConfig({
  plugins: [react(), tailwindcss()],
  test: {
    exclude: [...configDefaults.exclude, 'tests/e2e/**'],
    globals: true,
    environment: 'jsdom',
    setupFiles: './tests/setup.ts',
  },
})
