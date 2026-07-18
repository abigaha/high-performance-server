# Step 15 — 配套前端 Web 界面

> 基于 **React + Vite + TypeScript** 构建 SPA，对接后端 RESTful API 和 WebSocket。

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
| 样式 | Tailwind CSS 4 | 原子化 CSS |
| 测试 (单元) | Vitest | 与 Vite 同生态 |
| 测试 (E2E) | Playwright | CI 友好 |

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
├── public/
│   └── favicon.ico
├── src/
│   ├── main.tsx
│   ├── App.tsx
│   ├── router.tsx
│   ├── api/                  # API 请求封装
│   │   ├── client.ts         # fetch 封装 + token 注入
│   │   ├── auth.ts           # POST /api/auth/login
│   │   ├── files.ts          # 文件 CRUD + 上传下载
│   │   └── users.ts          # 用户信息
│   ├── stores/               # Zustand 状态
│   │   ├── auth.ts           # token / user / role
│   │   └── player.ts         # 播放器状态
│   ├── pages/
│   │   ├── LoginPage.tsx
│   │   ├── RegisterPage.tsx
│   │   ├── FileListPage.tsx
│   │   ├── FileDetailPage.tsx
│   │   ├── UploadPage.tsx
│   │   ├── PlayerPage.tsx
│   │   └── UserManagePage.tsx
│   ├── components/           # 通用组件
│   │   ├── Layout.tsx        # 侧边栏 + 顶栏 + 路由 outlet
│   │   ├── ProtectedRoute.tsx
│   │   ├── AudioPlayer.tsx    # 播放控件
│   │   ├── FileCard.tsx
│   │   └── Pagination.tsx
│   └── types/                # TypeScript 类型定义
│       ├── api.ts            # 请求/响应类型
│       └── models.ts         # 领域模型
├── tests/
│   ├── setup.ts
│   ├── api/                  # Vitest 单元测试
│   │   └── auth.test.ts
│   └── e2e/                  # Playwright E2E
│       └── login.spec.ts
└── Dockerfile                # Nginx 托管静态文件
```

---

## 组件树

```
App
├── GuestLayout (未登录)
│   ├── LoginPage
│   └── RegisterPage
└── AppLayout (已登录)
    ├── Sidebar (导航: 文件/上传/用户/播放)
    ├── Header (用户信息 + 登出)
    ├── <Outlet>
    │   ├── FileListPage
    │   │   ├── FileCard[]
    │   │   └── Pagination
    │   ├── FileDetailPage
    │   ├── UploadPage
    │   ├── PlayerPage
    │   │   └── AudioPlayer
    │   └── UserManagePage
    └── Footer / MiniPlayer
```

---

## 页面清单

| 页面 | 路由 | 说明 | 认证要求 |
|------|------|------|---------|
| 登录 | `/login` | 用户名 + 密码 → 获取 token 存入 localStorage | 否 |
| 注册 | `/register` | 创建新用户（当前后端需补充） | 否 |
| 文件列表 | `/files` | 分页显示文件，搜索，排序 | NORMAL+ |
| 文件详情 | `/files/:id` | 文件元信息 + 下载按钮 | NORMAL+ |
| 上传 | `/upload` | 文件选择 + 上传进度 + 结果展示 | NORMAL+ |
| 音乐播放 | `/player/:id` | 音频播放器 + 进度/音量控制 | NORMAL+ |
| 用户管理 | `/users` | 用户列表/编辑（仅 VIP 可访问） | VIP+ |

---

## API 对接清单

### 需前端调整的后端缺失项

1. **注册接口**：当前无 `POST /api/users`，需后端补充
2. **CORS 头**：前后端分离需要后端添加 `Access-Control-Allow-Origin` 等 CORS 响应头
3. **播放进度存储**：如果后端需要记录播放进度，需新增 API（MVP 可先跳过）

### API 客户端设计

```typescript
// src/api/client.ts — 核心 fetch 封装
const API_BASE = import.meta.env.VITE_API_URL || "http://127.0.0.1:9090";

async function request<T>(path: string, options: RequestInit = {}): Promise<T> {
  const token = localStorage.getItem("token");
  const headers: Record<string, string> = {
    "Content-Type": "application/json",
    ...(token ? { Authorization: `Bearer ${token}` } : {}),
    ...(options.headers as Record<string, string> || {}),
  };
  const res = await fetch(`${API_BASE}${path}`, { ...options, headers });
  if (res.status === 401) {
    localStorage.removeItem("token");
    window.location.href = "/login";
    throw new Error("Unauthorized");
  }
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  return res.json();
}
```

---

## 状态管理（Zustand）

```typescript
// auth store
interface AuthState {
  token: string | null;
  user: AuthUser | null;
  login: (username: string, password: string) => Promise<void>;
  logout: () => void;
}

// player store
interface PlayerState {
  currentFile: FileInfo | null;
  playing: boolean;
  volume: number;
  play: (file: FileInfo) => void;
  pause: () => void;
  seek: (time: number) => void;
}
```

---

## 开发阶段（MVPs）

### MVP 1 — 脚手架 + 认证（1–2 天）
- [ ] Vite + React + TypeScript + Tailwind 项目初始化
- [ ] React Router 路由配置（GuestLayout / AppLayout）
- [ ] LoginPage 实现（调用 POST /api/auth/login）
- [ ] Zustand auth store + token 持久化
- [ ] ProtectedRoute 组件
- [ ] Vitest 测试框架接入 + auth API 测试

### MVP 2 — 文件上传 + 列表（2–3 天）
- [ ] UploadPage（文件选择 → 流式上传 → 进度显示）
- [ ] FileListPage（分页列表 + 搜索）
- [ ] FileDetailPage（元信息 + 下载）
- [ ] API 封装：files.ts
- [ ] 文件相关组件：FileCard, Pagination

### MVP 3 — 音乐播放（2–3 天）
- [ ] AudioPlayer 组件（Web Audio API）
- [ ] PlayerPage（文件详情 + 播放控件）
- [ ] Zustand player store（播放/暂停/进度/音量）
- [ ] 播放进度同步

### MVP 4 — 用户管理 + 打磨（1–2 天）
- [ ] UserManagePage（VIP 权限）
- [ ] 注册页面（需后端配合补充接口）
- [ ] CORS 配置后端适配
- [ ] Playwright E2E 测试（登录 → 上传 → 播放）

---

## 部署方案

```
请求流程:
  浏览器 → Nginx (frontend/) → /api/* → 后端容器 (9090)
                            → /ws   → 后端 WebSocket
```

- Nginx 容器托管静态文件（由 Vite build 生成）
- 通过 `location /api/` 和 `location /ws` 反向代理到后端
- 由用户自行下载 Nginx 镜像并配置

---

## 质量门禁

- [ ] Vite build: 0 error
- [ ] TypeScript strict: 0 error
- [ ] Vitest 单元测试: 100% 通过
- [ ] Playwright E2E 核心流程通过
- [ ] 对接后端 API 端到端验证

---

## 依赖的外部资源

| 资源 | 来源 | 说明 |
|------|------|------|
| Nginx 镜像 | 用户自行下载 | 用于部署前端静态文件 |
| 后端注册接口 | 当前缺失，需补充 | MVP 4 前需完成 |
| CORS 支持 | 后端添加中间件 | MVP 1 前需完成 |
