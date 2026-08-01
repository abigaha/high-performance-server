import { expect, test } from '@playwright/test';
import type { Locator, Page, TestInfo } from '@playwright/test';

const FIXED_TIME = new Date('2026-01-15T08:00:00.000Z');
const AUTH_TOKEN = 'visual-fixture-token';
const FIXTURE_USER = {
  user_id: 42,
  username: 'visual-user',
  email: 'visual@example.com',
  role: 'VIP',
  vip_status: 'ACTIVE',
  vip_expires_at: '2026-02-15T08:00:00.000Z',
  capabilities: ['USE_AUTHENTICATED_FEATURES', 'USE_VIP_BENEFITS'],
  created_at: '2026-01-01T08:00:00.000Z',
};
const LONG_FILE_NAME = `${'crystal-visual-layout-'.repeat(8)}final-track.mp3`;
const LONG_GATEWAY_DETAIL = '上游存储节点返回了无法完成请求的确定性错误，请检查完整错误文本在窄屏队列中是否换行且没有被按钮、进度条或视口边缘截断。';
const LONG_GATEWAY_ERROR = `上游网关拒绝上传 ${LONG_GATEWAY_DETAIL}`;
const HTTP_502_CONSOLE_ERROR = '[console.error] Failed to load resource: the server responded with a status of 502 (Bad Gateway)';

interface BrowserError {
  source: 'console.error' | 'pageerror';
  text: string;
  url: string;
}

const files = [
  {
    file_id: 101,
    file_name: 'crystal-dawn.mp3',
    file_hash: 'fixture-file-hash-101',
    file_size: 4_321_000,
    content_type: 'audio/mpeg',
    uploaded_by: FIXTURE_USER.user_id,
    can_delete: true,
    created_at: '2026-01-15T07:30:00.000Z',
  },
  {
    file_id: 102,
    file_name: 'emerald-night-session-with-a-long-name.flac',
    file_hash: 'fixture-file-hash-102',
    file_size: 12_345_678,
    content_type: 'audio/flac',
    uploaded_by: FIXTURE_USER.user_id,
    can_delete: true,
    created_at: '2026-01-14T18:20:00.000Z',
  },
];

const music = Array.from({ length: 8 }, (_, index) => {
  const musicId = index + 1;
  return {
    music_id: musicId,
    title: `Crystal Track ${musicId}`,
    artist: `Visual Artist ${musicId}`,
    album: `Fixture Album ${Math.ceil(musicId / 2)}`,
    genre: 'Ambient',
    duration_sec: 180 + musicId,
    file_hash: `fixture-music-hash-${musicId}`,
    file_size: 1_000_000 + musicId,
    content_type: 'audio/wav',
  };
});

let browserErrors: BrowserError[] = [];

test.beforeEach(async ({ page }) => {
  browserErrors = [];
  page.on('console', (message) => {
    if (message.type() === 'error') {
      browserErrors.push({
        source: 'console.error',
        text: `[console.error] ${message.text()}`,
        url: message.location().url,
      });
    }
  });
  page.on('pageerror', (error) => {
    browserErrors.push({
      source: 'pageerror',
      text: `[pageerror] ${error.stack ?? error.message}`,
      url: page.url(),
    });
  });

  await page.clock.install({ time: FIXED_TIME });
  await page.emulateMedia({ colorScheme: 'light', reducedMotion: 'reduce' });
  await installVisualApiFixtures(page);
});

test.afterEach(() => {
  expect(
    browserErrors,
    `浏览器存在未处理错误：\n${JSON.stringify(browserErrors, null, 2)}`,
  ).toEqual([]);
});

async function expectNoHorizontalOverflow(page: Page, context: string): Promise<void> {
  const dimensions = await page.evaluate(() => ({
    viewportWidth: window.innerWidth,
    documentWidth: document.documentElement.scrollWidth,
    bodyWidth: document.body.scrollWidth,
  }));
  const contentWidth = Math.max(dimensions.documentWidth, dimensions.bodyWidth);
  expect(
    contentWidth,
    `${context} 存在横向溢出：内容 ${contentWidth}px，视口 ${dimensions.viewportWidth}px`,
  ).toBeLessThanOrEqual(dimensions.viewportWidth + 1);
}

async function expectNoOverlap(first: Locator, second: Locator, context: string): Promise<void> {
  await expect(first, `${context} 的第一个区域不可见`).toBeVisible();
  await expect(second, `${context} 的第二个区域不可见`).toBeVisible();
  const secondElement = await second.elementHandle();
  if (!secondElement) throw new Error(`${context} 的第二个区域不存在`);
  const intersectionArea = await first.evaluate((firstElement, secondElement) => {
    const firstRect = firstElement.getBoundingClientRect();
    const secondRect = (secondElement as Element).getBoundingClientRect();
    const width = Math.max(0, Math.min(firstRect.right, secondRect.right) - Math.max(firstRect.left, secondRect.left));
    const height = Math.max(0, Math.min(firstRect.bottom, secondRect.bottom) - Math.max(firstRect.top, secondRect.top));
    return width * height;
  }, secondElement);
  expect(intersectionArea, `${context} 的 DOMRect 交集面积应为 0`).toBe(0);
}

async function expectMinimumSize(locator: Locator, minimum: number, context: string): Promise<void> {
  await expect(locator, `${context} 不可见`).toBeVisible();
  const box = await locator.boundingBox();
  expect(box, `${context} 缺少可测量的边界框`).not.toBeNull();
  expect(box!.width, `${context} 宽度小于 ${minimum}px`).toBeGreaterThanOrEqual(minimum);
  expect(box!.height, `${context} 高度小于 ${minimum}px`).toBeGreaterThanOrEqual(minimum);
}

async function expectCssCustomProperty(locator: Locator, property: string, expected: string): Promise<void> {
  const actual = await locator.evaluate((element, property) => (
    getComputedStyle(element).getPropertyValue(property).trim().toUpperCase()
  ), property);
  expect(actual).toBe(expected.toUpperCase());
}

async function expectAndConsumeBrowserError(expectedText: string, expectedPathname: string): Promise<BrowserError> {
  const matchesExpectedError = (error: BrowserError): boolean => {
    if (error.text !== expectedText || !error.url) return false;
    try {
      return new URL(error.url).pathname === expectedPathname;
    } catch {
      return false;
    }
  };
  await expect.poll(
    () => browserErrors.filter(matchesExpectedError).length,
    { message: `未捕获唯一的预期浏览器错误：${expectedText} (${expectedPathname})` },
  ).toBe(1);
  const errorIndex = browserErrors.findIndex(matchesExpectedError);
  return browserErrors.splice(errorIndex, 1)[0];
}

async function installVisualApiFixtures(page: Page): Promise<void> {
  const configuredBaseUrl = process.env.PLAYWRIGHT_BASE_URL?.trim() || 'http://127.0.0.1:18080';
  const baseOrigin = new URL(configuredBaseUrl).origin;
  await page.route('**/*', async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    const pathname = url.pathname;

    const isHttpRequest = url.protocol === 'http:' || url.protocol === 'https:';
    if (isHttpRequest && url.origin !== baseOrigin) {
      await route.abort('blockedbyclient');
      return;
    }
    if (!isHttpRequest) {
      await route.continue();
      return;
    }

    if (pathname === '/api/auth/me') {
      await route.fulfill({ json: FIXTURE_USER });
      return;
    }
    if (pathname === '/api/auth/login') {
      await route.fulfill({ json: { token: AUTH_TOKEN, user: FIXTURE_USER } });
      return;
    }
    if (pathname === '/api/auth/register') {
      await route.fulfill({ status: 201, json: { token: AUTH_TOKEN, user: FIXTURE_USER } });
      return;
    }
    if (pathname === '/api/auth/logout') {
      await route.fulfill({ status: 200, json: { message: '已登出' } });
      return;
    }
    if (pathname === '/api/files/upload') {
      await route.fulfill({
        status: 502,
        contentType: 'text/html; charset=utf-8',
        body: `<html><body><p>${LONG_GATEWAY_ERROR}</p></body></html>`,
      });
      return;
    }
    if (/^\/api\/files\/\d+\/stream$/.test(pathname)) {
      await route.fulfill({
        status: 200,
        contentType: 'audio/wav',
        headers: { 'Accept-Ranges': 'bytes' },
        body: createWavFixture(),
      });
      return;
    }
    if (/^\/api\/files\/\d+$/.test(pathname)) {
      const id = Number(pathname.split('/').at(-1));
      await route.fulfill({ json: files.find((file) => file.file_id === id) ?? files[0] });
      return;
    }
    if (pathname === '/api/files') {
      await route.fulfill({ json: { items: files, total: files.length, offset: 0, limit: 20 } });
      return;
    }
    if (/^\/api\/music\/library\/\d+$/.test(pathname)) {
      const id = Number(pathname.split('/').at(-1));
      const track = music.find((item) => item.music_id === id) ?? music[0];
      await route.fulfill({
        json: {
          music_id: track.music_id,
          title: track.title,
          artist: track.artist,
          album: track.album,
          genre: track.genre,
          duration_sec: track.duration_sec,
          files: [{
            file_id: 500 + track.music_id,
            file_hash: track.file_hash,
            file_size: track.file_size,
            content_type: track.content_type,
          }],
        },
      });
      return;
    }
    if (pathname === '/api/music/library') {
      await route.fulfill({ json: { items: music, total: music.length, offset: 0, limit: 20 } });
      return;
    }
    if (/^\/api\/users\/\d+\/playlists$/.test(pathname)) {
      await route.fulfill({ json: { playlists: [] } });
      return;
    }
    if (/^\/api\/playlists\/\d+\/items(?:\/.*)?$/.test(pathname)) {
      await route.fulfill({ json: { playlist_id: 900, items: [] } });
      return;
    }

    await route.continue();
  });
}

async function setTheme(page: Page, theme: 'light' | 'dark'): Promise<void> {
  await page.evaluate((selectedTheme) => {
    localStorage.setItem('theme', selectedTheme);
  }, theme);
  await page.emulateMedia({ colorScheme: theme, reducedMotion: 'reduce' });
  await page.reload();
  await expect(page.locator('html')).toHaveClass(theme === 'dark' ? /\bdark\b/ : /^(?!.*\bdark\b)/);
}

async function authenticate(page: Page): Promise<void> {
  await page.goto('/login');
  await page.evaluate((token) => localStorage.setItem('token', token), AUTH_TOKEN);
}

async function expectInsideViewport(locator: Locator, context: string): Promise<void> {
  await expect(locator, `${context} 不可见`).toBeVisible();
  const position = await locator.evaluate((element) => {
    const rect = element.getBoundingClientRect();
    return {
      top: rect.top,
      right: rect.right,
      bottom: rect.bottom,
      left: rect.left,
      viewportWidth: window.innerWidth,
      viewportHeight: window.innerHeight,
    };
  });
  expect(position.left, `${context} 超出视口左侧`).toBeGreaterThanOrEqual(0);
  expect(position.top, `${context} 超出视口顶部`).toBeGreaterThanOrEqual(0);
  expect(position.right, `${context} 超出视口右侧`).toBeLessThanOrEqual(position.viewportWidth + 1);
  expect(position.bottom, `${context} 超出视口底部`).toBeLessThanOrEqual(position.viewportHeight + 1);
}

async function openAuthenticatedPage(page: Page, path: string): Promise<void> {
  await authenticate(page);
  await page.goto(path);
  await expect(page).toHaveURL(new RegExp(`${path.replaceAll('/', '\\/')}$`));
}

async function expectScreenshot(page: Page, name: string): Promise<void> {
  await disableAnimations(page);
  await page.evaluate(async () => document.fonts.ready);
  await expect(page).toHaveScreenshot(name, {
    animations: 'disabled',
    caret: 'hide',
  });
}

async function disableAnimations(page: Page): Promise<void> {
  await page.evaluate(() => {
    if (document.getElementById('visual-test-disable-animations')) return;
    const style = document.createElement('style');
    style.id = 'visual-test-disable-animations';
    style.textContent = `
      *, *::before, *::after {
        animation: none !important;
        caret-color: transparent !important;
        scroll-behavior: auto !important;
        transition: none !important;
      }
    `;
    document.head.append(style);
  });
}

async function expectImagesLoaded(images: Locator, count: number, context: string): Promise<void> {
  await expect(images, `${context} 图片数量不稳定`).toHaveCount(count);
  await expect.poll(
    () => images.evaluateAll((elements) => elements.every((image) => (
      image instanceof HTMLImageElement && image.complete && image.naturalWidth > 0
    ))),
    { message: `${context} 存在未加载成功的图片` },
  ).toBe(true);
}

function createWavFixture(): Buffer {
  const sampleRate = 8_000;
  const sampleCount = 800;
  const dataSize = sampleCount * 2;
  const wav = Buffer.alloc(44 + dataSize);
  wav.write('RIFF', 0, 'ascii');
  wav.writeUInt32LE(36 + dataSize, 4);
  wav.write('WAVE', 8, 'ascii');
  wav.write('fmt ', 12, 'ascii');
  wav.writeUInt32LE(16, 16);
  wav.writeUInt16LE(1, 20);
  wav.writeUInt16LE(1, 22);
  wav.writeUInt32LE(sampleRate, 24);
  wav.writeUInt32LE(sampleRate * 2, 28);
  wav.writeUInt16LE(2, 32);
  wav.writeUInt16LE(16, 34);
  wav.write('data', 36, 'ascii');
  wav.writeUInt32LE(dataSize, 40);
  return wav;
}

test('登录入口浅色与深色主题可读且无横向溢出', async ({ page }) => {
  await page.goto('/login');
  await page.evaluate(() => localStorage.removeItem('theme'));
  await page.reload();

  const brand = page.getByLabel('Crystal Music');
  const form = page.getByRole('form', { name: '登录' });
  const themeButton = page.getByRole('button', { name: '切换到深色主题' });
  await expect(brand).toBeVisible();
  await expect(form).toBeVisible();
  await expect(themeButton).toBeVisible();
  await expectCssCustomProperty(page.locator('html'), '--color-bg', '#F0FDF4');
  await expectCssCustomProperty(page.locator('html'), '--color-bg-end', '#ECFDF5');
  await page.getByLabel('用户名').focus();
  expect(await page.getByLabel('用户名').evaluate((element) => getComputedStyle(element).outlineStyle)).not.toBe('none');
  await expectNoHorizontalOverflow(page, '浅色登录页');

  await setTheme(page, 'dark');
  await expect(brand).toBeVisible();
  await expect(form).toBeVisible();
  await expect(page.getByRole('button', { name: '切换到浅色主题' })).toBeVisible();
  await expectCssCustomProperty(page.locator('html'), '--color-bg', '#0A0F0D');
  await expectCssCustomProperty(page.locator('html'), '--color-bg-end', '#0F1A14');
  await page.getByLabel('用户名').focus();
  expect(await page.getByLabel('用户名').evaluate((element) => getComputedStyle(element).outlineStyle)).not.toBe('none');
  await expectNoHorizontalOverflow(page, '深色登录页');
});

test('桌面壳层固定区域互不遮挡', async ({ page }, testInfo) => {
  test.skip(testInfo.project.name !== 'visual-breakpoint', '仅检查 1024px 桌面断点');
  await openAuthenticatedPage(page, '/music/library');
  await page.getByRole('button', { name: '播放 Crystal Track 1' }).click();
  await expect(page).toHaveURL(/\/player\/1$/);
  await page.getByRole('link', { name: '文件', exact: true }).click();
  await expect(page).toHaveURL(/\/files$/);

  const sidebar = page.locator('aside[aria-label="主导航"]');
  const header = page.locator('header');
   const fileCard = page.getByRole('link', { name: 'crystal-dawn.mp3' });
  const contentHeading = page.getByRole('heading', { name: '文件列表' });
  const miniPlayer = page.getByLabel('迷你播放器');
  await expect(sidebar).toBeVisible();
  await expect(header).toBeVisible();
  await expect(fileCard).toBeVisible();
  await expect(miniPlayer).toBeVisible();

  await page.getByRole('link', { name: '上传', exact: true }).click();
  await expect(page).toHaveURL(/\/upload$/);
  await page.locator('input[type="file"]').setInputFiles({
    name: 'invalid-visual-fixture.txt',
    mimeType: 'text/plain',
    buffer: Buffer.from('invalid audio fixture'),
  });
  const toast = page.getByRole('alert').filter({ hasText: '个文件未能上传' });
  await expect(toast).toBeVisible();
  await page.getByRole('link', { name: '文件', exact: true }).click();
  await expect(page).toHaveURL(/\/files$/);
  await expect(toast).toBeVisible();

  await expectNoOverlap(header, sidebar, 'Header/侧栏');
  const fixedRegionSpacing = await page.evaluate(() => {
    const header = document.querySelector('header');
    const main = document.querySelector('main[aria-label="主内容"]');
    const heading = Array.from(document.querySelectorAll('main h1, main h2'))
      .find((element) => element.textContent?.trim() === '文件列表');
    const miniPlayer = document.querySelector('[aria-label="迷你播放器"]');
    if (!header || !main || !heading || !miniPlayer) return null;
    return {
      headerBottom: header.getBoundingClientRect().bottom,
      contentTop: heading.getBoundingClientRect().top,
      mainPaddingBottom: Number.parseFloat(getComputedStyle(main).paddingBottom),
      miniPlayerHeight: miniPlayer.getBoundingClientRect().height,
    };
  });
  expect(fixedRegionSpacing, '固定区域或首个正文标题不存在').not.toBeNull();
  expect(fixedRegionSpacing!.contentTop, '首个正文标题应位于 Header 下方')
    .toBeGreaterThanOrEqual(fixedRegionSpacing!.headerBottom);
  expect(fixedRegionSpacing!.mainPaddingBottom, 'main 底部 padding 应足以容纳 mini player')
    .toBeGreaterThanOrEqual(fixedRegionSpacing!.miniPlayerHeight);
  await expect(contentHeading).toBeVisible();
  await expectNoOverlap(toast, header, 'Toast/Header');
  await expectNoOverlap(toast, miniPlayer, 'Toast/mini player');
});

test('移动抽屉保留 Escape、遮罩和焦点恢复', async ({ page }, testInfo) => {
  test.skip(testInfo.project.name !== 'visual-mobile-small', '仅检查 375px 移动断点');
  await openAuthenticatedPage(page, '/files');
  const menuButton = page.getByRole('button', { name: '打开导航菜单' });
  const drawer = page.getByRole('dialog', { name: '主导航' });
  const backdrop = page.getByRole('button', { name: '关闭导航遮罩' });

  await expect(drawer).toBeHidden();
  await menuButton.click();
  await expect(drawer).toBeVisible();
  await expect(backdrop).toBeVisible();
  await expect(page.locator('body')).toHaveCSS('overflow', 'hidden');
  await page.keyboard.press('Escape');
  await expect(drawer).toBeHidden();
  await expect(menuButton).toBeFocused();
  await expect(page.locator('body')).not.toHaveCSS('overflow', 'hidden');

  await menuButton.click();
  await expect(drawer).toBeVisible();
  await backdrop.click({ position: { x: 370, y: 400 } });
  await expect(drawer).toBeHidden();
  await expect(menuButton).toBeFocused();
  await expect(page.locator('body')).not.toHaveCSS('overflow', 'hidden');
});

test('音乐库只加载四张本地 WebP 且无失败图片', async ({ page }) => {
  const imageRequests: string[] = [];
  const assetRequests: string[] = [];
  page.on('request', (request) => {
    if (request.resourceType() === 'image') imageRequests.push(request.url());
    if (request.resourceType() === 'image' || request.resourceType() === 'font') {
      assetRequests.push(request.url());
    }
  });
  await openAuthenticatedPage(page, '/music/library');
  const images = page.locator('img');
  await expectImagesLoaded(images, 8, '音乐库');

  const imageSources = await images.evaluateAll((elements) => elements.map((image) => (
    new URL((image as HTMLImageElement).src).pathname
  )));
  expect(new Set(imageSources)).toEqual(new Set([
    '/covers/crystal-cover-01.webp',
    '/covers/crystal-cover-02.webp',
    '/covers/crystal-cover-03.webp',
    '/covers/crystal-cover-04.webp',
  ]));
  const pageOrigin = new URL(page.url()).origin;
  expect(assetRequests.every((url) => new URL(url).origin === pageOrigin)).toBe(true);
  expect(imageRequests.every((url) => /^\/covers\/crystal-cover-0[1-4]\.webp$/.test(new URL(url).pathname))).toBe(true);
});

test('上传队列长文件名与长错误在小屏完整可读', async ({ page }, testInfo) => {
  test.skip(testInfo.project.name !== 'visual-mobile-small', '仅检查 375px 小屏布局');
  await openAuthenticatedPage(page, '/upload');
  let releaseUpload!: () => void;
  const uploadRelease = new Promise<void>((resolve) => {
    releaseUpload = resolve;
  });
  let uploadRequest: Promise<void> | undefined;
  await page.route('**/api/files/upload', async (route) => {
    uploadRequest = (async () => {
      await uploadRelease;
      await route.fulfill({
        status: 502,
        contentType: 'text/html; charset=utf-8',
        body: `<html><body><p>${LONG_GATEWAY_ERROR}</p></body></html>`,
      });
    })();
    await uploadRequest;
  });
  const observedUploadRequestPromise = page.waitForRequest((request) => (
    new URL(request.url()).pathname === '/api/files/upload'
  ));
  const observedUploadResponsePromise = page.waitForResponse((response) => (
    response.status() === 502 && new URL(response.url()).pathname === '/api/files/upload'
  ));
  const dropZone = page.getByRole('button', { name: /选择音频文件/ });
  const beforeDrag = await dropZone.boundingBox();
  await dropZone.evaluate((element) => {
    const dataTransfer = new DataTransfer();
    element.dispatchEvent(new DragEvent('dragenter', { bubbles: true, cancelable: true, dataTransfer }));
  });
  const afterDrag = await dropZone.boundingBox();
  expect(afterDrag).toEqual(beforeDrag);
  await dropZone.evaluate((element) => {
    const dataTransfer = new DataTransfer();
    element.dispatchEvent(new DragEvent('dragleave', { bubbles: true, cancelable: true, dataTransfer }));
  });

  try {
    await page.locator('input[type="file"]').setInputFiles({
      name: LONG_FILE_NAME,
      mimeType: 'audio/mpeg',
      buffer: Buffer.from('deterministic mp3 fixture'),
    });
    const queueItem = page.getByRole('listitem').filter({ hasText: LONG_FILE_NAME });
    const fileName = queueItem.getByText(LONG_FILE_NAME, { exact: true });
    const progressbar = queueItem.getByRole('progressbar');
    await expect(fileName).toBeVisible();
    await expect(progressbar).toHaveAttribute('data-status', 'uploading');
    const cancelButton = queueItem.getByRole('button', { name: new RegExp(`取消 ${LONG_FILE_NAME}`) });
    await expectInsideViewport(cancelButton, '取消按钮');
    await expectMinimumSize(cancelButton, 44, '取消按钮');

    releaseUpload();
    const errorText = queueItem.getByText(LONG_GATEWAY_ERROR, { exact: true });
    await expect(errorText).toBeVisible();
    const observedUploadRequest = await observedUploadRequestPromise;
    const observedUploadResponse = await observedUploadResponsePromise;
    expect(observedUploadRequest.method()).toBe('POST');
    expect(observedUploadResponse.status()).toBe(502);
    expect(observedUploadResponse.request().url()).toBe(observedUploadRequest.url());
    const browserError = await expectAndConsumeBrowserError(HTTP_502_CONSOLE_ERROR, '/api/files/upload');
    expect(browserError.url).toBe(observedUploadRequest.url());
    expect(browserError.url).toBe(observedUploadResponse.url());
    await expectInsideViewport(fileName, '完整长文件名');
    await expectInsideViewport(errorText, '完整网关错误正文');
    await expectInsideViewport(progressbar, '上传进度条');
    const retryButton = queueItem.getByRole('button', { name: new RegExp(`重试 ${LONG_FILE_NAME}`) });
    const removeButton = queueItem.getByRole('button', { name: new RegExp(`移除 ${LONG_FILE_NAME}`) });
    await expectInsideViewport(retryButton, '重试按钮');
    await expectInsideViewport(removeButton, '移除按钮');
    await expectMinimumSize(retryButton, 44, '重试按钮');
    await expectMinimumSize(removeButton, 44, '移除按钮');
    await expectNoHorizontalOverflow(page, '小屏上传队列');
  } finally {
    releaseUpload();
    await uploadRequest?.catch(() => undefined);
  }
});

test('全屏播放器封面、时间轴和控制区完整可见', async ({ page }) => {
  await openAuthenticatedPage(page, '/player/5');
  const player = page.getByRole('region', { name: '音乐播放器' });
  await expect(player.locator('audio')).toHaveCount(1);
  await expect(player.locator('img')).toBeVisible();
  await expect(player.getByRole('heading', { name: 'Crystal Track 5' })).toBeVisible();
  await expect(player.getByText('Visual Artist 5', { exact: true })).toBeVisible();
  await expect(player.getByRole('slider', { name: '播放进度' })).toBeVisible();
  await expect(player.getByRole('button', { name: '上一首' })).toBeVisible();
  await expect(player.getByRole('button', { name: '播放' })).toBeVisible();
  await expect(player.getByRole('button', { name: '下一首' })).toBeVisible();
  await expect(player.getByRole('slider', { name: '音量' })).toBeVisible();

  for (const [name, locator] of [
    ['播放器封面', player.locator('img')],
    ['标题', player.getByRole('heading', { name: 'Crystal Track 5' })],
    ['艺术家', player.getByText('Visual Artist 5', { exact: true })],
    ['播放进度', player.getByRole('slider', { name: '播放进度' })],
    ['上一首', player.getByRole('button', { name: '上一首' })],
    ['播放', player.getByRole('button', { name: '播放' })],
    ['下一首', player.getByRole('button', { name: '下一首' })],
    ['音量', player.getByRole('slider', { name: '音量' })],
  ] as const) {
    await expectInsideViewport(locator, name);
  }
});

test('关键页面截图', async ({ page }, testInfo: TestInfo) => {
  await page.goto('/login');
  await setTheme(page, 'light');
  await disableAnimations(page);
  await expect(page.getByLabel('Crystal Music')).toBeVisible();
  await expect(page.getByRole('form', { name: '登录' })).toBeVisible();
  await expectScreenshot(page, 'login-light.png');

  await setTheme(page, 'dark');
  await disableAnimations(page);
  await expect(page.getByLabel('Crystal Music')).toBeVisible();
  await expect(page.getByRole('button', { name: '切换到浅色主题' })).toBeVisible();
  await expectScreenshot(page, 'login-dark.png');

  await authenticate(page);
  await setTheme(page, 'light');
  await page.goto('/files');
  await disableAnimations(page);
  await expect(page.getByRole('heading', { name: '文件列表' })).toBeVisible();
   await expect(page.getByRole('link', { name: 'crystal-dawn.mp3' })).toBeVisible();
   await expect(page.getByRole('link', { name: 'emerald-night-session-with-a-long-name.flac' })).toBeVisible();
  await expectScreenshot(page, 'files.png');

  await page.goto('/music/library');
  await disableAnimations(page);
  await expect(page.locator('main[aria-label="主内容"]').getByRole('heading', { name: '音乐库', exact: true })).toBeVisible();
  await expectImagesLoaded(page.locator('img'), 8, '音乐库截图');
  await expectScreenshot(page, 'music-library.png');

  await page.goto('/upload');
  await disableAnimations(page);
  await expect(page.locator('main[aria-label="主内容"]').getByRole('heading', { name: '上传音频' })).toBeVisible();
  const observedUploadRequestPromise = page.waitForRequest((request) => (
    new URL(request.url()).pathname === '/api/files/upload'
  ));
  const observedUploadResponsePromise = page.waitForResponse((response) => (
    response.status() === 502 && new URL(response.url()).pathname === '/api/files/upload'
  ));
  await page.locator('input[type="file"]').setInputFiles({
    name: LONG_FILE_NAME,
    mimeType: 'audio/mpeg',
    buffer: Buffer.from('deterministic mp3 fixture'),
  });
  const uploadItem = page.getByRole('listitem').filter({ hasText: LONG_FILE_NAME });
  await expect(uploadItem.getByRole('progressbar')).toHaveAttribute('data-status', 'error');
  await expect(uploadItem.getByText(LONG_GATEWAY_ERROR, { exact: true })).toBeVisible();
  const observedUploadRequest = await observedUploadRequestPromise;
  const observedUploadResponse = await observedUploadResponsePromise;
  expect(observedUploadRequest.method()).toBe('POST');
  expect(observedUploadResponse.status()).toBe(502);
  expect(observedUploadResponse.request().url()).toBe(observedUploadRequest.url());
  const browserError = await expectAndConsumeBrowserError(HTTP_502_CONSOLE_ERROR, '/api/files/upload');
  expect(browserError.url).toBe(observedUploadRequest.url());
  expect(browserError.url).toBe(observedUploadResponse.url());
  await expectScreenshot(page, 'upload.png');

  await page.goto('/player/5');
  await disableAnimations(page);
  const player = page.getByRole('region', { name: '音乐播放器' });
  const playerCover = player.locator('img');
  await expect(player).toBeVisible();
  await expect(player.getByRole('heading', { name: 'Crystal Track 5' })).toBeVisible();
  await expect(player.getByText('Visual Artist 5', { exact: true })).toBeVisible();
  await expectImagesLoaded(playerCover, 1, '播放器截图');
  const audio = player.locator('audio');
  const timeline = player.getByRole('slider', { name: '播放进度' });
  await expect(audio).toHaveAttribute('src', /^blob:/);
  await expect.poll(() => audio.evaluate((element) => (
    element instanceof HTMLMediaElement
    && element.readyState >= HTMLMediaElement.HAVE_METADATA
  )), { message: '播放器截图前音频元数据未加载完成' }).toBe(true);
  const mediaDuration = await audio.evaluate((element) => (element as HTMLMediaElement).duration);
  expect(Number.isFinite(mediaDuration) && mediaDuration > 0, 'WAV 夹具应提供有效媒体时长').toBe(true);
  const expectedTimelineMaximum = Math.max(mediaDuration, 1);
  await expect.poll(async () => Number(await timeline.getAttribute('max')), {
    message: '播放器时间轴最大值尚未更新为媒体元数据对应的安全上限',
  }).toBeCloseTo(expectedTimelineMaximum, 3);
  const formattedDuration = `${Math.floor(mediaDuration / 60)}:${Math.floor(mediaDuration % 60).toString().padStart(2, '0')}`;
  await expect(timeline).toHaveAttribute('aria-valuetext', `0:00 / ${formattedDuration}`);
  await expectScreenshot(page, 'player.png');

  expect(['visual-breakpoint', 'visual-mobile-small']).toContain(testInfo.project.name);
});
