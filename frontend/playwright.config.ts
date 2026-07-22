import { defineConfig } from '@playwright/test';
import path from 'node:path';

function formatTimestamp(date: Date): string {
  const pad = (value: number) => String(value).padStart(2, '0');
  return [
    date.getFullYear(),
    pad(date.getMonth() + 1),
    pad(date.getDate()),
    '_',
    pad(date.getHours()),
    pad(date.getMinutes()),
    pad(date.getSeconds()),
  ].join('');
}

function sanitizePathPart(value: string): string {
  return value.replace(/[^A-Za-z0-9_-]+/g, '_').replace(/^_+|_+$/g, '');
}

function makeRunId(): string {
  const timestamp = formatTimestamp(new Date());
  const requested = process.env.PLAYWRIGHT_RUN_ID?.trim();
  if (!requested) return timestamp;

  const safeRequested = sanitizePathPart(requested);
  if (!safeRequested) return timestamp;
  return /\d{8}_\d{6}/.test(safeRequested)
    ? safeRequested
    : `${timestamp}_${safeRequested}`;
}

const runId = makeRunId();
const baseURL = process.env.PLAYWRIGHT_BASE_URL?.trim() || 'http://127.0.0.1:18080';
const executablePath = process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH?.trim();
const outputRoot = process.env.PLAYWRIGHT_OUTPUT_ROOT?.trim() || 'test-results';
const reportRoot = process.env.PLAYWRIGHT_REPORT_ROOT?.trim() || 'playwright-report';
const outputDir = path.resolve(outputRoot, `e2e_${runId}`);
const reportDir = path.resolve(reportRoot, `e2e_${runId}`);

export default defineConfig({
  testDir: './tests/e2e',
  fullyParallel: false,
  workers: 1,
  retries: process.env.CI ? 1 : 0,
  forbidOnly: Boolean(process.env.CI),
  timeout: 120_000,
  expect: {
    timeout: 15_000,
  },
  outputDir,
  preserveOutput: 'always',
  reporter: [
    ['line'],
    ['html', { outputFolder: reportDir, open: 'never' }],
  ],
  use: {
    baseURL,
    browserName: 'chromium',
    headless: true,
    launchOptions: executablePath ? { executablePath } : undefined,
    trace: 'retain-on-failure',
    screenshot: 'only-on-failure',
    video: 'retain-on-failure',
  },
  projects: [
    {
      name: 'desktop',
      use: { viewport: { width: 1440, height: 900 } },
    },
    {
      name: 'desktop-compact',
      use: { viewport: { width: 1280, height: 800 } },
    },
    {
      name: 'tablet',
      use: { viewport: { width: 768, height: 1024 } },
    },
    {
      name: 'mobile',
      use: { viewport: { width: 390, height: 844 } },
    },
  ],
});
