import { randomBytes } from 'node:crypto';
import { expect, test as base } from '@playwright/test';
import type { Page, TestInfo } from '@playwright/test';

interface ConsoleAuditFixture {
  consoleAudit: void;
}

function isExpectedMediaDecodeError(message: string): boolean {
  const normalized = message.toUpperCase();
  return [
    'MEDIA_ERR_DECODE',
    'DEMUXER_ERROR_COULD_NOT_OPEN',
    'PIPELINE_ERROR_DECODE',
  ].some((marker) => normalized.includes(marker));
}

const test = base.extend<ConsoleAuditFixture>({
  consoleAudit: [async ({ page }, use) => {
    const browserErrors: string[] = [];
    page.on('console', (message) => {
      if (message.type() !== 'error') return;
      const text = message.text();
      // 微型音频夹具用于验证上传链路；部分 Chromium 版本探测它时会报告解码错误。
      // 只忽略明确的媒体解码标识，网络、接口和脚本错误仍会使测试失败。
      if (isExpectedMediaDecodeError(text)) return;

      const location = message.location();
      const source = location.url
        ? ` (${location.url}:${location.lineNumber ?? 0})`
        : '';
      browserErrors.push(`[console.error] ${text}${source}`);
    });
    page.on('pageerror', (error) => {
      if (!isExpectedMediaDecodeError(error.message)) {
        browserErrors.push(`[pageerror] ${error.stack ?? error.message}`);
      }
    });

    await use();
    expect(browserErrors, `浏览器控制台存在未处理错误：\n${browserErrors.join('\n')}`).toEqual([]);
  }, { auto: true }],
});

function formatTimestamp(date = new Date()): string {
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

function createUniqueIdentity(projectName: string) {
  const timestamp = formatTimestamp();
  const randomSuffix = randomBytes(4).toString('hex');
  const projectSuffix = projectName.slice(0, 3).toLowerCase();
  const key = `${timestamp}_${projectSuffix}_${randomSuffix}`;
  return {
    key,
    username: `e2e_${key}`,
    email: `e2e_${key}@example.com`,
    password: `E2ePass_${randomSuffix}`,
  };
}

function createWavFixture(): Buffer {
  const sampleRate = 8_000;
  const channels = 1;
  const bitsPerSample = 16;
  const sampleCount = 800;
  const bytesPerSample = bitsPerSample / 8;
  const dataSize = sampleCount * channels * bytesPerSample;
  const wav = Buffer.alloc(44 + dataSize);

  wav.write('RIFF', 0, 'ascii');
  wav.writeUInt32LE(36 + dataSize, 4);
  wav.write('WAVE', 8, 'ascii');
  wav.write('fmt ', 12, 'ascii');
  wav.writeUInt32LE(16, 16);
  wav.writeUInt16LE(1, 20);
  wav.writeUInt16LE(channels, 22);
  wav.writeUInt32LE(sampleRate, 24);
  wav.writeUInt32LE(sampleRate * channels * bytesPerSample, 28);
  wav.writeUInt16LE(channels * bytesPerSample, 32);
  wav.writeUInt16LE(bitsPerSample, 34);
  wav.write('data', 36, 'ascii');
  wav.writeUInt32LE(dataSize, 40);
  randomBytes(dataSize).copy(wav, 44);
  return wav;
}

async function expectNoHorizontalOverflow(page: Page, context: string): Promise<void> {
  const dimensions = await page.evaluate(() => ({
    viewportWidth: window.innerWidth,
    documentWidth: document.documentElement.scrollWidth,
    bodyWidth: document.body.scrollWidth,
  }));
  const contentWidth = Math.max(dimensions.documentWidth, dimensions.bodyWidth);
  expect(
    contentWidth,
    `${context} 存在水平溢出：内容 ${contentWidth}px，视口 ${dimensions.viewportWidth}px`,
  ).toBeLessThanOrEqual(dimensions.viewportWidth + 1);
}

async function capture(page: Page, testInfo: TestInfo, timestamp: string, name: string): Promise<void> {
  const projectName = testInfo.project.name.replace(/[^A-Za-z0-9_-]+/g, '_');
  await page.screenshot({
    path: testInfo.outputPath(`${timestamp}_${projectName}_${name}.png`),
    fullPage: true,
  });
}

async function openUploadThroughResponsiveNavigation(
  page: Page,
  testInfo: TestInfo,
  screenshotTimestamp: string,
): Promise<void> {
  const sidebar = page.locator('aside[aria-label="主导航"]');
  const isDesktop = (page.viewportSize()?.width ?? 0) >= 1024;

  if (isDesktop) {
    await expect(sidebar).toBeVisible();
    await expect(page.getByRole('button', { name: '打开导航菜单' })).toBeHidden();
  } else {
    await expect(sidebar).toHaveAttribute('aria-hidden', 'true');
    await page.getByRole('button', { name: '打开导航菜单' }).click();
    await expect(sidebar).toBeVisible();
    await expect(sidebar).toHaveAttribute('aria-hidden', 'false');
  }

  await expectNoHorizontalOverflow(page, `${testInfo.project.name} 导航`);
  await capture(page, testInfo, screenshotTimestamp, 'navigation');
  await sidebar.getByRole('link', { name: '上传', exact: true }).click();
  await expect(page).toHaveURL(/\/upload$/);
  await expect(page.getByText('选择音频文件', { exact: true })).toBeVisible();

  if (!isDesktop) {
    await expect(sidebar).toHaveAttribute('aria-hidden', 'true');
  }
}

test('真实部署核心流程与响应式界面', async ({ page, request }, testInfo) => {
  const identity = createUniqueIdentity(testInfo.project.name);
  const screenshotTimestamp = formatTimestamp();

  const healthResponse = await request.get('/api/health');
  expect(healthResponse, `健康检查失败：${await healthResponse.text()}`).toBeOK();
  expect(await healthResponse.json()).toEqual(expect.objectContaining({
    status: 'ok',
    uptime: expect.any(Number),
  }));

  const rootResponse = await request.get('/');
  expect(rootResponse, `首页不可达：${await rootResponse.text()}`).toBeOK();
  expect(rootResponse.headers()['content-type']).toContain('text/html');

  const deepLinkResponse = await request.get('/music/library');
  expect(deepLinkResponse, `SPA 深链不可达：${await deepLinkResponse.text()}`).toBeOK();
  expect(deepLinkResponse.headers()['content-type']).toContain('text/html');

  await page.goto('/register');
  await expect(page.getByRole('heading', { name: '注册', exact: true })).toBeVisible();
  await expectNoHorizontalOverflow(page, `${testInfo.project.name} 注册页`);
  await page.getByLabel('用户名').fill(identity.username);
  await page.getByLabel('邮箱').fill(identity.email);
  await page.getByLabel('密码').fill(identity.password);

  const registerResponsePromise = page.waitForResponse((response) => (
    response.request().method() === 'POST'
    && new URL(response.url()).pathname === '/api/auth/register'
  ));
  await page.getByRole('button', { name: '注册', exact: true }).click();
  const registerResponse = await registerResponsePromise;
  expect(
    registerResponse.status(),
    `注册接口响应异常：${await registerResponse.text()}`,
  ).toBe(201);
  await expect(page).toHaveURL(/\/files$/);
  await expect(page.getByRole('heading', { name: '文件列表', exact: true })).toBeVisible();
  await expect(page.getByText('正在加载文件...', { exact: true })).toBeHidden();
  await expect(page.getByLabel(new RegExp(`当前用户：${identity.username}`))).toBeVisible();

  const logoutResponsePromise = page.waitForResponse((response) => (
    response.request().method() === 'POST'
    && new URL(response.url()).pathname === '/api/auth/logout'
  ));
  await page.getByRole('button', { name: '退出登录' }).click();
  const logoutResponse = await logoutResponsePromise;
  expect(logoutResponse.status(), `退出接口响应异常：${await logoutResponse.text()}`).toBe(200);
  await expect(page).toHaveURL(/\/login$/);
  await expectNoHorizontalOverflow(page, `${testInfo.project.name} 登录页`);

  await page.getByLabel('用户名').fill(identity.username);
  await page.getByLabel('密码').fill(identity.password);
  const loginResponsePromise = page.waitForResponse((response) => (
    response.request().method() === 'POST'
    && new URL(response.url()).pathname === '/api/auth/login'
  ));
  await page.getByRole('button', { name: '登录', exact: true }).click();
  const loginResponse = await loginResponsePromise;
  const loginResponseBody = await loginResponse.text();
  expect(loginResponse.status(), `登录接口响应异常：${loginResponseBody}`).toBe(200);
  const loginResult = JSON.parse(loginResponseBody) as { token: string };
  expect(loginResult).toEqual(expect.objectContaining({ token: expect.any(String) }));
  const bearerToken = loginResult.token;
  expect(bearerToken, '登录响应未返回有效 Bearer Token').not.toBe('');
  await expect(page).toHaveURL(/\/files$/);
  await expect(page.getByText('正在加载文件...', { exact: true })).toBeHidden();

  await page.reload();
  await expect(page).toHaveURL(/\/files$/);
  await expect(page.getByRole('heading', { name: '文件列表', exact: true })).toBeVisible();
  await expect(page.getByText('正在加载文件...', { exact: true })).toBeHidden();
  await expectNoHorizontalOverflow(page, `${testInfo.project.name} 文件页`);
  await capture(page, testInfo, screenshotTimestamp, 'files');

  await openUploadThroughResponsiveNavigation(page, testInfo, screenshotTimestamp);
  await expectNoHorizontalOverflow(page, `${testInfo.project.name} 上传页`);

  const uploadRequests: string[] = [];
  page.on('request', (uploadRequest) => {
    if (
      uploadRequest.method() === 'POST'
      && new URL(uploadRequest.url()).pathname === '/api/files/upload'
    ) {
      uploadRequests.push(uploadRequest.headers()['content-disposition'] ?? uploadRequest.url());
    }
  });

  const fileInput = page.locator('input[type="file"]');
  const invalidFileName = `invalid_${identity.key}.txt`;
  await fileInput.setInputFiles({
    name: invalidFileName,
    mimeType: 'text/plain',
    buffer: Buffer.from('not an audio file', 'utf8'),
  });
  await expect(page.getByText(/不支持该文件类型/)).toBeVisible();
  await page.waitForTimeout(300);
  expect(uploadRequests, '无效文件不应产生上传请求').toHaveLength(0);

  const firstValidFileName = `e2e_audio_${identity.key}_1.wav`;
  const secondValidFileName = `e2e_audio_${identity.key}_2.wav`;
  const firstWavBytes = createWavFixture();
  let markFirstUploadIntercepted: (() => void) | undefined;
  let releaseFirstUpload: (() => void) | undefined;
  const firstUploadIntercepted = new Promise<void>((resolve) => {
    markFirstUploadIntercepted = resolve;
  });
  const firstUploadGate = new Promise<void>((resolve) => {
    releaseFirstUpload = resolve;
  });
  const fallbackReleaseTimer = setTimeout(() => releaseFirstUpload?.(), 5_000);

  await page.route('**/api/files/upload', async (route) => {
    const disposition = route.request().headers()['content-disposition'] ?? '';
    if (disposition.includes(firstValidFileName)) {
      markFirstUploadIntercepted?.();
      await firstUploadGate;
    }
    await route.continue();
  });

  const waitForUploadResponse = (fileName: string) => page.waitForResponse((response) => (
    response.request().method() === 'POST'
    && new URL(response.url()).pathname === '/api/files/upload'
    && (response.request().headers()['content-disposition'] ?? '').includes(fileName)
  ));
  const firstUploadResponsePromise = waitForUploadResponse(firstValidFileName);
  await fileInput.setInputFiles({
    name: firstValidFileName,
    mimeType: 'audio/wav',
    buffer: firstWavBytes,
  });
  await firstUploadIntercepted;
  const firstUploadItem = page.getByRole('listitem').filter({ hasText: firstValidFileName });
  await expect(firstUploadItem).toContainText('上传中');

  const secondUploadResponsePromise = waitForUploadResponse(secondValidFileName);
  await fileInput.setInputFiles({
    name: secondValidFileName,
    mimeType: 'audio/wav',
    buffer: createWavFixture(),
  });
  const secondUploadItem = page.getByRole('listitem').filter({ hasText: secondValidFileName });
  await expect(secondUploadItem).toBeVisible();
  await expect(firstUploadItem).toContainText('上传中');
  clearTimeout(fallbackReleaseTimer);
  releaseFirstUpload?.();

  const [firstUploadResponse, secondUploadResponse] = await Promise.all([
    firstUploadResponsePromise,
    secondUploadResponsePromise,
  ]);
  expect(
    firstUploadResponse.status(),
    `第一条有效音频上传失败：${await firstUploadResponse.text()}`,
  ).toBe(201);
  expect(
    secondUploadResponse.status(),
    `追加的有效音频上传失败：${await secondUploadResponse.text()}`,
  ).toBe(201);
  const firstUploadResult = await firstUploadResponse.json() as {
    file_id: number;
    file_name: string;
    size: number;
  };
  const secondUploadResult = await secondUploadResponse.json() as {
    file_id: number;
    file_name: string;
    size: number;
  };
  expect(firstUploadResult).toEqual(expect.objectContaining({
    file_id: expect.any(Number),
    file_name: firstValidFileName,
    size: expect.any(Number),
  }));
  expect(secondUploadResult).toEqual(expect.objectContaining({
    file_id: expect.any(Number),
    file_name: secondValidFileName,
    size: expect.any(Number),
  }));
  expect(firstUploadResult.size).toBe(firstWavBytes.length);
  await expect(firstUploadItem).toContainText('上传成功');
  await expect(secondUploadItem).toContainText('上传成功');

  const authenticatedHeaders = { Authorization: `Bearer ${bearerToken}` };
  const fullStreamResponse = await request.get(`/api/files/${firstUploadResult.file_id}/stream`, {
    headers: authenticatedHeaders,
  });
  const fullStreamBody = await fullStreamResponse.body();
  expect(
    fullStreamResponse.status(),
    `已认证完整流播请求失败：${fullStreamBody.toString('utf8')}`,
  ).toBe(200);
  const fullStreamHeaders = fullStreamResponse.headers();
  expect(fullStreamHeaders['content-length']).toBe(String(firstWavBytes.length));
  expect(fullStreamHeaders['accept-ranges']).toBe('bytes');
  expect(fullStreamBody).toEqual(firstWavBytes);

  const rangeStart = 44;
  const rangeEnd = 127;
  const expectedRangeBody = firstWavBytes.subarray(rangeStart, rangeEnd + 1);
  const rangeStreamResponse = await request.get(`/api/files/${firstUploadResult.file_id}/stream`, {
    headers: {
      ...authenticatedHeaders,
      Range: `bytes=${rangeStart}-${rangeEnd}`,
    },
  });
  const rangeStreamBody = await rangeStreamResponse.body();
  expect(
    rangeStreamResponse.status(),
    `已认证区间流播请求失败：${rangeStreamBody.toString('utf8')}`,
  ).toBe(206);
  const rangeStreamHeaders = rangeStreamResponse.headers();
  expect(rangeStreamHeaders['content-range']).toBe(
    `bytes ${rangeStart}-${rangeEnd}/${firstWavBytes.length}`,
  );
  expect(rangeStreamHeaders['content-length']).toBe(String(expectedRangeBody.length));
  expect(rangeStreamBody).toEqual(expectedRangeBody);

  const unsatisfiableRangeResponse = await request.get(`/api/files/${firstUploadResult.file_id}/stream`, {
    headers: {
      ...authenticatedHeaders,
      Range: `bytes=${firstWavBytes.length}-`,
    },
  });
  const unsatisfiableRangeBody = await unsatisfiableRangeResponse.text();
  expect(
    unsatisfiableRangeResponse.status(),
    `不可满足的区间流播请求未返回 416：${unsatisfiableRangeBody}`,
  ).toBe(416);
  expect(unsatisfiableRangeResponse.headers()['content-range']).toBe(`bytes */${firstWavBytes.length}`);

  const downloadResponse = await request.get(`/api/files/${firstUploadResult.file_id}/download`, {
    headers: authenticatedHeaders,
  });
  const downloadBody = await downloadResponse.body();
  expect(downloadResponse.status(), `已认证下载请求失败：${downloadBody.toString('utf8')}`).toBe(200);
  const contentDisposition = downloadResponse.headers()['content-disposition'];
  expect(contentDisposition).toContain(`filename="${firstValidFileName}"`);
  expect(contentDisposition).toContain(`filename*=UTF-8''${encodeURIComponent(firstValidFileName)}`);
  expect(downloadBody).toEqual(firstWavBytes);

  // Playwright request 不共享页面 localStorage 中的 Bearer Token，此请求应保持匿名。
  const anonymousStreamResponse = await request.get(`/api/files/${firstUploadResult.file_id}/stream`);
  const anonymousStreamBody = await anonymousStreamResponse.text();
  expect(
    anonymousStreamResponse.status(),
    `匿名流播请求未被拒绝：${anonymousStreamBody}`,
  ).toBe(401);
  expect(anonymousStreamBody, '匿名流播错误正文应明确说明未登录').toMatch(/需要登录|未登录/);

  expect(uploadRequests).toHaveLength(2);
  expect(uploadRequests).toEqual(expect.arrayContaining([
    expect.stringContaining(firstValidFileName),
    expect.stringContaining(secondValidFileName),
  ]));
  await expectNoHorizontalOverflow(page, `${testInfo.project.name} 上传完成页`);
  await capture(page, testInfo, screenshotTimestamp, 'upload-success');

  const finalHealthResponse = await request.get('/api/health');
  expect(finalHealthResponse, `流程结束时健康检查失败：${await finalHealthResponse.text()}`).toBeOK();
  expect(await finalHealthResponse.json()).toEqual(expect.objectContaining({
    status: 'ok',
    uptime: expect.any(Number),
  }));
});
