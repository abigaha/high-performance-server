# 前端开发说明

`frontend/` 是 High-Performance Server 的工作界面，当前使用 React 19、TypeScript 6、Vite 8、React Router 7、Zustand 5、Tailwind CSS 4、Oxlint、Vitest 4 和 Playwright 1.61。它提供注册登录、文件管理与音频上传、音乐库、歌单和播放器；`/users` 只如实展示后端当前未开放用户目录，不虚构管理能力。

桌面端使用固定侧栏，小于 `1024px` 时切换为可通过菜单、遮罩和 Escape 关闭的导航抽屉。核心读取页面区分加载、空数据、错误和重试状态，变更操作会防止重复提交并保留后端详细错误。

## 环境准备

需要 Node.js 20.19+ 或 22.12+ 以及 npm。首次进入目录或锁文件变化后安装依赖：

```bash
cd frontend
npm ci
```

项目质量脚本也会检查 `package.json` 与 `package-lock.json`；依赖缺失或锁文件变化时会自动执行可重复的 `npm ci`。

## 接口地址

前端通过 `VITE_API_URL` 设置后端基地址，`src/api/client.ts` 会移除末尾 `/` 后再拼接 `/api/...` 路径。

本地前后端分开运行时：

```bash
VITE_API_URL=http://127.0.0.1:9090 npm run dev
```

Vite 当前没有配置开发代理。若不设置 `VITE_API_URL`，请求将发送到当前页面的同源地址。这正是 Docker/nginx 部署所需的行为，因此生产构建通常保持该变量为空。

不要把路径写入变量：

```text
正确：http://127.0.0.1:9090
错误：http://127.0.0.1:9090/api
```

## 常用命令

以下命令在 `frontend/` 中执行：

| 命令 | 作用 | 产物或行为 |
|---|---|---|
| `npm run dev` | 启动 Vite 开发服务器和热更新 | 仅开发使用 |
| `npm run build` | TypeScript 构建检查并生成生产包 | `dist/` |
| `npm run lint` | 执行 Oxlint | 只检查前端源码 |
| `npm run test` | 以非监听模式执行 Vitest | 运行 `tests/` |
| `npm run test:e2e` | 对已部署服务执行 Playwright | 需要 nginx、后端和 MySQL |
| `npm run preview` | 本地预览已有生产包 | 需先执行 build |

推荐的本地开发方式：

```bash
# 终端 1：仓库根目录
export AUTH_SECRET="$(openssl rand -hex 48)"
xmake run high-performance-server

# 终端 2：frontend/
VITE_API_URL=http://127.0.0.1:9090 npm run dev
```

## 与根目录脚本的关系

正式质量检查应从仓库根目录调用统一脚本：

```bash
# 构建全部 C++ 目标和前端生产包
bash scripts/compile.sh build

# C++ 静态分析和完整前端 Oxlint
bash scripts/lint.sh --changed

# 全部 Google Test 和完整前端 Vitest
bash scripts/test.sh

# 包含格式化、Lint、编译、CodeQL、测试
bash scripts/pipeline.sh all
```

即使 `lint.sh --changed` 只缩小 C++ 检查范围，前端 Oxlint 仍会完整运行；指定一个后端测试目标时，前端 Vitest 也仍会完整运行。

Playwright 不加入 `scripts/test.sh`，因为它验证真实 Docker 部署并会创建带随机后缀的测试用户和音频记录。部署后运行：

```bash
bash scripts/docker.sh deploy
cd frontend
PLAYWRIGHT_BASE_URL=http://127.0.0.1:18080 npm run test:e2e
```

默认包含 `1440x900`、`1280x800`、`768x1024` 和 `390x844` 四个 Chromium 项目。本机需要指定系统浏览器时设置 `PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH`。`PLAYWRIGHT_RUN_ID`、`PLAYWRIGHT_OUTPUT_ROOT`、`PLAYWRIGHT_REPORT_ROOT` 可控制带时间戳的产物目录，禁止使用 `latest` 名称。

## 上传行为

只允许 `.mp3`、`.ogg`、`.wav`、`.flac`、`.aac`、`.m4a`、`.wma`、`.ape`、`.opus`，扩展名大小写不敏感。NORMAL 和 VIP 的默认单文件上限分别为 `10 MiB`、`100 MiB`，零字节文件会被拒绝。

选择与拖放使用同一套预检：扩展名、浏览器 MIME 冲突、零字节和角色上限。合法文件以稳定 ID 进入两路并发队列；上传期间可以继续追加，失败项可重试，进行中可取消，已结束项可移除。混合批次只会发送合法文件，并分别统计成功、失败和取消。

XHR 直接发送 `File` 原始字节，不使用 `FormData`；文件名通过 `Content-Disposition` 的 `filename*` UTF-8 参数传递。服务端仍是安全边界，会在分片写入前重复检查类型、长度和权限。前端会解析 JSON `error/message`、纯文本和 HTML 网关错误，不把详细原因替换成泛化的“上传失败”。

## 下载与播放鉴权

文件下载先用带 Bearer Token 的 `fetch` 获取 Blob，再触发浏览器下载。`<audio>` 同样无法直接附带 Bearer Header，因此播放器先获取完整音频 Blob，再使用 object URL；切歌、卸载和迟到响应都会释放 URL。当前方案不能边下载边播放大文件，长期 Range 流播需要后端提供 HttpOnly Cookie 或短期签名 URL 等适合媒体元素的认证机制。

## 目录结构

```text
frontend/
├── src/
│   ├── api/             # API 客户端与各业务请求
│   ├── components/      # 通用界面组件
│   ├── lib/             # 上传策略等纯前端规则
│   ├── pages/           # 路由页面
│   ├── stores/          # Zustand 状态
│   ├── types/           # TypeScript 数据模型
│   ├── App.tsx
│   ├── main.tsx
│   └── router.tsx
├── tests/
│   ├── api|components|lib|pages|stores/  # Vitest
│   └── e2e/             # 已部署环境的 Playwright 用例
├── public/              # 原样复制的静态资源
├── index.html
├── package.json
├── package-lock.json
├── playwright.config.ts
└── vite.config.ts
```

生产部署时 `npm run build` 生成的 `frontend/dist/` 由 nginx 提供，nginx 同时把 `/api/` 和 `/ws` 转发到 C++ 后端。前端第一版历史计划见 `plan/step-15-frontend.md`，本轮契约、响应式和测试优化见 `plan/step-17-frontend-optimization.md`。
