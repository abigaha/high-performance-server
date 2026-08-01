import { expect, test } from '@playwright/test';
import type { APIRequestContext, Page, TestInfo } from '@playwright/test';

type Role = 'NORMAL' | 'VIP' | 'ADMIN';
type Identity = { username: string; email: string; password: string; token: string; userId: number; role: Role };
type Fixture = {
  admin: Identity;
  first: Identity;
  second: Identity;
  fileIds: Record<'admin' | 'first' | 'second', number>;
  musicIds: Record<'admin' | 'first' | 'second', number>;
  playlistIds: Record<'admin' | 'first' | 'second', number>;
  paginationUsers: string[];
};

const runId = required('E2E_RUN_ID');
let fixture: Fixture;

test.describe.configure({ mode: 'serial' });

test.beforeAll(async ({ request }) => {
  fixture = await createFixture(request);
});

function required(name: string): string {
  const value = process.env[name]?.trim();
  if (!value) throw new Error(`缺少隔离 E2E 环境变量 ${name}`);
  return value;
}

function auth(token: string) { return { Authorization: `Bearer ${token}` }; }

async function jsonRequest<T>(request: APIRequestContext, method: 'get' | 'post' | 'put' | 'delete', path: string, token?: string, data?: unknown): Promise<T> {
  const response = await request[method](path, { headers: token ? auth(token) : undefined, data });
  const text = await response.text();
  expect(response.status(), `${method.toUpperCase()} ${path}: ${text}`).toBeGreaterThanOrEqual(200);
  expect(response.status(), `${method.toUpperCase()} ${path}: ${text}`).toBeLessThan(300);
  return text ? JSON.parse(text) as T : undefined as T;
}

async function expectStatus(request: APIRequestContext, method: 'get' | 'post' | 'put' | 'delete', path: string, status: number, token?: string, data?: unknown) {
  const response = await request[method](path, { headers: token ? auth(token) : undefined, data });
  expect(response.status(), `${method.toUpperCase()} ${path}: ${await response.text()}`).toBe(status);
}

async function expectErrorCode(request: APIRequestContext, method: 'get' | 'post' | 'put' | 'delete', path: string, status: number, code: string, token: string, data?: unknown) {
  const response = await request[method](path, { headers: auth(token), data });
  const text = await response.text();
  expect(response.status(), `${method.toUpperCase()} ${path}: ${text}`).toBe(status);
  expect(JSON.parse(text), `${method.toUpperCase()} ${path}: ${text}`).toMatchObject({ code });
}

async function login(request: APIRequestContext, username: string, password: string): Promise<Identity> {
  const body = await jsonRequest<{ token: string; user: { user_id: number; username: string; email: string; role: Role } }>(
    request, 'post', '/api/auth/login', undefined, { username, password },
  );
  return { ...body.user, userId: body.user.user_id, password, token: body.token };
}

async function register(request: APIRequestContext, prefix: 'ONE' | 'TWO'): Promise<Identity> {
  const username = required(`E2E_NORMAL_${prefix}_USERNAME`);
  const email = required(`E2E_NORMAL_${prefix}_EMAIL`);
  const password = required(`E2E_NORMAL_${prefix}_PASSWORD`);
  const body = await jsonRequest<{ token: string; user: { user_id: number; username: string; email: string; role: Role } }>(
    request, 'post', '/api/auth/register', undefined, { username, email, password },
  );
  expect(body.user.role).toBe('NORMAL');
  return { ...body.user, userId: body.user.user_id, password, token: body.token };
}

function wav(seed: number): Buffer {
  const samples = 80_000;
  const value = Buffer.alloc(44 + samples * 2);
  value.write('RIFF', 0); value.writeUInt32LE(36 + samples * 2, 4); value.write('WAVE', 8);
  value.write('fmt ', 12); value.writeUInt32LE(16, 16); value.writeUInt16LE(1, 20); value.writeUInt16LE(1, 22);
  value.writeUInt32LE(8000, 24); value.writeUInt32LE(16000, 28); value.writeUInt16LE(2, 32); value.writeUInt16LE(16, 34);
  value.write('data', 36); value.writeUInt32LE(samples * 2, 40);
  for (let index = 44; index < value.length; index += 2) value.writeInt16LE((index * seed) % 30000, index);
  return value;
}

async function upload(request: APIRequestContext, identity: Identity, label: string, seed: number) {
  const name = `${label}_${runId}.wav`;
  const response = await request.post('/api/files/upload', {
    headers: { ...auth(identity.token), 'Content-Type': 'audio/wav', 'Content-Disposition': `attachment; filename="${name}"` },
    data: wav(seed),
  });
  const text = await response.text();
  expect(response.status(), text).toBe(201);
  const fileId = (JSON.parse(text) as { file_id: number }).file_id;
  const library = await jsonRequest<{ items: Array<{ music_id: number; title: string }> }>(request, 'get', `/api/music/library?search=${encodeURIComponent(label)}&offset=0&limit=20`, identity.token);
  const track = library.items.find((item) => item.title.includes(label));
  expect(track, `上传 ${name} 后音乐库缺少对应曲目`).toBeDefined();
  return { fileId, musicId: track!.music_id };
}

async function createFixture(request: APIRequestContext): Promise<Fixture> {
  const admin = await login(request, required('E2E_ADMIN_USERNAME'), required('E2E_ADMIN_PASSWORD'));
  expect(admin.role).toBe('ADMIN');
  const first = await register(request, 'ONE');
  const second = await register(request, 'TWO');
  const paginationUsers: string[] = [];
  const suffix = runId.replace(/[^a-z0-9]/gi, '').slice(-8).toLowerCase();
  for (let index = 0; index < 18; index += 1) {
    const username = `p${suffix}${String(index).padStart(2, '0')}`;
    await jsonRequest(request, 'post', '/api/auth/register', undefined, {
      username,
      email: `${username}@example.test`,
      password: second.password,
    });
    paginationUsers.push(username);
  }
  const granted = await jsonRequest<{ role: Role }>(request, 'post', `/api/admin/users/${first.userId}/vip`, admin.token, { duration_days: 30 });
  expect(granted.role).toBe('VIP');
  first.role = 'VIP';

  const adminMedia = await upload(request, admin, 'admin_audio', 3);
  const firstMedia = await upload(request, first, 'first_audio', 5);
  const secondMedia = await upload(request, second, 'second_audio', 7);
  const identities = { admin, first, second };
  const media = { admin: adminMedia, first: firstMedia, second: secondMedia };
  const playlistIds = {} as Fixture['playlistIds'];
  for (const key of ['admin', 'first', 'second'] as const) {
    const playlist = await jsonRequest<{ id: number }>(request, 'post', `/api/users/${identities[key].userId}/playlists`, identities[key].token, { name: `${key}_${runId}`, description: 'isolated public API fixture' });
    playlistIds[key] = playlist.id;
    await jsonRequest(request, 'post', `/api/playlists/${playlist.id}/items`, identities[key].token, { music_id: media[key].musicId });
  }
  return {
    admin, first, second, playlistIds, paginationUsers,
    fileIds: { admin: adminMedia.fileId, first: firstMedia.fileId, second: secondMedia.fileId },
    musicIds: { admin: adminMedia.musicId, first: firstMedia.musicId, second: secondMedia.musicId },
  };
}

async function useIdentity(page: Page, identity: Identity, path: string) {
  await page.goto('/login');
  await page.evaluate(({ token }) => localStorage.setItem('token', token), identity);
  await page.goto(path);
}

async function noOverflow(page: Page, label: string) {
  const widths = await page.evaluate(() => ({ viewport: innerWidth, document: document.documentElement.scrollWidth, body: document.body.scrollWidth }));
  expect(Math.max(widths.document, widths.body), `${label} 横向溢出`).toBeLessThanOrEqual(widths.viewport + 1);
}

async function screenshot(page: Page, testInfo: TestInfo, name: string) {
  await page.screenshot({ path: testInfo.outputPath(`${runId}_${name}.png`), fullPage: true });
}

async function tabTo(page: Page, target: ReturnType<Page['locator']>) {
  for (let attempt = 0; attempt < 200; attempt += 1) {
    await page.keyboard.press('Tab');
    if (await target.evaluateAll((elements) => elements.includes(document.activeElement))) return;
  }
  throw new Error('Tab 顺序中未找到目标控件');
}

async function expectFocused(target: ReturnType<Page['locator']>) {
  await expect.poll(() => target.evaluateAll((elements) => elements.includes(document.activeElement))).toBe(true);
}

function countdownSeconds(value: string | null): number {
  const match = /^(\d+):(\d{2}):(\d{2})$/.exec(value ?? '');
  expect(match, `倒计时格式无效: ${value}`).not.toBeNull();
  return Number(match![1]) * 3600 + Number(match![2]) * 60 + Number(match![3]);
}

test('E2E-01 NORMAL 和 VIP 不能访问管理页', async ({ page }) => {
  for (const identity of [fixture.first, fixture.second]) {
    await useIdentity(page, identity, '/admin/users');
    await expect(page).toHaveURL(/\/files$/);
    await expect(page.getByRole('heading', { name: '用户管理' })).toHaveCount(0);
  }
});

test('E2E-02 ADMIN 搜索分页结果与 total 稳定', async ({ page, request }) => {
  const firstPage = await jsonRequest<{ items: unknown[]; total: number }>(request, 'get', '/api/admin/users?offset=0&limit=1', fixture.admin.token);
  const secondPage = await jsonRequest<{ items: unknown[]; total: number }>(request, 'get', '/api/admin/users?offset=1&limit=1', fixture.admin.token);
  expect(firstPage.items).toHaveLength(1); expect(secondPage.items).toHaveLength(1); expect(secondPage.total).toBe(firstPage.total);
  await useIdentity(page, fixture.admin, '/admin/users');
  await page.getByLabel('搜索用户').fill(fixture.first.username);
  const table = page.getByRole('table');
  await expect(table.getByText(fixture.first.email, { exact: true })).toBeVisible();
  await expect(table.getByText(fixture.second.email, { exact: true })).toHaveCount(0);
});

test('E2E-03 ADMIN 授予、续期、撤销即时替换完整行', async ({ page }) => {
  await useIdentity(page, fixture.admin, '/admin/users');
  await page.getByLabel('搜索用户').fill(fixture.second.username);
  const email = page.getByRole('table').getByText(fixture.second.email, { exact: true });
  await expect(email).toBeVisible();
  await page.getByRole('button', { name: `授予 ${fixture.second.username} 30 天会员` }).click();
  await expect(email.locator('xpath=ancestor::tr').getByText('VIP', { exact: true })).toBeVisible();
  // 加强：授予后 vip_status 显示"有效"或 ACTIVE，到期时间非空
  await expect(email.locator('xpath=ancestor::tr').getByText(/有效|ACTIVE/)).toBeVisible();
  const firstExpiry = await email.locator('xpath=ancestor::tr').getByRole('time').getAttribute('datetime');
  expect(firstExpiry, '授予后到期时间 datetime 应不为 null').not.toBeNull();
  await page.getByRole('button', { name: `授予 ${fixture.second.username} 30 天会员` }).click();
  await expect.poll(async () => email.locator('xpath=ancestor::tr').getByRole('time').getAttribute('datetime')).not.toBe(firstExpiry);
  // 加强：续期后新到期时间严格晚于首次授予
  const secondExpiry = await email.locator('xpath=ancestor::tr').getByRole('time').getAttribute('datetime');
  expect(secondExpiry, '续期后到期时间 datetime 应不为 null').not.toBeNull();
  expect(new Date(secondExpiry!).getTime(), '续期后到期时间应严格晚于首次授予').toBeGreaterThan(new Date(firstExpiry!).getTime());
  await page.getByRole('button', { name: `撤销 ${fixture.second.username} 的会员` }).click();
  await page.getByRole('button', { name: '确认撤销' }).click();
  await expect(email.locator('xpath=ancestor::tr').getByText('未开通', { exact: true })).toBeVisible();
  // 加强：撤销后 role 显示"普通"或 NORMAL，到期时间消失
  await expect(email.locator('xpath=ancestor::tr').getByText(/普通|NORMAL/)).toBeVisible();
  await expect(email.locator('xpath=ancestor::tr').getByRole('time')).toHaveCount(0);
});

test('E2E-04 管理员撤销后旧 Token 下一请求即时降级', async ({ request }) => {
  await jsonRequest(request, 'delete', `/api/admin/users/${fixture.first.userId}/vip`, fixture.admin.token);
  const me = await jsonRequest<{ role: Role; vip_status: string }>(request, 'get', '/api/auth/me', fixture.first.token);
  expect(me.role).toBe('NORMAL'); expect(me.vip_status).toBe('NONE'); fixture.first.role = 'NORMAL';
});

test('E2E-05 文件删除严格执行 owner、他人和 ADMIN 权限', async ({ request }) => {
  const ownerMedia = await upload(request, fixture.first, 'delete_owner_audio', 11);
  const adminMedia = await upload(request, fixture.second, 'delete_admin_audio', 13);
  await expectStatus(request, 'delete', `/api/files/${ownerMedia.fileId}`, 403, fixture.second.token);
  await expectStatus(request, 'delete', `/api/files/${ownerMedia.fileId}`, 200, fixture.first.token);
  await expectErrorCode(request, 'get', `/api/files/${ownerMedia.fileId}`, 404, 'FILE_NOT_FOUND', fixture.first.token);
  await expectStatus(request, 'delete', `/api/files/${adminMedia.fileId}`, 200, fixture.admin.token);
  await expectErrorCode(request, 'get', `/api/files/${adminMedia.fileId}`, 404, 'FILE_NOT_FOUND', fixture.admin.token);
});

test('E2E-06 歌单治理允许 owner 并拒绝跨账号', async ({ request }) => {
  const created = await jsonRequest<{ id: number; name: string }>(request, 'post', `/api/users/${fixture.first.userId}/playlists`, fixture.first.token, { name: `governance_${runId}`, description: 'owner create' });
  const ownerList = await jsonRequest<{ playlists: Array<{ id: number; name: string }> }>(request, 'get', `/api/users/${fixture.first.userId}/playlists`, fixture.first.token);
  expect(ownerList.playlists).toContainEqual(expect.objectContaining({ id: created.id, name: created.name }));
  const renamed = await jsonRequest<{ name: string }>(request, 'put', `/api/playlists/${created.id}`, fixture.first.token, { name: `renamed_governance_${runId}`, description: 'owner update' });
  expect(renamed.name).toBe(`renamed_governance_${runId}`);

  const allMusic = [fixture.musicIds.admin, fixture.musicIds.first, fixture.musicIds.second];
  for (const musicId of allMusic) {
    await jsonRequest(request, 'post', `/api/playlists/${created.id}/items`, fixture.first.token, { music_id: musicId });
  }
  const initialItems = await jsonRequest<{ items: Array<{ music_id: number }> }>(request, 'get', `/api/playlists/${created.id}/items`, fixture.first.token);
  expect(initialItems.items.map((item) => item.music_id)).toEqual(allMusic);
  await jsonRequest(request, 'delete', `/api/playlists/${created.id}/items/${fixture.musicIds.second}`, fixture.first.token);
  await jsonRequest(request, 'post', `/api/playlists/${created.id}/items`, fixture.first.token, { music_id: fixture.musicIds.second });
  const reorderedIds = [fixture.musicIds.second, fixture.musicIds.admin, fixture.musicIds.first];
  await jsonRequest(request, 'put', `/api/playlists/${created.id}/items/reorder`, fixture.first.token, { music_ids: reorderedIds });
  const reordered = await jsonRequest<{ items: Array<{ music_id: number; sort_order: number }> }>(request, 'get', `/api/playlists/${created.id}/items`, fixture.first.token);
  expect(reordered.items.map((item) => item.music_id)).toEqual(reorderedIds);
  expect(reordered.items.map((item) => item.sort_order)).toEqual([0, 1, 2]);

  const forbidden = 'PLAYLIST_OWNER_REQUIRED';
  await expectErrorCode(request, 'get', `/api/users/${fixture.first.userId}/playlists`, 403, forbidden, fixture.second.token);
  await expectErrorCode(request, 'put', `/api/playlists/${created.id}`, 403, forbidden, fixture.second.token, { name: 'forbidden', description: '' });
  await expectErrorCode(request, 'delete', `/api/playlists/${created.id}`, 403, forbidden, fixture.second.token);
  await expectErrorCode(request, 'get', `/api/playlists/${created.id}/items`, 403, forbidden, fixture.second.token);
  await expectErrorCode(request, 'post', `/api/playlists/${created.id}/items`, 403, forbidden, fixture.second.token, { music_id: fixture.musicIds.second });
  await expectErrorCode(request, 'delete', `/api/playlists/${created.id}/items/${fixture.musicIds.second}`, 403, forbidden, fixture.second.token);
  await expectErrorCode(request, 'put', `/api/playlists/${created.id}/items/reorder`, 403, forbidden, fixture.second.token, { music_ids: reorderedIds });

  await jsonRequest(request, 'delete', `/api/playlists/${created.id}`, fixture.first.token);
  await expectStatus(request, 'get', `/api/playlists/${created.id}/items`, 404, fixture.first.token);
  const fixtureRenamed = await jsonRequest<{ name: string }>(request, 'put', `/api/playlists/${fixture.playlistIds.first}`, fixture.first.token, { name: `renamed_${runId}`, description: 'owner update' });
  expect(fixtureRenamed.name).toBe(`renamed_${runId}`);
});

test('E2E-07 删除当前来源歌单后不断播且来源转 SINGLE', async ({ page }) => {
  await useIdentity(page, fixture.first, '/my/music');
  await page.getByRole('button', { name: `renamed_${runId}` }).click();
  await page.getByRole('button', { name: `播放 first_audio_${runId}` }).click();
  await expect(page).toHaveURL(new RegExp(`/player/${fixture.musicIds.first}$`));
  await page.getByRole('link', { name: '我的歌单', exact: true }).click();
  await page.getByRole('button', { name: `renamed_${runId}` }).click();
  // 加强：删除前断言迷你播放器可见且音频正在播放（paused === false）
  await expect(page.getByLabel('迷你播放器')).toBeVisible();
  const isPlayingBeforeDelete = await page.evaluate(() => {
    const audio = document.querySelector('audio');
    return audio ? !audio.paused : false;
  });
  expect(isPlayingBeforeDelete, '删除前音频应正在播放（paused === false）').toBe(true);
  page.once('dialog', (dialog) => void dialog.accept());
  await page.getByRole('button', { name: `删除歌单 renamed_${runId}` }).click();
  await expect(page.getByLabel('迷你播放器')).toBeVisible();
  await expect(page.getByLabel('迷你播放器')).toHaveAttribute('data-source', 'SINGLE');
});

test('E2E-08 移除待播曲目后下一曲跳过', async ({ page, request }) => {
  await jsonRequest(request, 'post', `/api/playlists/${fixture.playlistIds.admin}/items`, fixture.admin.token, { music_id: fixture.musicIds.first });
  await jsonRequest(request, 'post', `/api/playlists/${fixture.playlistIds.admin}/items`, fixture.admin.token, { music_id: fixture.musicIds.second });
  await useIdentity(page, fixture.admin, '/my/music');
  await page.getByRole('button', { name: `admin_${runId}` }).click();
  await page.getByRole('button', { name: `播放 admin_audio_${runId}` }).click();
  await expect(page).toHaveURL(new RegExp(`/player/${fixture.musicIds.admin}$`));
  await page.getByRole('link', { name: '我的歌单', exact: true }).click();
  await page.getByRole('button', { name: `admin_${runId}` }).click();
  await expect(page.getByLabel('迷你播放器')).toContainText(`admin_audio_${runId}`);
  await expect(page.getByRole('button', { name: '下一首' })).toBeEnabled();
  page.once('dialog', (dialog) => void dialog.accept());
  await page.getByRole('button', { name: `从歌单移除 first_audio_${runId}` }).click();
  await page.getByRole('button', { name: '下一首' }).click();
  await expect(page.getByLabel('迷你播放器')).toContainText(`second_audio_${runId}`);
});

test('E2E-09 Profile 和 Player 深链刷新可恢复', async ({ page }) => {
  const profileUsername = page.getByLabel('主内容').getByText(fixture.admin.username, { exact: true });
  await useIdentity(page, fixture.admin, '/profile');
  await expect(profileUsername).toBeVisible();
  await page.reload();
  await expect(profileUsername).toBeVisible();
  const playerHeading = page.getByRole('heading', { name: `admin_audio_${runId}` });
  await page.goto(`/player/${fixture.musicIds.admin}`);
  await expect(playerHeading).toBeVisible();
  await page.reload();
  await expect(playerHeading).toBeVisible();
});

test('E2E-10 登出后后退不恢复旧用户域状态', async ({ page }) => {
  await useIdentity(page, fixture.admin, '/my/music');
  await expect(page.getByRole('button', { name: `admin_${runId} 2 首` })).toBeVisible();
  await page.getByRole('button', { name: '退出登录' }).click();
  await page.goBack();
  await page.waitForURL(/\/login$/);
  await expect(page).toHaveURL(/\/login$/);
  await expect(page.getByText(`admin_${runId}`, { exact: true })).toHaveCount(0);
  // 加强：断言 localStorage token 已清除
  const tokenAfterLogout = await page.evaluate(() => localStorage.getItem('token'));
  expect(tokenAfterLogout, '登出后 localStorage.token 应为 null').toBeNull();
});

test('E2E-11 六视口无溢出遮挡且键盘可用', async ({ page }, testInfo) => {
  const viewports = [[375, 812], [390, 844], [768, 1024], [1024, 768], [1280, 800], [1440, 900]] as const;
  for (const [width, height] of viewports) {
    await page.setViewportSize({ width, height });
    await useIdentity(page, fixture.admin, '/admin/users');
    const heading = page.getByLabel('主内容').getByRole('heading', { name: '用户管理' });
    await expect(heading).toBeVisible();
    const search = page.getByLabel('搜索用户');
    await search.fill(fixture.second.username);
    const searchGeometry = await search.evaluate((input) => {
      const control = input.parentElement!;
      const icon = control.querySelector<SVGElement>('.admin-users-search-icon')!;
      const inputBox = input.getBoundingClientRect();
      const iconBox = icon.getBoundingClientRect();
      const styles = getComputedStyle(input);
      return {
        iconRight: iconBox.right,
        textLeft: inputBox.left + Number.parseFloat(styles.paddingLeft),
      };
    });
    expect(searchGeometry.iconRight, `${width}x${height} 搜索图标与文本内容区域重叠`).toBeLessThanOrEqual(searchGeometry.textLeft);
    await search.fill('');
    await expect(page.getByRole('button', { name: '下一页' })).toBeVisible();

    const interactivePaginationButtons = page.getByRole('navigation', { name: '分页导航' }).getByRole('button');
    if (await interactivePaginationButtons.count()) {
      for (const button of await interactivePaginationButtons.all()) {
        const box = await button.boundingBox();
        expect(box?.width, `${width}x${height} 分页按钮宽度`).toBeGreaterThanOrEqual(44);
        expect(box?.height, `${width}x${height} 分页按钮高度`).toBeGreaterThanOrEqual(44);
      }
    }

    if (width === 375) {
      await tabTo(page, search);
      await expectFocused(search);
      await page.keyboard.type(fixture.second.username);
      await expect(page.locator('article').filter({ hasText: fixture.second.email })).toBeVisible();
      const clear = page.getByRole('button', { name: '清除搜索' });
      await tabTo(page, clear);
      await expectFocused(clear);
      await page.keyboard.press('Enter');
      await expect(search).toHaveValue('');
      await expect(page.getByRole('button', { name: '下一页' })).toBeVisible();
      await tabTo(page, search);
      await expectFocused(search);
    } else if (width === 390) {
      await tabTo(page, search);
      await page.keyboard.type(fixture.second.username);
      const item = page.locator('article').filter({ hasText: fixture.second.email });
      await expect(item).toBeVisible();
      const duration = page.getByRole('button', { name: `授予 ${fixture.second.username} 90 天会员` });
      await tabTo(page, duration);
      await expectFocused(duration);
      await page.keyboard.press('Enter');
      await expect(item.getByText('VIP', { exact: true })).toBeVisible();
    } else if (width === 768 || width === 1024) {
      await tabTo(page, search);
      await page.keyboard.type(fixture.second.username);
      const row = page.getByRole('table').getByText(fixture.second.email, { exact: true }).locator('xpath=ancestor::tr');
      await expect(row).toBeVisible();
      const revoke = page.getByRole('button', { name: `撤销 ${fixture.second.username} 的会员` });
      const revokeColors = await revoke.evaluate((button) => {
        const styles = getComputedStyle(button);
        return { background: styles.backgroundColor, color: styles.color };
      });
      expect(revokeColors.background).not.toBe('rgb(5, 150, 105)');
      expect(revokeColors.color).not.toBe('rgb(220, 38, 38)');
      await tabTo(page, revoke);
      await page.keyboard.press('Enter');
      const dialog = page.getByRole('dialog', { name: '撤销会员' });
      const cancel = page.getByRole('button', { name: '取消撤销' });
      const confirm = page.getByRole('button', { name: '确认撤销' });
      await expect(dialog).toBeVisible();
      await expectFocused(cancel);
      if (width === 768) {
        await page.keyboard.press('Shift+Tab');
        await expectFocused(confirm);
        await page.keyboard.press('Tab');
        await expectFocused(cancel);
        await page.keyboard.press('Escape');
        await expect(dialog).toHaveCount(0);
        await expectFocused(revoke);
      } else {
        await page.keyboard.press('Tab');
        await expectFocused(confirm);
        await page.keyboard.press('Enter');
        await expect(dialog).toHaveCount(0);
        await expectFocused(page.getByRole('button', { name: `${fixture.second.username} 会员已撤销` }));
        await expect(row.getByText('未开通', { exact: true })).toBeVisible();
      }
    } else if (width === 1280) {
      expect(fixture.paginationUsers).toHaveLength(18);
      const next = page.getByRole('button', { name: '下一页' });
      await tabTo(page, next);
      await page.keyboard.press('Enter');
      await expect(page.getByRole('button', { name: '第 2 页' })).toHaveAttribute('aria-current', 'page');
      const previous = page.getByRole('button', { name: '上一页' });
      await tabTo(page, previous);
      await page.keyboard.press('Enter');
      await expect(page.getByRole('button', { name: '第 1 页' })).toHaveAttribute('aria-current', 'page');
      await tabTo(page, page.getByLabel('搜索用户'));
      await expectFocused(page.getByLabel('搜索用户'));
    } else {
      await tabTo(page, page.getByLabel('搜索用户'));
      await expectFocused(page.getByLabel('搜索用户'));
    }

    await noOverflow(page, `${width}x${height}`);
    await expect(page.locator(':focus-visible')).toBeVisible();
    const fixed = await page.getByRole('banner').evaluate((header) => ({ bottom: header.getBoundingClientRect().bottom }));
    const headingBox = await heading.boundingBox();
    expect(headingBox!.y).toBeGreaterThanOrEqual(fixed.bottom);
    await screenshot(page, testInfo, `governance_${width}x${height}`);
  }
});

test('E2E-12 自助激活续期更新 UTC 与倒计时且无支付', async ({ page }) => {
  await useIdentity(page, fixture.first, '/vip');
  await page.getByRole('button', { name: '激活 30 天会员' }).click();
  const expiry = page.getByText('UTC 到期').locator('..').getByRole('time');
  await expect(expiry).toHaveAttribute('datetime', /Z$/);
  const firstExpiry = await expiry.getAttribute('datetime');
  const countdown = page.getByText('剩余时间').locator('..').locator('p[aria-live="polite"]');
  const initialRemaining = countdownSeconds(await countdown.textContent());
  expect(initialRemaining).toBeGreaterThan(0);
  await expect.poll(async () => countdownSeconds(await countdown.textContent())).toBeLessThan(initialRemaining);
  const decreasedRemaining = countdownSeconds(await countdown.textContent());
  await page.getByRole('button', { name: '续期 30 天会员' }).click();
  await expect.poll(() => expiry.getAttribute('datetime')).not.toBe(firstExpiry);
  await expect(expiry).toHaveAttribute('datetime', /Z$/);
  await expect.poll(async () => countdownSeconds(await countdown.textContent())).toBeGreaterThan(decreasedRemaining);
  await expect(page.getByText(/支付|付款/)).toHaveCount(0);
});

test('E2E-13 ADMIN 无会员入口、页面拒绝且 API 返回 403', async ({ page, request }) => {
  await useIdentity(page, fixture.admin, '/files');
  await expect(page.getByRole('link', { name: '会员中心' })).toHaveCount(0);
  await page.goto('/vip'); await expect(page).toHaveURL(/\/files$/);
  await expectStatus(request, 'get', '/api/vip/plans', 403, fixture.admin.token);
  await expectStatus(request, 'get', '/api/vip/membership', 403, fixture.admin.token);
  await expectStatus(request, 'post', '/api/vip/membership/activate', 403, fixture.admin.token, { duration_days: 30 });
});
