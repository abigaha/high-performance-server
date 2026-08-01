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
  return value
    .replace(/latest/gi, '')
    .replace(/[^A-Za-z0-9_-]+/g, '_')
    .replace(/^_+|_+$/g, '');
}

function makeRunId(): string {
  const timestamp = formatTimestamp(new Date());
  const requested = process.env.PLAYWRIGHT_RUN_ID?.trim();
  if (!requested) return timestamp;

  const safeRequested = sanitizePathPart(requested);
  if (!safeRequested) return timestamp;
  return `${timestamp}_${safeRequested}`;
}

const runId = makeRunId();
const baseURL = process.env.PLAYWRIGHT_BASE_URL?.trim() || 'http://127.0.0.1:18080';
const executablePath = process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH?.trim();
const outputRoot = process.env.PLAYWRIGHT_OUTPUT_ROOT?.trim() || 'test-results';
const reportRoot = process.env.PLAYWRIGHT_REPORT_ROOT?.trim() || 'playwright-report';

function resolveRunDirectory(root: string, variableName: string): string {
  if (root.split(/[\\/]/).some((segment) => /latest/i.test(segment))) {
    throw new Error(`环境变量 ${variableName} 不允许包含 latest 路径段`);
  }

  const resolved = path.resolve(root, `e2e_${runId}`);
  if (/latest/i.test(resolved)) {
    throw new Error(`Playwright ${variableName} 最终路径不允许包含 latest`);
  }
  return resolved;
}

const outputDir = resolveRunDirectory(outputRoot, 'PLAYWRIGHT_OUTPUT_ROOT');
const reportDir = resolveRunDirectory(reportRoot, 'PLAYWRIGHT_REPORT_ROOT');

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
      name: 'user-governance',
      testMatch: /user-governance\.spec\.ts/,
      use: { viewport: { width: 1440, height: 900 } },
    },
    {
      name: 'desktop',
      testMatch: /deployment\.spec\.ts/,
      use: { viewport: { width: 1440, height: 900 } },
    },
    {
      name: 'desktop-compact',
      testMatch: /deployment\.spec\.ts/,
      use: { viewport: { width: 1280, height: 800 } },
    },
    {
      name: 'tablet',
      testMatch: /deployment\.spec\.ts/,
      use: { viewport: { width: 768, height: 1024 } },
    },
    {
      name: 'mobile',
      testMatch: /deployment\.spec\.ts/,
      use: { viewport: { width: 390, height: 844 } },
    },
    {
      name: 'visual-breakpoint',
      testMatch: /visual\.spec\.ts/,
      use: { viewport: { width: 1024, height: 768 } },
    },
    {
      name: 'visual-mobile-small',
      testMatch: /visual\.spec\.ts/,
      use: { viewport: { width: 375, height: 812 } },
    },
  ],
});
