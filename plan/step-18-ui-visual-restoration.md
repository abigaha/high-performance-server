# Step 18：Crystal Music UI 视觉还原

> 状态：已完成（2026-07-27）。Task 1-11、12 张截图基线人工审批、Step 18 指定的分项质量门禁、Docker 部署和六项目浏览器验收均已通过。
>
> 文档地位：本文件是 Step 18 的唯一权威文档，合并原计划、需求规格和实施追踪；历史计划均按已实施事实记录，不表示待执行工作。
>
> 性质：前端视觉专项。恢复 Crystal Music（翠澄·雨璃）的青绿玻璃态与毛玻璃视觉合同，不改变业务功能、接口或 Step 17 技术修复。
>
> 视觉依据：`61b6fee:plan/step-15-frontend.md`（2026-07-20 的完整设计版本）。当前工作树中的 Step 15 已被归档压缩，不能替代该历史视觉依据。
>
> 技术基线：[Step 17：前端体验与上传链路优化](step-17-frontend-optimization.md)及其[运行时回归修复计划](bugfix-step17-runtime-regressions.md)。

## 一、目标、方案与边界

### 1.1 目标与实施方案

在不改变业务功能、接口契约、路由、状态模型或后端行为的前提下，将偏中性文件管理后台的前端还原为“雨后清晨、青绿澄澈”的音乐产品体验：浅绿环境画布承载半透明玻璃卡片，侧栏、顶栏和迷你播放器形成毛玻璃层，翡翠绿色用于品牌与操作强调。

实施采用设计令牌驱动的渐进式还原：先落地纯展示封面映射、本地素材、字体和全局材质，再依次调整访客入口、应用壳层、文件与音乐内容、上传、通知和播放器，最后以 Vitest、六项目 Playwright、截图基线和真实部署流程验收。视觉还原不等于新增功能。

技术栈保持 React 19、TypeScript 6、React Router 7、Zustand 5、Tailwind CSS 4、Phosphor Icons、Fontsource、Vitest 4、Testing Library、Playwright 1.61 和 Vite 8。

### 1.2 包含范围

- 全局色彩、字体、圆角、阴影、边框、透明度、模糊、间距、动画令牌及可访问性回退。
- 登录/注册、桌面侧栏、移动抽屉、顶栏、主题切换、用户区域、文件卡片与详情、音乐库、歌单、分页、上传队列、Toast、迷你/全屏播放器和静态音乐封面。
- 浅色/深色主题，以及 `375×812`、`390×844`、`768×1024`、`1024×768`、`1280×800`、`1440×900` 六个视口。
- Vitest 稳定语义断言、Playwright 布局与资源断言、关键截图基线、前端说明和真实验收记录。

### 1.3 排除与不变边界

- 不修改 C++ 后端、数据库、REST API、WebSocket、nginx、`xmake.lua`、前端 API、Zustand Store、路由、领域类型或上传策略。
- 不改变原始 `File` 请求体上传、`Content-Disposition` 文件名、前后端音频签名校验、Bearer 认证下载/播放、Range 流播、上传队列调度或 Blob URL 生命周期合同。
- 不新增全局搜索、文件视图切换、歌单排序、用户管理 CRUD、音乐元数据、封面 API、播放队列规则或其他业务入口。
- 不引入新 UI 框架，不迁移 Tailwind CSS 4，不使用远程图片、运行时网络字体、装饰性光球、噪声背景或营销式落地页。
- 不用视觉模拟掩盖未实现能力；用户管理页继续如实说明后端未开放用户目录。

### 1.4 必须保留的 Step 17 行为

| 范围 | 必须保持的行为 |
|---|---|
| 认证与权限 | 数字/字符串角色归一化、会话恢复等待、受保护路由和 `401` 清理语义。 |
| 上传 | 九种格式、大小/零字节及角色限额预检、原始字节、两路并发、追加、取消、重试、移除、汇总和逐项详细错误。 |
| 下载与播放 | Bearer 认证、Blob URL 生命周期、单一 `<audio>`、深链恢复、前后曲边界、完整/Range 流播和播放错误处理。 |
| 响应式与无障碍 | 抽屉开关、遮罩、Escape、滚动锁、焦点恢复、44px 触控目标、`focus-visible`、减少动画和 `aria` 语义。 |
| 状态反馈 | 加载、空数据、错误、重试、危险操作确认、忙碌态和后端详细错误。 |

## 二、视觉、资源与兼容性合同

### 2.1 色彩令牌

| 令牌 | 浅色主题 | 深色主题 | 用途 |
|---|---|---|---|
| `--color-bg` | `#F0FDF4` | `#0A0F0D` | 环境画布起始色 |
| `--color-bg-end` | `#ECFDF5` | `#0F1A14` | 环境画布辅助色 |
| `--color-primary` | `#059669` | `#34D399` | 品牌与主要操作 |
| `--color-accent` | `#10B981` | `#10B981` | 当前状态和强调 |
| `--color-secondary` | `#34D399` | `#6EE7B7` | 次级品牌层次 |
| `--color-text` | `#0F172A` | `#D1FAE5` | 主要正文 |
| `--color-text-muted` | `#52645B` | `#9DD8BB` | 次要正文 |
| `--color-destructive` | `#DC2626` | `#F87171` | 删除和错误 |
| `--color-info` | `#2563EB` | `#60A5FA` | 信息状态 |
| `--color-warning` | `#B45309` | `#FBBF24` | VIP 和警告 |

品牌色不覆盖危险、警告和信息语义；页面不得由单一青绿色占满全部层级。

### 2.2 材质与布局合同

| 要素 | 合同 |
|---|---|
| 普通 `glass-card` | 半透明背景、`12px` 背景模糊、`16px` 圆角、1px 半透明边框、轻绿色投影。 |
| 固定 `frosted-bar` | 侧栏、顶栏和迷你播放器使用 `20px` 模糊、半透明背景和清晰分层边界。 |
| 输入与主要按钮 | `12px` 圆角；输入有清晰占位文字和 3px 焦点环；按钮悬停最多上移 1px。 |
| 图标按钮 | 稳定 `44px × 44px` 触控区，使用 Phosphor 图标并提供可访问名称或 `title`。 |
| Toast、菜单、状态区 | 单层玻璃表面，不在玻璃卡片中嵌套厚重卡片。 |
| 应用壳层 | 桌面侧栏 `240px`，顶栏 `64px`；内容区与 mini player 在 `lg` 断点偏移。 |
| 固定控件 | 悬停、加载或文字变化不得引发布局位移；六视口不得横向溢出、文本越界或固定区域重叠。 |

保留 `min-width: 320px`、`overflow-x: hidden`、全局 `focus-visible` 和 `prefers-reduced-motion`。不支持 `backdrop-filter` 时使用更高不透明度背景；`forced-colors` 下使用系统画布、文字和边框，WCAG AA 与焦点可见性优先于历史透明度数值。

### 2.3 字体合同与 Google Fonts 修复

- 正文只使用本地 Inter `400/500/600`，品牌 Logo 和展示标题只使用 Righteous `400`；回退为 `system-ui, sans-serif`。
- `@fontsource/inter` 与 `@fontsource/righteous` 实际版本均为 `5.3.0`，入口导入上述四个字重，生产包字体进入 `dist/`。
- 页面正文不使用展示字体，紧凑面板标题保持信息密度，不使用超大字号。
- 真实浏览器验收曾发现 Google Fonts 运行时外链；该外链已移除并改为仅使用 Fontsource，本轮最终代码不发起远程字体请求。

### 2.4 静态视觉资源合同

- `frontend/public/covers/` 包含四张项目原创、非空的 `1200×1200` WebP：`crystal-cover-01.webp` 至 `crystal-cover-04.webp`，分别采用唱片、声波、舞台光束和琴键主题，不依赖外部版权素材或远程请求。
- 正安全整数按 `music_id % 4` 选择数组索引；`0`、负数、小数、非安全整数、NaN 和无穷值回退第一张。
- 同一曲目在音乐库、歌单、迷你播放器和全屏播放器中使用同一路径；文件卡片仍使用文件类型图标。
- 素材测试检查文件大于 4096 字节，并具有 `RIFF`/`WEBP` 文件签名。

## 三、接口合同

下列既有接口的字段、可选性和参数类型保持不变：

```ts
interface FileCardProps {
  file: FileRecord;
  onDownload?: (id: number) => void;
  onDelete?: (id: number) => void;
  downloading?: boolean;
  deleting?: boolean;
}

interface MusicCardProps {
  music: MusicMeta;
  inPlaylist?: boolean;
  onPlay: (music: MusicMeta) => void;
  onAddToPlaylist?: (musicId: number) => void;
  onRemove?: (musicId: number) => void;
  busyAction?: 'play' | 'add' | 'remove' | null;
}

interface AudioPlayerProps { mode: 'mini' | 'fullscreen'; }
interface SidebarProps {
  open: boolean;
  onClose: () => void;
  sidebarRef: RefObject<HTMLElement | null>;
}
interface HeaderProps {
  menuOpen: boolean;
  onMenuOpen: () => void;
  menuButtonRef: RefObject<HTMLButtonElement | null>;
}
```

唯一新增的纯展示公开接口为：

```ts
export function getCoverPlaceholder(musicId: number): string;
```

它接收音乐 ID，按上述规则返回仓库内 WebP 的绝对静态路径，无副作用，不访问网络、API 或 Store。

## 四、区域行为与文件职责

### 4.1 区域行为

| 区域 | 已实现的视觉与语义 | 保留行为 |
|---|---|---|
| 全局基础 | Tailwind `@theme` 映射、浅/深环境、玻璃工具类、稳定控件尺寸和可访问回退。 | 主题、320px 最小宽度、焦点、减少动画和触控尺寸。 |
| 访客入口 | `main[aria-label="访客入口"]`、品牌区、主题按钮和单层玻璃表单；首屏可操作，无营销说明。 | 字段、校验、提交锁、错误、重定向和主题持久化。 |
| 应用壳层 | `main[aria-label="主内容"]`、连续毛玻璃侧栏/顶栏、当前项左侧品牌标识。 | 桌面偏移、移动抽屉、遮罩、Escape、焦点恢复、角色和登出。 |
| 文件 | 文件名可访问全称，详情名称/哈希可换行，忙碌按钮宽度稳定，状态区无嵌套卡片。 | 下载、VIP 删除、导航、分页、加载/空/错误/重试。 |
| 音乐与歌单 | 稳定比例本地封面、可换行操作区和单层菜单。 | 300ms 防抖、防旧响应、播放、加入/移除、创建和忙碌态。 |
| 上传 | 稳定拖放区、分状态进度色与 `aria-valuetext`，375px 下长文件名和 HTML 网关错误完整可读。 | 九种格式、角色限额、两路并发、追加、取消、重试、移除和汇总。 |
| Toast | `region[aria-label="通知"]`、状态图标/边界、44px 关闭按钮，避开顶栏与 mini player。 | `aria-live`、完整长错误和显式关闭。 |
| 播放器 | mini 的 `48×48` 封面与 fullscreen 的响应式大封面共用映射；全屏为单一无嵌套表面。 | 单一 `<audio>`、认证 Blob、切歌、进度、音量、失败和 URL 回收。 |

### 4.2 新增文件职责

| 文件 | 职责 |
|---|---|
| `frontend/src/lib/coverPlaceholder.ts` | 根据音乐 ID 稳定选择本地封面的纯函数。 |
| `frontend/tests/lib/coverPlaceholder.test.ts` | 映射、边界和本地 WebP 完整性。 |
| `frontend/tests/styles/designTokens.test.ts` | 色彩、模糊、圆角和可访问回退合同。 |
| `frontend/tests/components/GuestLayout.test.tsx` | 访客入口地标、品牌与主题按钮。 |
| `frontend/tests/e2e/visual.spec.ts` | 固定 API 夹具下的主题、布局、溢出、资源和截图验收。 |
| `frontend/public/covers/crystal-cover-01.webp` 至 `04.webp` | 项目原创本地音乐封面。 |

### 4.3 修改文件职责

| 文件组 | 职责 |
|---|---|
| `frontend/package.json`、`package-lock.json`、`src/main.tsx` | 固定 Fontsource 依赖并导入选定字重。 |
| `frontend/src/index.css` | 设计令牌、玻璃材质、主题、焦点、回退和响应式基础。 |
| `GuestLayout.tsx`、登录/注册页 | 品牌入口和玻璃表单。 |
| `Layout.tsx`、`Sidebar.tsx`、`Header.tsx`、`ThemeToggle.tsx` | 毛玻璃壳层和移动抽屉视觉。 |
| `FileCard.tsx`、`Pagination.tsx`、文件页、`UserManagePage.tsx` | 文件层级、状态视觉和能力边界。 |
| `MusicCard.tsx`、音乐库、歌单页 | 静态封面、曲目信息、菜单和操作视觉。 |
| `UploadPage.tsx`、`Toast.tsx` | 拖放、队列、进度和通知视觉。 |
| `AudioPlayer.tsx`、`PlayerPage.tsx` | 全屏与迷你播放器统一视觉。 |
| 现有组件/页面测试 | 保留行为断言，只增加稳定语义或结构断言。 |
| `playwright.config.ts` | 四个完整流程项目与两个轻量视觉项目隔离。 |
| `frontend/README.md` | 真实资源、项目矩阵、截图审批和验收命令。 |

## 五、测试规格

### 5.1 Vitest

| 编号 | 场景 | 预期 |
|---|---|---|
| UT-01 | 同一合法 `music_id` 多次映射 | 始终返回同一 WebP 路径。 |
| UT-02 | 连续 ID | 在四张封面间按 `music_id % 4` 稳定循环。 |
| UT-03 | `0`、负数、小数、非安全整数、NaN、无穷值 | 返回第一张默认封面。 |
| UT-04 | ThemeToggle | `dark` class、按钮语义和持久化不变。 |
| UT-05 | 桌面/移动 Layout | 侧栏、顶栏、遮罩、抽屉和播放器区域语义不变。 |
| UT-06 | FileCard/MusicCard | 文件忙碌语义与完整名称存在；封面 `alt=""`、装饰语义和原操作行为不变。 |
| UT-07 | AudioPlayer mini/fullscreen | 同曲同封面，只挂载当前可见模式的单一 `<audio>`，Blob 生命周期回归通过。 |
| UT-08 | 上传与 Toast | 进度状态 `aria-valuetext`、完整长错误、通知区域和关闭按钮存在。 |

测试不逐字符快照 Tailwind 类名，只锁定接口、ARIA 语义、稳定结构、计算样式和几何关系。本轮无 C++ 行为变更，未新增无意义的 Google Test，但全量 Google Test 必须通过。

### 5.2 六项目 Playwright

| 项目 | 视口 | 用例与职责 |
|---|---:|---|
| `desktop` | `1440×900` | `deployment.spec.ts`，真实部署核心流程和宽桌面响应式。 |
| `desktop-compact` | `1280×800` | `deployment.spec.ts`，真实流程和紧凑桌面布局。 |
| `tablet` | `768×1024` | `deployment.spec.ts`，真实流程和平板抽屉/溢出。 |
| `mobile` | `390×844` | `deployment.spec.ts`，真实流程和移动导航/布局。 |
| `visual-breakpoint` | `1024×768` | `visual.spec.ts`，固定 API 夹具下的桌面断点、资源、遮挡和截图。 |
| `visual-mobile-small` | `375×812` | `visual.spec.ts`，固定夹具下的小屏抽屉、长文本、资源和截图。 |

`visual.spec.ts` 收集 `console.error`/`pageerror`，拦截认证、文件、音乐、歌单和文件流接口，使用固定时间、数据和禁用动画，不允许远程图片或字体。辅助断言覆盖：`scrollWidth <= innerWidth + 1`、两个 `DOMRect` 交集面积为零、主题切换和固定 API 夹具。

| 编号 | 场景 | 核心断言 |
|---|---|---|
| E2E-01 | 登录浅/深主题 | 品牌、表单、主题按钮、对应背景令牌和焦点可见；无横向溢出。 |
| E2E-02 | 桌面壳层固定区域 | Header/侧栏、Toast/Header、Toast/mini player 不重叠；正文和底部 padding 正确。 |
| E2E-03 | 移动抽屉 | 初始隐藏，打开后 dialog/遮罩和滚动锁生效；Escape/遮罩关闭并恢复焦点。 |
| E2E-04 | 音乐库资源 | 八首曲目只形成四个同源 WebP；所有 `img.naturalWidth > 0`，无远程图片。 |
| E2E-05 | 上传小屏 | drag enter 前后尺寸不变；长文件名、长 HTML 错误、按钮和进度均在视口内。 |
| E2E-06 | 全屏播放器 | 单一 audio、封面、标题、艺术家、时间轴、三个传输按钮和音量完整可见。 |
| E2E-07 | 关键截图 | 登录浅/深、文件、音乐库、上传、播放器生成固定命名基线。 |
| E2E-08 | 原真实流程 | 注册、登录、上传、Bearer 下载、完整/Range 流播、匿名拒绝、健康检查和容器重启继续通过。 |

每个视觉项目生成六张截图：`login-light.png`、`login-dark.png`、`files.png`、`music-library.png`、`upload.png`、`player.png`，共 12 张。截图只锁定关键页面和主题，结构、可见性、资源、溢出和交互由 DOM 断言负责。

## 六、Task 1-11 已完成实施追踪

各任务实施时遵循 RED → GREEN；Task 1-8 的 Vitest 全绿后才进入 E2E，截图差异经人工审批后才更新基线，业务展示修复后重新执行 Step 18 指定的分项门禁。以下复选框记录历史完成状态，不是待办。

### Task 1：本地封面映射与原创 WebP 素材

- [x] 先增加稳定映射、非法 ID 和 WebP 签名/大小测试；RED 因模块和素材缺失失败，既有测试不受影响。
- [x] 实现无副作用的 `getCoverPlaceholder`，生成四张 `1200×1200`、`-strip -quality 86` 原创 WebP；GREEN 全部通过。

### Task 2：本地字体、全局令牌和可访问材质基础

- [x] 增加十个浅/深色令牌、`12px/20px` 模糊、`16px/12px` 圆角、`backdrop-filter` 和 `forced-colors` 回退测试。
- [x] 固定两个 Fontsource `5.3.0` 依赖，导入 Inter `400/500/600` 与 Righteous `400`；重建 `index.css` 并通过测试和生产构建。

### Task 3：访客入口、登录与注册

- [x] 增加“访客入口” main、Crystal Music 品牌区和主题按钮语义测试；保留字段、唯一提交按钮和 `role="alert"`。
- [x] 实现单层玻璃入口，保持表单校验、Store、提交锁、错误和跳转不变；相关测试通过。

### Task 4：侧栏、顶栏和移动抽屉壳层

- [x] 增加“主内容”“功能导航”、菜单 `aria-controls` 地标测试，保留桌面偏移、全屏不挂 mini、路由关闭、Escape/遮罩和焦点恢复断言。
- [x] 实现 `240px` 侧栏、`64px` 顶栏和固定毛玻璃层，不改变抽屉交互；Layout/ThemeToggle 测试通过。

### Task 5：文件、详情、分页与能力边界

- [x] 增加完整文件名、下载/删除 `aria-busy`、禁用联动和单一错误区测试。
- [x] 实现稳定忙碌按钮、可换行详情/哈希、44px 分页和无嵌套状态区；UserManagePage 保留真实能力边界；测试通过。

### Task 6：音乐卡片、音乐库和歌单

- [x] 增加装饰封面、稳定路径、播放与歌单操作回归测试。
- [x] 音乐库、歌单和卡片共用封面函数，保持标题/元数据顺序、300ms 防抖、防旧响应、菜单语义和忙碌状态；测试通过。

### Task 7：上传队列与 Toast

- [x] 增加 progressbar `aria-valuetext`、通知 region、完整长错误和可访问关闭按钮测试。
- [x] 实现不改变尺寸的拖放状态、五种进度状态表达、375px 长文本布局，以及避开 Header/mini player 的单层 Toast；并发与通知测试通过。

### Task 8：迷你与全屏播放器

- [x] 增加双模式同封面、单 audio、迟到响应、切歌、卸载回收、失败、进度、音量和前后曲测试。
- [x] 实现 mini/fullscreen 共用封面和无嵌套全屏表面，不改 `getFileStreamUrl`、Store 和播放控制；播放器及 Blob 生命周期测试通过。
- [x] 后续真实浏览器验收发现 AudioPlayer 资源解绑与 Blob URL 回收存在竞态；已修复并重新通过 Step 18 指定的分项门禁。

### Task 9：六视口 Playwright 与截图门禁

- [x] 用 `testMatch` 隔离四个真实流程项目和两个固定夹具视觉项目，保留 `deployment.spec.ts` 全部 Step 17 断言。
- [x] 完成固定 API、主题、资源、无溢出、几何不重叠、移动抽屉、长错误和播放器断言；RED 阶段除截图缺失外的适用结构断言最终全部通过。
- [x] RED 浏览器运行发现并修复 Google Fonts 外链；12 张浅/深、桌面/小屏截图逐张人工审批后生成基线，再以不更新基线模式复验通过。
- [x] 四个真实部署项目 `4/4` 通过，认证、上传、下载、完整/Range 流播、健康检查和响应式断言未削弱。

### Task 10：前端说明与验收记录

- [x] `frontend/README.md` 和本文件记录 Fontsource 字重、四张原创 WebP、映射规则、六项目矩阵、测试命令、截图审批和 Step 17 保留情况。
- [x] 所有正式证据使用时间戳路径，不建立无时间戳“最新”别名。

### Task 11：Step 18 分项门禁与完成判定

- [x] `bash scripts/lint.sh --changed`：PASS，前端 Oxlint 零告警，本轮无 C++ 变更。
- [x] `bash scripts/compile.sh build`：PASS，后端与前端通过，字体和 WebP 进入生产包。
- [x] `bash scripts/codeql.sh run`：PASS，任务 `2e701c4c-70be-4f3e-96fb-b038885c98ec`，`critical=0`、`high=0`。
- [x] `bash scripts/test.sh`：PASS，Google Test `42/42`、脚本回归、Vitest `21` 文件 `106/106`，无媒体 mock 噪声。
- [x] `bash scripts/docker.sh deploy` 与六项目 E2E：PASS，部署健康，截图无未审批差异，浏览器无未处理错误。

### 6.1 依赖、所有权与冲突规则

- Task 1 是 Task 6、Task 8 的前置；Task 2 是 Task 3-8 的前置；Task 3-8 全绿后进入 Task 9；Task 10 记录资源和命令，Task 11 写入最终实绩。
- Task 2 拥有全局令牌和通用材质；Task 5-7 不重新定义全局颜色、圆角或模糊。
- Task 6 与 Task 8 共用 `getCoverPlaceholder`，不复制映射数组或建立第二套规则。
- Task 7 拥有 Toast 固定位置，Task 8 拥有 mini player 固定高度，最终不重叠由 Task 9 几何断言裁决。
- Task 9 不弱化 `deployment.spec.ts`，视觉项目与完整流程项目必须隔离。
- 任何门禁修复若触及展示代码，截图和文档证据必须复验，不沿用修复前结果。

### 6.2 规格追踪矩阵

| 规格区域 | 覆盖任务 |
|---|---|
| 色彩、材质、字体、回退、动效 | Task 2 |
| 四张本地 WebP 与纯映射接口 | Task 1、Task 6、Task 8 |
| 登录、注册、品牌入口、主题切换 | Task 3 |
| 240px 侧栏、64px 顶栏、移动抽屉、焦点恢复 | Task 4、Task 9 |
| 文件、详情、分页、状态区、能力边界 | Task 5 |
| 音乐库、歌单、菜单、播放和忙碌状态 | Task 6 |
| 九种上传格式、两路并发、长错误、Toast | Task 7、Task 9 |
| 单一 audio、Blob URL 生命周期、mini/fullscreen | Task 8 |
| 六视口、双主题、截图、固定区域和真实部署 | Task 9、Task 11 |
| README、验收记录和 Step 18 指定的分项质量门禁 | Task 10、Task 11 |

## 七、最终真实验收记录（2026-07-27）

本节记录的是 Step 18 当时实际执行的分项质量门禁，并非 `bash scripts/pipeline.sh all` 的执行记录。`lint --changed`、构建和测试结果的证据来自当时会话的完整原始输出；当前仓库仅保留本文档中的结果记录，以及带时间戳的 Playwright 报告、结果文件和截图基线产物。本文不宣称这些会话输出构成与特定 commit 或 tree 绑定的独立可审计证据，但不否定其历史执行结果。

| 阶段 | 命令 | 时间戳 | 结果 | 证据路径 |
|---|---|---|---|---|
| 变更静态检查 | `bash scripts/lint.sh --changed` | 2026-07-27（脚本未生成独立时间戳报告） | PASS；前端 Oxlint 零告警，本轮无 C++ 变更 | 当时会话完整原始输出；当前仅保留本文档记录 |
| 后端与前端构建 | `bash scripts/compile.sh build` | 2026-07-27（脚本未生成独立时间戳报告） | PASS；后端、前端均通过，本地 Fontsource 字体进入 `dist/` | 当时会话完整原始输出；当前仅保留本文档记录 |
| CodeQL | `bash scripts/codeql.sh run` | 2026-07-27（服务任务未生成本地时间戳报告） | PASS；`critical=0`、`high=0` | 任务 `2e701c4c-70be-4f3e-96fb-b038885c98ec` |
| 全量测试 | `bash scripts/test.sh` | 2026-07-27（脚本未生成独立时间戳报告） | PASS；Google Test `42/42`、脚本回归、Vitest `21` 文件 `106/106`，无媒体 mock 噪声 | 当时会话完整原始输出；当前仅保留本文档记录 |
| Docker 部署 | `bash scripts/docker.sh deploy` | 2026-07-27（脚本未生成独立时间戳报告） | PASS；健康检查通过 | `http://127.0.0.1:18080` |
| 六项目浏览器验收 | `npm --prefix frontend run test:e2e` | `20260727_134904` | PASS；`15 passed`、`3 skipped`、`0 failed` | `frontend/playwright-report/e2e_20260727_134904/index.html`；`frontend/test-results/e2e_20260727_134904/.last-run.json` |

四个真实部署项目 `4/4` 通过；两个视觉项目的全部适用结构和截图用例通过。3 项仅因项目不适用条件跳过，浏览器无未处理错误。Step 17 的健康检查、SPA 深链、认证、上传、Bearer 下载、完整/Range 流播和错误审计断言均保留。

12 张经人工检查的浅/深主题、桌面/小屏截图基线保留在 `frontend/tests/e2e/visual.spec.ts-snapshots/`。验收期间发现并修复 Google Fonts 运行时外链，以及 AudioPlayer 资源解绑与 Blob URL 回收竞态；历史执行记录显示修复后的实现重新通过上述 Step 18 分项门禁和六项目验收。

## 八、完成定义与文档同步

Step 18 已于 2026-07-27 满足以下完成条件：

1. Crystal Music 的浅绿/深绿环境、翡翠强调、层叠玻璃、本地字体和原创封面已可观察地恢复。
2. UI 改动限于视觉、布局、静态素材和必要 ARIA 补强；接口、数据流、业务逻辑与 Step 17 修复未回退。
3. 六个视口和双主题均通过可读性、焦点、无溢出、无文本越界、无按钮位移和固定区域验收。
4. 历史执行记录显示最终实现通过 Step 18 指定的 lint、构建、CodeQL、测试、Docker 部署和六项目 Playwright 分项验收；授权完整/Range 流播保持健康。
5. 12 张截图完成人工审批，README、资源合同、测试命令和带时间戳证据与真实结果一致。

文档同步已完成：`goal.md` 登记 Step 18 目标与边界，`plan/development_plan.md` 将 Step 18 标记完成并以本文件为入口，`frontend/README.md` 记录资源和验收方法。本文件现为唯一权威文档。
