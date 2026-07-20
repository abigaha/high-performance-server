# Step 15 — 配套前端 Web 界面（Crystal Music）

> 基于 **React 19 + Vite 6 + TypeScript 5** 构建 SPA，对接后端 RESTful API 和 WebSocket。
> 设计体系：**Crystal Music** — 青绿翡翠色调 × Glassmorphism 玻璃态 × Frosted Glass 毛玻璃

---

## 技术栈

| 层 | 选型 | 说明 |
|---|---|---|
| 框架 | React 19 | 函数组件 + Hooks |
| 构建 | Vite 6 | 秒级 HMR，原生 TS/JSX |
| 语言 | TypeScript 5 | strict 模式 |
| 路由 | React Router v7 | SPA 路由 |
| 状态管理 | Zustand | 轻量，TS 友好 |
| HTTP 客户端 | fetch（原生） | 无额外依赖 |
| 音频播放 | Web Audio API | 浏览器原生 |
| 样式 | Tailwind CSS 4 | 原子化 CSS + 自定义设计 Token |
| 图标 | Phosphor Icons (`@phosphor-icons/react`) | 矢量图标，风格统一 |
| Toast | 自建轻量 Toast 组件 | Zustand 全局通知 store |
| 测试 (单元) | Vitest | 与 Vite 同生态 |
| 测试 (E2E) | Playwright | CI 友好 |

---

## 设计体系：Crystal Music（翠澄·雨璃）

### 设计理念

- **主题**：雨后清晨，青绿澄澈
- **核心手法**：大面积玻璃态（Glassmorphism）卡片叠加，关键导航/页脚用毛玻璃（Frosted Glass）
- **模式**：Light Mode（主）+ Dark Mode（辅），通过 Tailwind `dark:` 切换

### CSS 设计 Token

```css
:root {
  /* ── Light Mode ── */
  --color-bg: #F0FDF4;
  --color-bg-end: #ECFDF5;
  --color-primary: #059669;      /* emerald-600 */
  --color-on-primary: #FFFFFF;
  --color-accent: #10B981;       /* emerald-500 */
  --color-secondary: #34D399;    /* emerald-400 */
  --color-text: #0F172A;
  --color-text-muted: #64748B;
  --color-destructive: #EF4444;

  /* ── Glassmorphism Tokens ── */
  --glass-bg: rgba(255, 255, 255, 0.18);
  --glass-blur: 12px;
  --glass-border: rgba(255, 255, 255, 0.25);
  --glass-shadow: 0 8px 32px rgba(5, 150, 105, 0.06);

  /* ── Frosted Glass Tokens ── */
  --frosted-bg: rgba(255, 255, 255, 0.08);
  --frosted-blur: 20px;
  --frosted-border: rgba(255, 255, 255, 0.12);

  /* ── Typography ── */
  --font-display: 'Righteous', cursive;
  --font-body: 'Inter', system-ui, sans-serif;
}

.dark {
  --color-bg: #0A0F0D;
  --color-bg-end: #0F1A14;
  --color-text: #D1FAE5;
  --color-text-muted: #6EE7B7;
  --glass-bg: rgba(10, 15, 13, 0.55);
  --glass-blur: 16px;
  --glass-border: rgba(255, 255, 255, 0.06);
  --glass-shadow: 0 8px 32px rgba(0, 0, 0, 0.3);
  --frosted-bg: rgba(10, 15, 13, 0.4);
  --frosted-blur: 24px;
  --frosted-border: rgba(255, 255, 255, 0.04);
}
```

### Tailwind CSS 扩展

```javascript
// tailwind.config.ts
export default {
  theme: {
    extend: {
      colors: {
        primary: { DEFAULT: '#059669', ... },
        accent:  { DEFAULT: '#10B981', ... },
      },
      fontFamily: {
        display: ['Righteous', 'cursive'],
        body: ['Inter', 'system-ui'],
      },
      backdropBlur: {
        glass: '12px',
        frosted: '20px',
      },
      boxShadow: {
        glass: '0 8px 32px rgba(5,150,105,0.06)',
      },
    },
  },
};
```

### 公共 CSS 工具类

```css
.glass-card {
  background: var(--glass-bg);
  backdrop-filter: blur(var(--glass-blur));
  -webkit-backdrop-filter: blur(var(--glass-blur));
  border: 1px solid var(--glass-border);
  box-shadow: var(--glass-shadow);
  border-radius: 16px;
}

.frosted-bar {
  background: var(--frosted-bg);
  backdrop-filter: blur(var(--frosted-blur));
  -webkit-backdrop-filter: blur(var(--frosted-blur));
  border-bottom: 1px solid var(--frosted-border);
}

.glass-input {
  background: rgba(255, 255, 255, 0.12);
  backdrop-filter: blur(8px);
  border: 1px solid rgba(255, 255, 255, 0.2);
  border-radius: 12px;
  padding: 12px 16px;
  color: var(--color-text);
  outline: none;
  transition: border-color 0.2s, box-shadow 0.2s;
}
.glass-input:focus {
  border-color: var(--color-primary);
  box-shadow: 0 0 0 3px rgba(5, 150, 105, 0.15);
}

.glass-button {
  background: var(--color-primary);
  color: white;
  border: none;
  border-radius: 12px;
  padding: 10px 24px;
  font-weight: 500;
  cursor: pointer;
  transition: opacity 0.2s, transform 0.15s;
  backdrop-filter: blur(4px);
}
.glass-button:hover { opacity: 0.9; transform: translateY(-1px); }
.glass-button:active { transform: translateY(0); }
```

---

## 目录结构

```
frontend/
├── index.html
├── package.json
├── tsconfig.json
├── vite.config.ts
├── vitest.config.ts
├── tailwind.config.ts
├── postcss.config.js
├── public/
│   └── favicon.ico
├── src/
│   ├── main.tsx                # 入口
│   ├── App.tsx                 # 根组件
│   ├── router.tsx              # 路由配置
│   ├── index.css               # 全局样式 + 设计 Token + glass 工具类
│   ├── api/                    # API 请求封装
│   │   ├── client.ts           # fetch 封装 + token 注入 + 401 自动跳转 + Toast 错误
│   │   ├── auth.ts             # 认证相关（login / register / me / logout）
│   │   ├── files.ts            # 文件 CRUD + 上传下载
│   │   ├── music.ts            # 音乐库 + 歌单相关
│   │   └── users.ts            # 用户信息
│   ├── stores/                 # Zustand 状态
│   │   ├── auth.ts             # token / user / role / login / logout / restore
│   │   ├── player.ts           # 播放器状态（currentFile / playing / volume / seek）
│   │   ├── toast.ts            # 全局 Toast 消息（error / success / info）
│   │   └── music.ts            # 音乐库 + 当前歌单状态
│   ├── pages/
│   │   ├── LoginPage.tsx       # 登录（玻璃卡片表单）
│   │   ├── RegisterPage.tsx    # 注册
│   │   ├── FileListPage.tsx    # 文件列表（glass-card 网格）
│   │   ├── FileDetailPage.tsx  # 文件详情 + 下载
│   │   ├── UploadPage.tsx      # 拖拽上传 + 进度显示
│   │   ├── PlayerPage.tsx      # 音乐播放器页面
│   │   ├── MusicLibraryPage.tsx # 全局音乐库浏览
│   │   ├── UserPlaylistPage.tsx # 用户个人歌单管理
│   │   └── UserManagePage.tsx  # 用户管理（VIP）
│   ├── components/
│   │   ├── Layout.tsx          # 侧边栏 + 顶栏 + Outlet
│   │   ├── GuestLayout.tsx     # 未登录布局
│   │   ├── ProtectedRoute.tsx  # 认证守卫 + 角色检查
│   │   ├── Sidebar.tsx         # 侧边导航（文件/音乐库/歌单/上传/用户）
│   │   ├── Header.tsx          # 顶栏（用户信息 + 搜索 + 主题切换 + 登出）
│   │   ├── AudioPlayer.tsx     # 底部迷你播放栏 + 全屏模式
│   │   ├── FileCard.tsx        # 文件卡片（glass-card 样式）
│   │   ├── MusicCard.tsx       # 音乐卡片（封面占位 + 标题 + 艺术家 + 添加到歌单按钮）
│   │   ├── Pagination.tsx      # 分页组件
│   │   ├── Toast.tsx           # 全局 Toast 浮层
│   │   └── ThemeToggle.tsx     # Light/Dark 切换
│   └── types/
│       ├── api.ts              # 请求/响应类型
│       └── models.ts           # 领域模型（User, AuthUser, FileRecord, MusicMeta, Playlist...）
├── tests/
│   ├── setup.ts
│   ├── api/
│   │   ├── auth.test.ts        # login/register/me 测试
│   │   ├── files.test.ts       # 文件 API 测试
│   │   └── music.test.ts       # 音乐库/歌单 API 测试
│   ├── stores/
│   │   ├── auth.test.ts        # auth store 测试
│   │   └── player.test.ts      # player store 测试
│   └── e2e/
│       ├── login.spec.ts       # 登录 E2E
│       └── music.spec.ts       # 音乐库 E2E
└── Dockerfile                  # Nginx 托管静态文件
```

---

## 组件树

```
App
├── <Toast />                         (全局浮层，独立于布局)
├── GuestLayout (未登录)
│   ├── Header (仅 Logo)
│   └── <Outlet>
│       ├── LoginPage
│       │   └── glass-card 表单
│       └── RegisterPage
│           └── glass-card 表单
└── AppLayout (已登录)
    ├── Sidebar (frosted-bar)
    │   ├── 导航链接: 文件 / 音乐库 / 我的歌单 / 上传 / 用户管理(VIP)
    │   └── 主题切换 ThemeToggle
    ├── Header (frosted-bar)
    │   ├── 搜索栏 (音乐库/文件搜索)
    │   ├── 用户头像 + 用户名 + 角色标签
    │   └── 登出按钮
    ├── <Outlet>
    │   ├── FileListPage
    │   │   ├── FileCard[] (glass-card 网格)
    │   │   └── Pagination
    │   ├── FileDetailPage
    │   │   └── 元信息 + 下载/删除按钮
    │   ├── MusicLibraryPage
    │   │   ├── MusicCard[] (glass-card 网格)
    │   │   └── Pagination
    │   ├── UserPlaylistPage
    │   │   ├── 歌单列表 (glass-card)
    │   │   ├── MusicCard[] (歌单内歌曲)
    │   │   └── 移除按钮
    │   ├── UploadPage
    │   │   ├── 拖拽区域 (glass-card)
    │   │   ├── 文件选择 + 进度条 (多条)
    │   │   └── 上传结果
    │   ├── PlayerPage
    │   │   └── AudioPlayer (全屏模式)
    │   │       ├── 封面占位
    │   │       ├── 标题 / 艺术家
    │   │       ├── 进度条 (+ 拖拽 seek)
    │   │       ├── 播放/暂停 + 上一首/下一首
    │   │       └── 音量控制
    │   └── UserManagePage (VIP)
    │       └── 用户列表 (glass-card 表格)
    └── AudioPlayer (底部迷你栏, frosted-bar 固定)
```

---

## 页面清单

| 页面 | 路由 | 说明 | 认证 | 角色 |
|------|------|------|------|------|
| 登录 | `/login` | 用户名+密码 → token 存入 localStorage → 跳转 `redirect` | 否 | — |
| 注册 | `/register` | 用户名+密码+邮箱 → 注册成功跳转登录或直接登录 | 否 | — |
| 文件列表 | `/files` | 分页显示所有文件，搜索 name，切换视图 | 是 | NORMAL+ |
| 文件详情 | `/files/:id` | 文件元信息 + 下载按钮 + (VIP: 删除按钮) | 是 | NORMAL+ |
| 上传 | `/upload` | 拖拽/选择文件 → 流式上传 → 进度条 → 结果 | 是 | NORMAL+ |
| 音乐库 | `/music/library` | 全局音乐库（音频文件过滤），搜索/分页，添加到歌单 | 是 | NORMAL+ |
| 我的歌单 | `/my/music` | 个人歌单内的歌曲，可移除、排序 | 是 | NORMAL+ |
| 播放器 | `/player/:id` | 全屏播放器，Web Audio API，进度seek | 是 | NORMAL+ |
| 用户管理 | `/users` | 用户列表查看（VIP 独有） | 是 | VIP+ |

---

## 路由配置

```typescript
// src/router.tsx
const router = createBrowserRouter([
  {
    element: <GuestLayout />,
    children: [
      { path: "/login",    element: <LoginPage /> },
      { path: "/register", element: <RegisterPage /> },
    ],
  },
  {
    element: <ProtectedRoute />,
    children: [
      {
        element: <AppLayout />,
        children: [
          { path: "/files",          element: <FileListPage /> },
          { path: "/files/:id",      element: <FileDetailPage /> },
          { path: "/upload",         element: <UploadPage /> },
          { path: "/music/library",  element: <MusicLibraryPage /> },
          { path: "/my/music",       element: <UserPlaylistPage /> },
          { path: "/player/:id",     element: <PlayerPage /> },
        ],
      },
    ],
  },
  {
    element: <ProtectedRoute requiredRole="VIP" />,
    children: [
      {
        element: <AppLayout />,
        children: [
          { path: "/users", element: <UserManagePage /> },
        ],
      },
    ],
  },
]);
```

---

## API 对接清单（前端消费的后端接口）

### 认证

| 方法 | 路径 | 前端文件 | 说明 |
|------|------|---------|------|
| POST | `/api/auth/login` | `api/auth.ts` | 登录 → token + user_id + role |
| POST | `/api/auth/register` | `api/auth.ts` | 注册 → token + user_id + role（同 login 响应） |
| POST | `/api/auth/logout` | `api/auth.ts` | 登出（可选，可仅前端清除 token） |
| GET | `/api/auth/me` | `api/auth.ts` | 从 token 反查当前用户完整信息 |

### 文件

| 方法 | 路径 | 前端文件 | 说明 |
|------|------|---------|------|
| GET | `/api/files?name=&type=&offset=&limit=` | `api/files.ts` | 文件列表（加 `type=audio` 过滤音乐，加 `total` 字段） |
| GET | `/api/files/:id` | `api/files.ts` | 文件元信息 |
| GET | `/api/files/:id/download` | `api/files.ts` | 下载（by ID，浏览器直接打开） |
| GET | `/api/files/by-hash/:hash/download` | `api/files.ts` | 下载（by hash） |
| POST | `/api/files/upload` | `api/files.ts` | 流式上传 |
| GET | `/api/files/:id/stream` | `api/files.ts` | 音频流（支持 Range 206，用于播放器 seek） |
| DELETE | `/api/files/:id` | `api/files.ts` | 删除文件 |
| GET | `/api/files/search?q=&sort=&offset=&limit=` | `api/files.ts` | 增强搜索 |

### 音乐库 / 歌单

| 方法 | 路径 | 前端文件 | 说明 |
|------|------|---------|------|
| GET | `/api/music/library?offset=&limit=&search=` | `api/music.ts` | 全局音乐库列表 |
| GET | `/api/music/library/:id` | `api/music.ts` | 音乐详情 |
| GET | `/api/users/:id/playlists` | `api/music.ts` | 用户歌单列表 |
| POST | `/api/users/:id/playlists` | `api/music.ts` | 创建歌单 |
| GET | `/api/playlists/:id/items` | `api/music.ts` | 歌单内歌曲 |
| POST | `/api/playlists/:id/items` | `api/music.ts` | 添加歌曲 `{ music_id }` |
| DELETE | `/api/playlists/:id/items/:music_id` | `api/music.ts` | 从歌单移除 |
| PUT | `/api/playlists/:id/items/reorder` | `api/music.ts` | 排序 `{ ids: [3,1,2] }` |

### 用户

| 方法 | 路径 | 前端文件 | 说明 |
|------|------|---------|------|
| GET | `/api/users/:id` | `api/users.ts` | 用户信息 |
| PUT | `/api/users/:id` | `api/users.ts` | 编辑用户（邮箱/密码） |

### 其他

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/health` | 健康检查 |
| WS | `/ws` | WebSocket 实时通信 |

---

## API 客户端设计

```typescript
// src/api/client.ts
const API_BASE = import.meta.env.VITE_API_URL || "http://127.0.0.1:9090";

class ApiError extends Error {
  constructor(public status: number, message: string) {
    super(message);
  }
}

// 全局 toast 引用（在 main.tsx 注入）
let globalToast: { error: (msg: string) => void; success: (msg: string) => void };

export function injectToast(toast: typeof globalToast) {
  globalToast = toast;
}

async function request<T>(
  path: string,
  options: RequestInit = {},
  raw?: boolean   // true = 返回 Response 对象（用于流/下载）
): Promise<T> {
  const token = localStorage.getItem("token");
  const headers: Record<string, string> = {
    ...(options.body && !(options.body instanceof FormData)
      ? { "Content-Type": "application/json" }
      : {}),
    ...(token ? { Authorization: `Bearer ${token}` } : {}),
    ...(options.headers as Record<string, string> || {}),
  };
  const res = await fetch(`${API_BASE}${path}`, { ...options, headers });
  if (res.status === 401) {
    localStorage.removeItem("token");
    window.location.href = "/login";
    throw new ApiError(401, "未登录");
  }
  if (!res.ok) {
    const msg = `请求失败 (${res.status})`;
    globalToast?.error(msg);
    throw new ApiError(res.status, msg);
  }
  if (raw) return res as unknown as T;
  return res.json();
}
```

---

## Zustand Store 设计

### auth store

```typescript
interface AuthState {
  token: string | null;
  user: {
    user_id: number;
    username: string;
    email: string;
    role: 'GUEST' | 'NORMAL' | 'VIP';
  } | null;
  loading: boolean;
  login: (username: string, password: string) => Promise<void>;
  register: (username: string, password: string, email: string) => Promise<void>;
  logout: () => void;
  restore: () => Promise<void>;   // 页面刷新后从 token 恢复用户信息
}
```

### player store

```typescript
interface PlayerState {
  currentTrack: MusicMeta | null;
  playlist: MusicMeta[];          // 当前播放列表
  playlistIndex: number;
  playing: boolean;
  currentTime: number;
  duration: number;
  volume: number;
  play: (track: MusicMeta, list?: MusicMeta[]) => void;
  pause: () => void;
  resume: () => void;
  seek: (time: number) => void;
  next: () => void;
  prev: () => void;
  setVolume: (vol: number) => void;
}
```

### toast store

```typescript
interface ToastState {
  messages: { id: number; type: 'success' | 'error' | 'info'; text: string }[];
  success: (text: string) => void;
  error: (text: string) => void;
  info: (text: string) => void;
}
```

### music store

```typescript
interface MusicState {
  library: MusicMeta[];           // 当前浏览的音乐库
  libraryTotal: number;
  currentPlaylist: { id: number; name: string; items: MusicMeta[] } | null;
  userPlaylists: { id: number; name: string; itemCount: number }[];
  fetchLibrary: (offset: number, limit: number, search?: string) => Promise<void>;
  fetchPlaylists: (userId: number) => Promise<void>;
  addToPlaylist: (playlistId: number, musicId: number) => Promise<void>;
  removeFromPlaylist: (playlistId: number, musicId: number) => Promise<void>;
  reorderPlaylist: (playlistId: number, ids: number[]) => Promise<void>;
}
```

---

## TypeScript 类型定义

```typescript
// src/types/models.ts
interface AuthUser {
  user_id: number;
  username: string;
  email: string;
  role: 'GUEST' | 'NORMAL' | 'VIP';
}

interface FileRecord {
  file_id: number;
  file_name: string;
  file_hash: string;
  file_size: number;
  content_type: string;
  created_at: string;
}

interface MusicMeta {
  music_id: number;
  title: string;
  artist: string;
  album: string;
  genre: string;
  duration_sec: number;
  file_hash: string;
  file_size: number;
  content_type: string;
}

interface Playlist {
  id: number;
  user_id: number;
  name: string;
  description: string;
  item_count: number;
  created_at: string;
}

interface PlaylistItem {
  id: number;
  playlist_id: number;
  music_id: number;
  title: string;
  artist: string;
  file_hash: string;
  sort_order: number;
  added_at: string;
}

interface PaginatedResponse<T> {
  items: T[];
  total: number;
  offset: number;
  limit: number;
}

interface ApiError {
  error: string;
}
```

---

## 组件 Props 接口

```typescript
// FileCard
interface FileCardProps {
  file: FileRecord;
  onDownload?: (id: number) => void;
  onDelete?: (id: number) => void;
}

// MusicCard
interface MusicCardProps {
  music: MusicMeta;
  inPlaylist?: boolean;
  onPlay: (music: MusicMeta) => void;
  onAddToPlaylist?: (musicId: number) => void;
  onRemove?: (musicId: number) => void;
}

// Pagination
interface PaginationProps {
  current: number;
  total: number;
  pageSize: number;
  onChange: (page: number) => void;
}

// AudioPlayer
interface AudioPlayerProps {
  mode: 'mini' | 'fullscreen';
}

// ProtectedRoute
interface ProtectedRouteProps {
  requiredRole?: 'NORMAL' | 'VIP';
  children?: React.ReactNode;
}
```

---

## 开发阶段（MVPs）

### MVP 1 — 脚手架 + 认证 + 设计体系（2–3 天）

- [ ] Vite + React 19 + TypeScript 5 项目初始化
- [ ] Tailwind CSS 4 集成 + PostCSS 配置
- [ ] 设计 Token CSS 变量 + glass-card / frosted-bar 工具类
- [ ] Inter + Righteous 字体加载（Google Fonts）
- [ ] React Router v7 路由配置（GuestLayout / AppLayout / ProtectedRoute）
- [ ] Phosphor Icons 集成
- [ ] LoginPage（玻璃卡片表单，调用 POST /api/auth/login）
- [ ] RegisterPage（调用 POST /api/auth/register）
- [ ] Zustand auth store + token localStorage 持久化 + `restore()` 流程
- [ ] API client.ts 封装（token 注入 + 401 跳转 + Toast 错误）
- [ ] Zustand toast store + Toast 组件
- [ ] ThemeToggle 组件（Light/Dark 切换）
- [ ] Vitest 测试框架接入
- [ ] 测试：auth.test.ts（login/register/me/logout）

### MVP 2 — 文件管理（2–3 天）

- [ ] FileListPage（glass-card 网格 + 分页 Pagination）
- [ ] FileCard 组件
- [ ] FileDetailPage（元信息 + 下载按钮 + VIP 删除按钮）
- [ ] UploadPage（拖拽区域 + 多文件进度条 + 流式上传）
- [ ] api/files.ts 全部方法
- [ ] 测试：files.test.ts

### MVP 3 — 音乐库 + 歌单（2–3 天）

- [ ] MusicLibraryPage（全局音乐库浏览 + 搜索 + 分页）
- [ ] MusicCard 组件（封面占位色块 + 标题 + 艺术家 + 添加到歌单按钮）
- [ ] UserPlaylistPage（用户歌单列表 + 歌单内歌曲管理）
- [ ] Zustand music store
- [ ] api/music.ts 全部方法
- [ ] 测试：music.test.ts

### MVP 4 — 音乐播放器（2–3 天）

- [ ] AudioPlayer 组件（Web Audio API）
  - [ ] 底部迷你栏（frosted-bar 固定底部）：封面缩略 + 标题 + 播放/暂停 + 进度条 + 音量
  - [ ] 全屏模式（PlayerPage）：大封面占位 + 进度拖拽 seek + 上一首/下一首
- [ ] PlayerPage 路由
- [ ] Zustand player store（播放/暂停/seek/音量/播放列表）
- [ ] 音频流对接：GET /api/files/:id/stream（Range header 206）
- [ ] 测试：player.test.ts

### MVP 5 — 用户管理 + 打磨（1–2 天）

- [ ] UserManagePage（VIP 权限，用户列表）
- [ ] api/users.ts
- [ ] Playwright E2E 测试：login → 音乐库浏览 → 添加到歌单 → 播放
- [ ] 全量 UI 走查（玻璃态一致性、Dark Mode 切换流畅度）
- [ ] 响应式适配（375px / 768px / 1024px / 1440px）

---

## 质量门禁

- [ ] Vite build: 0 error
- [ ] TypeScript strict: 0 error
- [ ] Vitest 单元测试: 100% 通过
- [ ] Playwright E2E 核心流程通过
- [ ] 对接后端 API 端到端验证

---

## 部署方案

```
请求流程:
  浏览器 → Nginx (frontend/) → /api/* → 后端容器 (9090)
                            → /ws   → 后端 WebSocket
```

- Nginx 容器托管静态文件（由 Vite build 生成）
- 通过 `location /api/` 和 `location /ws` 反向代理到后端
- 生产环境 CORS 由 Nginx 统一注入
- 开发环境使用 Vite proxy 或后端直接返回 CORS 头

---

## 依赖的外部资源

| 资源 | 来源 | 说明 |
|------|------|------|
| Inter 字体 | Google Fonts | 正文字体 |
| Righteous 字体 | Google Fonts | 标题/Logo 字体 |
| Phosphor Icons | npm `@phosphor-icons/react` | 矢量图标 |
| Nginx 镜像 | 用户自行下载 | 生产部署 |
| 后端全部 API | 见 plan/step-16-backend-music.md | 后端已实现，可端到端对接 |
