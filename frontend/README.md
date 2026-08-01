# 前端开发说明

`frontend/` 是 High-Performance Server 的工作界面，当前使用 React 19、TypeScript 6、Vite 8、React Router 7、Zustand 5、Tailwind CSS 4、Oxlint、Vitest 4 和 Playwright 1.61。它提供注册登录、文件管理与音频上传、音乐库、歌单和播放器；`/users` 是按角色跳转的兼容入口：`ADMIN` 到 `/admin/users`，其他已登录角色到 `/files`。

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

以下开发命令在 `frontend/` 中执行：

| 命令 | 作用 | 产物或行为 |
|---|---|---|
| `npm run dev` | 启动 Vite 开发服务器和热更新 | 仅开发使用 |
| `npm run build` | TypeScript 构建检查并生成生产包 | `dist/` |
| `npm run lint` | 执行 Oxlint | 只检查前端源码 |
| `npm run test` | 以非监听模式执行 Vitest | 运行 `tests/` |
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

Playwright 通过仓库根目录的受控 E2E 入口运行。该脚本为每次执行创建隔离 Docker project、0600 临时凭据和动态公共端口，并在退出时清理容器、具名卷和临时 env：

```bash
bash scripts/test.sh e2e tests/e2e/user-governance.spec.ts
```

当前配置包含七个 Chromium 项目。本机需要指定系统浏览器时设置 `PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH`。受控入口会忽略调用者提供的 `PLAYWRIGHT_RUN_ID`，并自行生成唯一运行标识。`PLAYWRIGHT_OUTPUT_ROOT` 和 `PLAYWRIGHT_REPORT_ROOT` 可控制带时间戳产物目录的外部根路径；两个外部根路径的任一路径段只要包含大小写任意形式的 `latest`，配置即拒绝启动，最终结果和报告路径不会包含该字符串。

## Crystal Music 视觉资源

字体由 Fontsource 本地依赖提供，`@fontsource/inter` 与 `@fontsource/righteous` 的实际版本均为 `5.3.0`。入口导入 Inter `400`、`500`、`600` 三个字重和 Righteous `400` 字重；正文使用 Inter，品牌与展示标题使用 Righteous。字体文件随前端构建产物提供，不依赖远程字体请求。

`public/covers/` 中的 `crystal-cover-01.webp`、`crystal-cover-02.webp`、`crystal-cover-03.webp`、`crystal-cover-04.webp` 是本项目原创生成的四张 WebP 封面。页面不请求远程封面：正整数音乐 ID 以 `music_id % 4` 作为上述数组的索引，因而 `music_id=1` 对应第二张、`music_id=4` 对应第一张；非安全整数或非正数回退 `crystal-cover-01.webp`。

## 视觉验收

Vitest 与 Playwright 分开执行。Vitest 可在 `frontend/` 中使用 `npm run test`；Playwright 只使用仓库根目录的受控脚本入口：

```bash
bash scripts/test.sh e2e tests/e2e/visual.spec.ts
```

Playwright 的七项目矩阵如下：

| 项目 | 视口 | 用例与用途 |
|---|---:|---|
| `user-governance` | `1440x900` | `user-governance.spec.ts`，隔离 project 内的资料、会员、治理、会话和六视口流程。 |
| `desktop` | `1440x900` | `deployment.spec.ts`，真实部署核心流程与宽桌面响应式验收。 |
| `desktop-compact` | `1280x800` | `deployment.spec.ts`，紧凑桌面布局与真实流程验收。 |
| `tablet` | `768x1024` | `deployment.spec.ts`，平板抽屉、溢出与真实流程验收。 |
| `mobile` | `390x844` | `deployment.spec.ts`，移动导航、触控布局与真实流程验收。 |
| `visual-breakpoint` | `1024x768` | `visual.spec.ts`，固定夹具下的桌面断点、区域遮挡、资源和截图基线验收。 |
| `visual-mobile-small` | `375x812` | `visual.spec.ts`，固定夹具下的小屏抽屉、长文本、资源和截图基线验收。 |

截图基线必须人工审批，不能因差异自动覆盖。先通过上述受控入口运行视觉 spec 并检查失败产物中的实际图、差异图及预期图，确认变化符合视觉合同且没有遮挡、溢出、字体或资源回退后，才可另行批准受控的基线更新流程；不得以直接 npm 或 Playwright 命令绕过 E2E 脚本。

截图基线是经人工审批后纳入版本控制的规范参照，不等同于某次运行的验收证据。受控 E2E 的 `test-results/` 和 `playwright-report/` 中每次运行目录均形如 `e2e_YYYYMMDD_HHMMSS_YYYYMMDD_HHMMSS_<pid>_<random>`。前一个时间戳由 Playwright 配置生成；后一个时间戳、`<pid>` 和 `<random>` 来自脚本生成的 `run_id`。引用截图、报告或其他验收证据时必须保留完整目录名，禁止建立或引用无时间戳的 `latest` 别名。

视觉用例只增加固定夹具下的布局、图片加载和截图检查，不替代也不削弱 Step 17 的真实部署流程。`deployment.spec.ts` 中的健康检查、SPA 深链、注册、退出、登录、会话恢复、非法文件零请求、上传中追加、两项原始字节上传、Bearer 下载、已认证完整/Range 流播 `200/206`、不可满足 Range `416`、匿名流播 `401`、末尾健康检查、响应式溢出和浏览器错误断言均须保留。

2026-07-27 已完成 Step 18 最终验收。`bash scripts/lint.sh --changed` 通过，前端 Oxlint 零告警且本轮无 C++ 变更；`bash scripts/compile.sh build` 的后端与前端构建均通过，Fontsource 本地字体已进入 `dist/`；CodeQL 最终任务 `2e701c4c-70be-4f3e-96fb-b038885c98ec` 为 `critical=0`、`high=0`。`bash scripts/test.sh` 通过 Google Test `42/42`、脚本回归和 Vitest `21` 个文件 `106/106` 个用例，输出不再包含媒体 mock 噪声。

`bash scripts/docker.sh deploy` 部署健康，公共入口为 `http://127.0.0.1:18080`。Step 18 的历史六项目验收得到 `15 passed`、`3 skipped`、`0 failed`：四个真实部署项目 `4/4` 通过，两个视觉项目的全部适用结构与截图用例通过，浏览器无未处理错误。HTML 报告位于 `frontend/playwright-report/e2e_20260727_134904/index.html`，结果文件 `frontend/test-results/e2e_20260727_134904/.last-run.json` 记录为 `passed`。12 张浅色/深色、桌面/小屏截图基线已经人工检查并保留在 `frontend/tests/e2e/visual.spec.ts-snapshots/`。

真实浏览器验收期间发现并修复了两项部署级问题：移除 Google Fonts 运行时外链，确保字体仅从 Fontsource 本地构建产物加载；修正 AudioPlayer 在回收 Blob URL 时的资源解绑与 `revokeObjectURL` 竞态。两项修复均已进入最终门禁和六项目验收。

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
