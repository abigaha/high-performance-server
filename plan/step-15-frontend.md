# Step 15：配套前端 Web 界面（第一版历史计划）

> 状态：历史基线，不再作为当前开发与验收入口。
>
> 说明：本计划记录 Crystal Music 前端第一版的建设范围。第一版已完成基础页面和接口封装，但在上传协议、权限归一化、认证下载、播放器、响应式布局和真实浏览器验收方面存在缺口。当前实现与后续验收以 [Step 17：前端体验与上传链路优化](step-17-frontend-optimization.md) 为准。
>
> 当前技术基线：React 19、Vite 8、TypeScript 6、React Router 7、Zustand 5、Tailwind CSS 4、Vitest 4、Playwright 1.61。

## 一、历史目标

Step 15 的目标是在 C++ 后端之上提供一个可直接操作的 SPA，覆盖以下第一版工作流：

- 注册、登录、退出和会话恢复；
- 文件列表、文件详情、下载、删除和音频上传；
- 音乐库浏览、个人歌单和播放控制；
- VIP 路由保护；
- 深浅主题、桌面侧栏和基础通知；
- Vitest 单元测试基础设施。

这些页面和路由已经建立，但“页面存在”不等于端到端契约已经正确。第一版完成后发现的问题及其补充实现均记录在 Step 17，后续修改不得继续按本文件中的历史假设扩展。

## 二、当前技术栈

以下内容以 `frontend/package.json` 和实际配置为准：

| 层 | 当前选型 | 当前用途 |
|---|---|---|
| UI 框架 | React 19.2 | 函数组件与 Hooks |
| 构建工具 | Vite 8.1 | 开发服务器、生产构建和插件入口 |
| 类型系统 | TypeScript 6.0 | `tsc -b` 构建检查 |
| 路由 | React Router 7.18 | SPA 路由、嵌套布局和权限守卫 |
| 状态管理 | Zustand 5.0 | 认证、音乐、播放器和 Toast 状态 |
| 样式 | Tailwind CSS 4.3 | 通过 Vite 插件加载，设计 Token 位于 `src/index.css` |
| 图标 | Phosphor Icons 2.1 | 界面操作图标 |
| HTTP | 原生 `fetch` 与 `XMLHttpRequest` | 普通请求使用 `fetch`，上传使用 XHR 进度事件 |
| 单元测试 | Vitest 4.1 + Testing Library | 测试配置合并在 `vite.config.ts` |
| 浏览器验收 | Playwright 1.61 | 验证真实部署和四种视口 |
| 前端静态检查 | Oxlint 1.71 | 由前端命令和仓库质量脚本调用 |

前端目前通过 REST API 完成上述业务工作流；仓库中的 `/ws` 反向代理属于后端通道，并不是本轮前端功能的依赖。

## 三、当前目录结构

```text
frontend/
├── public/                    # 静态资源
├── src/
│   ├── api/                   # 认证、文件、音乐和用户请求
│   ├── components/            # 布局、导航、播放器、卡片和通知
│   ├── lib/                   # 上传策略等可独立测试的规则
│   ├── pages/                 # 路由页面
│   ├── stores/                # Zustand 状态
│   ├── types/                 # API 与领域类型
│   ├── App.tsx
│   ├── index.css
│   ├── main.tsx
│   └── router.tsx
├── tests/
│   ├── api/                   # API 单元测试
│   ├── components/            # 组件与壳层测试
│   ├── e2e/                   # 已部署环境 Playwright 用例
│   ├── lib/                   # 上传规则测试
│   ├── pages/                 # 页面工作流测试
│   ├── stores/                # 状态测试
│   └── setup.ts
├── index.html
├── package.json
├── package-lock.json
├── playwright.config.ts
├── tsconfig.app.json
├── tsconfig.json
├── tsconfig.node.json
└── vite.config.ts             # Vite、React、Tailwind 与 Vitest 配置
```

生产包由 `npm run build` 生成到 `frontend/dist/`。仓库根目录的 Docker 编排将该目录只读挂载给 nginx，前端目录本身不维护独立容器构建文件。

## 四、当前路由与能力

| 页面 | 路由 | 认证 | 当前行为 |
|---|---|---|---|
| 登录 | `/login` | 否 | 登录、保留后端详细错误、提交期间锁定表单 |
| 注册 | `/register` | 否 | 用户名至少 2 字符、密码至少 6 字符、邮箱浏览器校验 |
| 文件列表 | `/files` | NORMAL+ | 加载、空数据、错误重试、认证下载和 VIP 删除 |
| 文件详情 | `/files/:id` | NORMAL+ | 元信息、认证下载、VIP 删除和失败重试 |
| 上传 | `/upload` | NORMAL+ | 音频预检、两路并发、上传中追加、取消、重试和结果汇总 |
| 音乐库 | `/music/library` | NORMAL+ | 搜索、防旧响应覆盖、分页、播放和加入歌单 |
| 我的歌单 | `/my/music` | NORMAL+ | 创建歌单、查看条目、播放和确认移除 |
| 播放器 | `/player/:id` | NORMAL+ | 深链恢复、单一媒体元素、进度、音量和前后曲控制 |
| 用户管理 | `/users` | VIP | 如实说明服务端尚未开放用户目录，不伪造管理数据 |

`ProtectedRoute` 会等待令牌恢复完成后再判断用户和角色。服务端可能返回数字或字符串角色，API 层统一归一化为 `GUEST | NORMAL | VIP`，避免刷新 VIP 深链时被提前重定向。

## 五、当前接口契约

### 5.1 认证与错误

- `POST /api/auth/login`、`POST /api/auth/register` 返回令牌、用户 ID 和角色；前端兼容数字与字符串角色。
- `GET /api/auth/me` 用于刷新后的会话恢复。
- 普通受保护请求收到 `401` 时会清理失效会话；登录接口自身的 `401` 保留输入和服务端原因。
- API 客户端优先保留 JSON `error`、JSON `message`、纯文本或 HTML 网关正文中的可读错误，不用统一文案覆盖后端细节。

### 5.2 上传

当前上传不是 multipart 表单。XHR 直接发送 `File` 的原始字节，并设置：

```http
POST /api/files/upload
Authorization: Bearer <token>
Content-Type: <audio MIME>
Content-Disposition: attachment; filename="fallback"; filename*=UTF-8''<encoded-name>
```

前端和后端共同限制九种扩展名：`.mp3`、`.ogg`、`.wav`、`.flac`、`.aac`、`.m4a`、`.wma`、`.ape`、`.opus`。前端预检扩展名、明显 MIME 冲突、零字节和角色大小上限；NORMAL 默认 `10 MiB`，VIP 默认 `100 MiB`。后端在分片落盘前重复校验并拥有最终裁决权。

上传项使用稳定 ID 和两路并发调度器。进行中的批次不会锁住文件入口，用户可以继续追加文件；新增合法项进入等待队列并在有空闲槽位时自动开始。非法项不发请求，但会留在队列中展示原因。

当前类型安全边界是扩展名白名单与长度限制，尚未验证音频魔数或执行完整编解码。文件扩展名合法并不等于内容一定可播放。

### 5.3 下载与播放

下载和播放接口均需要 Bearer Token，不能把受保护地址直接交给浏览器导航或媒体元素。当前实现先通过认证 `fetch` 获取 Blob：

- 下载创建临时 object URL，触发浏览器保存后按时释放；
- 播放创建完整音频 Blob 的 object URL，切歌、卸载和迟到响应时释放；
- `/player/:id` 会恢复对应音乐详情；布局与全屏页不会同时挂载两个媒体元素；
- 播放器使用浏览器原生 `<audio>`，由 Zustand 同步播放、进度、音量和队列状态。

由于媒体内容需要先完整下载，当前实现不能利用 Range 实现边下载边播放。若要支持大文件流播，需要后端提供适合媒体元素的认证方式，例如短期签名 URL 或安全 Cookie。

## 六、设计与响应式现状

“Crystal Music”作为第一版产品名称保留，视觉实现已从大面积单色玻璃态收敛为中性双主题、清晰边框和品牌绿色操作强调：

- 卡片圆角不超过 `8px`，避免层层嵌套装饰卡片；
- 桌面在 `1024px` 及以上显示固定侧栏，小屏使用导航抽屉；
- 抽屉支持菜单、遮罩、Escape 和导航后关闭，并恢复触发按钮焦点；
- 正文、Toast、上传队列和迷你播放器使用响应式偏移；
- 全局提供 `focus-visible`、降低动画偏好和移动触控尺寸；
- 核心数据页区分加载、空数据、错误、重试和进行中状态。

## 七、第一版里程碑结果

| 第一版里程碑 | 历史结果 | 后续纠偏 |
|---|---|---|
| 脚手架、认证、主题 | 页面与状态已建立 | 修复角色归一化、恢复时序、错误保留和表单校验 |
| 文件管理 | 列表、详情、上传和删除已建立 | 修复原始字节上传、认证下载、前后端预检和队列控制 |
| 音乐库与歌单 | 浏览和歌单操作已建立 | 补充竞态保护、移动布局、忙碌状态和详细错误 |
| 音乐播放器 | 播放界面和状态已建立 | 改为认证 Blob、深链恢复、单实例和 URL 生命周期管理 |
| 响应式与验收 | 第一版覆盖不足 | 新增移动抽屉、四视口 Playwright 配置和部署验收用例 |

第一版计划中的勾选项只表示当时的开发范围，不应被引用为当前质量门禁证据。实际阶段性测试和未执行项统一记录在 Step 17。

## 八、构建、测试与部署入口

前端局部命令在 `frontend/` 下执行：

```bash
npm run dev
npm run lint
npm run build
npm run test
npm run test:e2e
```

正式质量流程必须从仓库根目录调用：

```bash
bash scripts/pipeline.sh all
```

该脚本依次执行格式化、全量 Lint、后端与前端构建、CodeQL 和全部 Google Test/Vitest。Playwright 依赖真实部署，不加入普通单元测试；部署和浏览器验收使用：

```bash
bash scripts/docker.sh deploy
cd frontend
PLAYWRIGHT_BASE_URL=http://127.0.0.1:18080 npm run test:e2e
```

应用公共入口默认是 `http://127.0.0.1:18080`。`http://localhost:8080` 是 CodeQL 服务，不是前端访问地址。

## 九、后续依据

后续开发、验收和文档维护按以下优先级读取：

1. `frontend/README.md`：当前前端安装、命令、接口和限制；
2. [Step 17：前端体验与上传链路优化](step-17-frontend-optimization.md)：第一版补充实现、测试矩阵和交付状态；
3. 本文件：仅用于追溯第一版范围与演进原因。
