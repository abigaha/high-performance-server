# Step 19：用户功能闭环、VIP 生命周期与管理员治理

> **面向执行代理：** 必须使用 `subagent-driven-development` 或 `executing-plans` 按 Task 严格串行实施，并用复选框记录进度。每个 Task 先 RED、再 GREEN；当前轮不要求执行 `git commit` 或 `git push`。
>
> **状态:** 已完成（2026-08-01）。Step 2 整体审查、Step 3-6 正式门禁与最终隔离 E2E 全部通过；已提交并 push 到 master。
>
> **采用方案：** 方案 C。VIP 会员权益与 ADMIN 治理权限完全分离；角色值只描述身份类别，所有授权通过显式能力函数完成，禁止比较枚举大小。

**目标：** 在保持现有认证、上传、音乐库和 Crystal Music 视觉基线的前提下，完成用户资料、自助 VIP、VIP 有效期、唯一特殊管理员、用户治理、文件与歌单所有权、播放队列快照及文件检索闭环。

**架构：** 数据库中的当前用户记录是授权唯一事实，Token 只证明用户 ID 和会话有效期；每次验证均回查数据库并按 UTC 计算有效身份。前端按服务端返回的显式能力显示入口，VIP 不复用管理员守卫，ADMIN 不继承会员权益。

**技术栈：** C++20、Boost.MySQL、nlohmann/json、React 19、TypeScript 6、React Router 7、Zustand 5、Vitest 4、Playwright 1.61、Google Test、xmake v3、Docker Compose。

## 一、背景

当前实现只有 `GUEST/NORMAL/VIP`，后端通过枚举大小授权，Token 携带并信任签发时角色；VIP 同时被前端当作用户管理入口和文件删除资格。用户管理页仍是能力说明，歌单缺少重命名、删除和完整所有权校验，播放器队列没有逐项来源，资料页、文件搜索过滤和登出跨 Store 清理也未闭环。

Step 19 采用方案 C：VIP 只代表有效会员，ADMIN 是环境引导的唯一特殊账号；权限由 capability 和 owner 显式判断，不从枚举序号推导。

## 二、目标与非目标

### 2.1 目标

1. 建立 `GUEST/NORMAL/VIP/ADMIN` 四类角色和显式能力模型。
2. 建立 UTC VIP 生命周期，支持 NORMAL/VIP 自助演示激活或手动续期，以及管理员授予、续期和撤销 30/90/365 天会员。
3. 启动时按三项环境变量幂等建立数据库唯一 ADMIN，并对错误配置 fail closed。
4. 每次 Token 验证回查数据库，旧 Token 不保留已撤销、过期或已改变的权限。
5. 完成管理员用户列表、搜索、分页和会员治理页面。
6. 完成用户资料、文件搜索过滤、文件删除所有权和歌单所有权治理。
7. 建立逐项播放队列快照及歌单变更后的确定性行为。
8. 完成 `/vip` 会员中心、服务端 UTC 到期时间与倒计时、深链恢复和用户域 Store 清理。
9. 建立可恢复的物理分片删除消费器及隔离、可重复的 E2E 夹具。

### 2.2 非目标

- 不接入真实支付、支付回调、自动扣费、自动续费或退款；用户和管理员手动续期属于本范围。
- 不实现用户反馈系统。
- 不提供通用管理员创建、角色编辑或设为管理员接口。
- 不允许管理员把普通用户改为 ADMIN，也不允许撤销自身 ADMIN。
- 不把 ADMIN 当作 VIP；管理员若需要会员权益，使用另一个独立 VIP 用户账号。
- 不新增测试专用 HTTP 端点、数据库直写夹具或生产可用的测试时钟。
- 不重做音频编码、文件分片上传协议、音乐元数据或 Step 18 视觉体系。

## 三、角色、会员与显式能力

### 3.1 C++ 角色与能力

```cpp
enum class UserRole : uint8_t { GUEST = 0, NORMAL = 1, VIP = 2, ADMIN = 3 };

enum class Capability : uint8_t {
  USE_AUTHENTICATED_FEATURES,
  USE_VIP_BENEFITS,
  MANAGE_USERS,
  DELETE_ANY_FILE,
};

bool has_capability(const EffectiveIdentity&, Capability) noexcept;
bool is_effective_vip(const User&, std::chrono::system_clock::time_point) noexcept;
UserRole effective_role(const User&, std::chrono::system_clock::time_point) noexcept;
std::size_t role_size_limit(UserRole effective_role, const ServerConfig&) noexcept;
```

枚举整数只用于持久化兼容，不表达包含关系。禁止 `role >= ...`、`role < ...`、转换整数后比较授权，也禁止前端按角色数组顺序授权。

- GUEST：无认证能力。
- NORMAL：具有普通登录用户能力。
- VIP：仅在 `vip_expires_at > now` 时具有会员权益；等于 now 即过期。
- ADMIN：具有普通登录、用户治理和删除任意文件能力，但 `USE_VIP_BENEFITS=false`。
- `role_size_limit(ADMIN,config)` 返回 `normal_max_size`，不得返回 `0`；有效 VIP 返回 VIP 上限，NORMAL 返回普通上限，GUEST 在上传预检前被拒绝。

### 3.2 权限矩阵

| 能力 | GUEST | NORMAL | 有效 VIP | ADMIN |
|---|---:|---:|---:|---:|
| 登录后页面、下载、流播、音乐库 | 否 | 是 | 是 | 是 |
| 普通上传上限 | 否 | 是 | 否 | 是 |
| VIP 上传上限及会员标识 | 否 | 否 | 是 | 否 |
| 自助查看、激活、续期会员 | 否 | 是 | 是 | 否 |
| 查看、修改自己的资料 | 否 | 是 | 是 | 是 |
| 管理自己的歌单 | 否 | 是 | 是 | 是 |
| 管理他人的歌单 | 否 | 否 | 否 | 否 |
| 删除自己上传的文件 | 否 | 是 | 是 | 是 |
| 删除他人上传的文件 | 否 | 否 | 否 | 是 |
| 进入管理员界面 | 否 | 否 | 否 | 是 |
| 用户列表、搜索、分页 | 否 | 否 | 否 | 是 |
| 授予、续期、撤销 VIP | 否 | 否 | 否 | 是 |
| 创建、授予或撤销 ADMIN | 否 | 否 | 否 | 否 |

## 四、数据模型、UTC 与迁移

### 4.1 用户模型

`users` 新增 `vip_expires_at DATETIME(6) NULL`，以及：

```sql
admin_slot TINYINT GENERATED ALWAYS AS
  (CASE WHEN role = 3 THEN 1 ELSE NULL END) STORED,
UNIQUE KEY uk_users_single_admin (admin_slot)
```

MySQL 允许多个 NULL、只允许一个值 1，因此数据库层最多存在一个 ADMIN。ADMIN 的 `vip_expires_at` 必须为 NULL。

```cpp
enum class VipStatus : uint8_t { NONE, ACTIVE, EXPIRED };

struct User {
  int64_t user_id{0};
  std::string username;
  std::string password_hash;
  std::string salt;
  UserRole role{UserRole::GUEST};
  std::string email;
  std::optional<std::chrono::system_clock::time_point> vip_expires_at;
  std::string created_at;
};

struct EffectiveIdentity {
  int64_t user_id{0};
  std::string username;
  UserRole role{UserRole::GUEST};
  VipStatus vip_status{VipStatus::NONE};
  std::optional<std::chrono::system_clock::time_point> vip_expires_at;
};
```

所有 API 时间使用 RFC 3339 UTC。`db/src/boost_mysql_connection.cpp` 每次新建或重连成功后执行 `SET time_zone = '+00:00'`，失败即连接失败。集中实现 `parse_mysql_utc_datetime` 与 `format_rfc3339_utc`，禁止依赖宿主机本地时区。

### 4.2 存量 Docker 数据卷迁移

存量卷不会重跑 `/docker-entrypoint-initdb.d`。新增：

```cpp
MutationResult<std::monostate> run_schema_migrations(
  IDatabasePool&, std::chrono::system_clock::time_point now);
```

启动顺序固定为：数据库池初始化 → `run_schema_migrations` → pending 物理清理 → ADMIN bootstrap → 注册路由 → HTTP 监听。

1. 先执行可重复 DDL：创建 `schema_migrations`，按 `information_schema` 探测后添加 VIP 字段、ADMIN 生成列、唯一键、索引和 pending 表。
2. MySQL DDL 会隐式提交，不宣称可事务回滚；任一步失败阻止启动，下次依靠幂等探测继续。
3. 唯一键创建时若已有多个 ADMIN，DDL 失败并阻止启动，不自动选择或降级管理员。
4. DDL 完成后开启单连接事务，锁定 `step_19_vip_lifecycle_v1` marker。
5. marker 不存在时，将当时 `role=VIP AND vip_expires_at IS NULL` 的历史 VIP 设置为 `now + 365 days`，再插入 marker，同一事务提交。
6. 回填或 marker 失败整体回滚并阻止启动；重复启动不再次延期。
7. 新库 `db/schema.sql` 直接包含最终结构，但启动期仍运行迁移以统一校验。

### 4.3 VIP 写入规则

- 时长只接受 30、90、365。
- 新到期时间为 `max(now,current vip_expires_at)+duration_days`。
- NORMAL、有效 VIP、过期 VIP 均可激活或续期；持久角色变为 VIP。
- 撤销将持久角色设为 NORMAL，并把到期时间设为 NULL。
- ADMIN 目标返回 `ADMIN_MEMBERSHIP_FORBIDDEN`，不改变字段。
- 自助端点只允许有效身份 NORMAL/VIP；ADMIN 返回 `VIP_SELF_SERVICE_UNAVAILABLE`。
- 所有更新使用事务和 `SELECT ... FOR UPDATE`，返回提交后的完整快照。

## 五、结构化数据库结果

```cpp
enum class MutationStatus : uint8_t {
  OK,
  NOT_FOUND,
  USER_NOT_FOUND,
  OWNER_REQUIRED,
  CONFLICT,
  INVALID_STATE,
  STORAGE_ERROR,
};

template <typename T> struct MutationResult {
  MutationStatus status{MutationStatus::STORAGE_ERROR};
  std::optional<T> value;
};

struct PaginatedUsers {
  std::vector<User> items;
  int total{0};
  int offset{0};
  int limit{20};
};
```

映射固定为：`USER_NOT_FOUND/NOT_FOUND -> 404`、`OWNER_REQUIRED -> 403`、`CONFLICT -> 409`、`INVALID_STATE -> 422`、`STORAGE_ERROR -> 500`。路由禁止从 optional 缺值或 bool=false 猜测业务原因。

## 六、唯一特殊管理员

启动读取 `ADMIN_USERNAME`、`ADMIN_PASSWORD`、`ADMIN_EMAIL`：

1. 三项均未设置：不创建，正常启动。
2. 部分设置、空值或格式不合格：错误日志不含密码，启动失败。
3. 查询唯一 ADMIN slot；不存在 ADMIN、用户名和邮箱均未占用时创建 ADMIN。
4. 同名账号不是 ADMIN：启动失败，绝不升级。
5. 已有 ADMIN 与配置用户名不同：`ADMIN_ACCOUNT_CONFLICT`，启动失败；改环境用户名不能创建第二个管理员。
6. 同名 ADMIN 且为 slot 用户：同步邮箱；密码不匹配时更新盐和哈希，匹配时不写库。
7. 同邮箱被其他账号占用：启动失败。
8. 并发创建由唯一键最终裁决，冲突返回结构化 CONFLICT 并阻止启动。
9. 公开注册携带 role、VIP 或管理员字段返回 `REGISTRATION_FIELD_FORBIDDEN`；结果固定 NORMAL。
10. 不提供通用 ADMIN 创建或修改 API。

Docker 开发部署缺少 `.env` 时，`scripts/docker.sh` 随现有密钥生成随机管理员用户名、至少 32 字节密码和 `.invalid` 邮箱，以 0600 写入。已有 `.env` 缺少三项时按三项全未设置处理，不擅自追加。

## 七、精确 REST API

错误统一为 `{"code":"...","error":"中文说明"}`。受保护端点使用 Bearer Token。

### 7.1 认证与资料

| 方法与路径 | 请求 | 成功响应 |
|---|---|---|
| `POST /api/auth/register` | `{username,password,email}` | `201 AuthResponse` |
| `POST /api/auth/login` | `{username,password}` | `200 AuthResponse` |
| `GET /api/auth/me` | 无 | `200 AuthUser` |
| `PUT /api/users/:id` | `{email?,password?}` | `200 AuthUser` |
| `POST /api/auth/logout` | 无 | `200 {message}` |

`AuthResponse={token,user:AuthUser}`。登录和注册不得返回顶层 user_id/role。AuthUser 的 `created_at` 必填；登录、注册、me 和 Profile 更新共享同一序列化函数。

API 的 role 始终是请求时有效角色。持久 VIP 已过期时返回 `role=NORMAL`、`vip_status=EXPIRED` 和历史到期时间；持久角色不作为第二个 API 字段暴露。

### 7.2 用户自助演示 VIP

| 方法与路径 | 请求 | 成功响应 |
|---|---|---|
| `GET /api/vip/plans` | 无 | `200 {plans:VipPlan[]}` |
| `GET /api/vip/membership` | 无 | `200 VipMembership` |
| `POST /api/vip/membership/activate` | `{duration_days:30|90|365}` | `200 VipMembership` |

`VipPlan={duration_days,label}`，不含价格、订单或支付字段。`VipMembership={role,vip_status,vip_expires_at,server_now,remaining_seconds}`；剩余秒数由服务端计算。NORMAL 首次调用为激活，VIP 调用为手动续期。ADMIN 对三个端点均返回 403。

### 7.3 管理员用户治理

| 方法与路径 | 请求/查询 | 成功响应 |
|---|---|---|
| `GET /api/admin/users?q=&offset=0&limit=20` | 用户名或邮箱查询 | `200 PaginatedResponse<AdminUserSummary>` |
| `POST /api/admin/users/:id/vip` | `{duration_days}` | `200 AdminUserSummary` |
| `DELETE /api/admin/users/:id/vip` | 无 | `200 AdminUserSummary` |

排序固定 `user_id ASC`；items 和 total 使用相同过滤条件。mutation 成功返回完整行，前端按 user_id 替换，不拼装局部结果。NORMAL/VIP 调用均为 403。

### 7.4 文件

`GET /api/files?name=<encoded>&type=<audio|image|video|other|空>&offset=0&limit=20` 是文件页唯一列表接口。后端 URL 解码并参数化查询，响应项包含 `uploaded_by` 和 `can_delete`。

`DELETE /api/files/:id` 仅上传者或 ADMIN 成功。VIP 不能删除他人文件。数据库事务锁定文件及 file_chunks，校验 owner，删除数据库记录；仅将删除后无引用的 chunk_hash 写入 pending 表；仅在无其他文件引用时删除 music_meta。事务失败整体回滚。

### 7.5 歌单

| 方法与路径 | 合同 |
|---|---|
| `GET /api/users/:id/playlists` | 仅本人 |
| `POST /api/users/:id/playlists` | 仅本人，名称 1..128 |
| `PUT /api/playlists/:id` | 仅 owner，重命名/描述 |
| `DELETE /api/playlists/:id` | 仅 owner，204 |
| `GET /api/playlists/:id/items` | 仅 owner |
| `POST /api/playlists/:id/items` | 仅 owner |
| `DELETE /api/playlists/:id/items/:music_id` | 仅 owner |
| `PUT /api/playlists/:id/items/reorder` | 仅 owner，集合完全一致且无重复 |

ADMIN 不绕过歌单所有权。所有变更在单连接事务内先锁 owner，再锁 items；移除后压缩为连续顺序，重排校验完整集合，失败整体回滚。

## 八、pending 分片删除消费器

### 8.1 表与时机

`pending_chunk_deletions` 字段固定为：`chunk_hash PK`、`state(PENDING|CLAIMED)`、`claim_token`、`claimed_at`、`retry_count`、`next_attempt_at`、`last_error`、`created_at`、`updated_at`。

不引入常驻后台线程：

- 启动时在 HTTP 监听前调用 `run_pending_chunk_deletions(db,fs,100)`，按批循环至本轮无到期可 claim 项。
- 每次文件删除事务提交后调用一次 `run_pending_chunk_deletions(db,fs,32)`。
- 失败项按 `min(2^retry_count,3600)` 秒退避，启动循环不会立即重复失败项。

### 8.2 接口

```cpp
struct FileDeletionPlan {
  int64_t file_id{0};
  std::size_t queued_chunk_count{0};
};

struct PendingChunkDeletion {
  std::string chunk_hash;
  std::string claim_token;
  int retry_count{0};
};

virtual MutationResult<FileDeletionPlan> delete_file_owned(
  int64_t file_id, int64_t actor_id, bool can_delete_any) = 0;
virtual MutationResult<std::vector<PendingChunkDeletion>> claim_pending_chunk_deletions(
  std::size_t limit, std::chrono::system_clock::time_point stale_before) = 0;
virtual MutationResult<std::monostate> complete_pending_chunk_deletion(
  const std::string& chunk_hash, const std::string& claim_token) = 0;
virtual MutationResult<std::monostate> release_pending_chunk_deletion(
  const std::string& chunk_hash, const std::string& claim_token,
  const std::string& last_error) = 0;
MutationResult<std::size_t> run_pending_chunk_deletions(
  IDatabasePool&, FileSystem&, std::size_t limit);
```

claim 使用 `SELECT ... FOR UPDATE SKIP LOCKED`，只抢占 `next_attempt_at<=UTC now` 的有限批次，并写随机 claim token。同一 hash 不会被并发消费者重复处理。物理文件不存在视为幂等成功并 complete；删除成功 complete；异常 release，恢复 PENDING、重试数加一并保存截断后的最后错误。超过 10 分钟的 CLAIMED 项在下一次启动回收。

## 九、认证、HTTP 客户端与会话

认证头文件为 `net/http/include/auth_service.h`。`validate_token` 改为返回 EffectiveIdentity；验签和 exp 检查后只提取 uid 回查 users，忽略 Token 中旧 role。

```ts
export type UserRole = 'GUEST' | 'NORMAL' | 'VIP' | 'ADMIN';
export type VipStatus = 'NONE' | 'ACTIVE' | 'EXPIRED';
export type Capability =
  | 'USE_AUTHENTICATED_FEATURES'
  | 'USE_VIP_BENEFITS'
  | 'MANAGE_USERS'
  | 'DELETE_ANY_FILE';

export interface AuthUser {
  user_id: number;
  username: string;
  email: string;
  role: UserRole;
  vip_status: VipStatus;
  vip_expires_at: string | null;
  capabilities: Capability[];
  created_at: string;
}

export interface AuthResponse { token: string; user: AuthUser; }
export interface AdminUserSummary extends Omit<AuthUser, 'capabilities'> {}
export interface VipPlan { duration_days: 30 | 90 | 365; label: string; }
export interface VipMembership {
  role: 'NORMAL' | 'VIP';
  vip_status: VipStatus;
  vip_expires_at: string | null;
  server_now: string;
  remaining_seconds: number;
}
```

`getVipPlans(): Promise<VipPlan[]>` 必须调用 `request<{plans:VipPlan[]}>` 后返回 `response.plans`，并有 API 单测。

`frontend/src/api/client.ts` 对 204 返回 `undefined as T`，不得调用 json。它暴露 `injectUnauthorizedHandler`，不直接 import Store。`frontend/src/main.tsx` 注入 `clearUserSession`，避免 `auth store -> auth api -> client -> stores` 循环依赖。全局 401 用重入锁只清一次再跳转；登录 401 不触发全局清理。主动登出复用同一清理函数，保留主题。

Profile 的读写必须复用 `frontend/src/api/users.ts`。

## 十、播放队列

```ts
export type QueueSource =
  | { kind: 'SINGLE'; id: null }
  | { kind: 'LIBRARY'; id: null }
  | { kind: 'PLAYLIST'; id: number };

export interface QueueEntry {
  track: MusicMeta;
  source: QueueSource;
}

play: (track: MusicMeta, queue: QueueEntry[]) => void;
detachSource: (source: QueueSource) => void;
removePendingTrack: (musicId: number, source: QueueSource) => void;
reset: () => void;
```

- 播放时复制当时列表为逐项 QueueEntry。
- 删除歌单后，该 PLAYLIST 来源 entry 全部改为 SINGLE，当前播放和顺序不变。
- 移除曲目时，当前索引之后的同来源同曲目 entry 删除。
- 当前 entry 匹配时改为 SINGLE 且不中断；已播放 entry 不回写。
- `/player/:id` 深链无 Store 状态时建立单条 SINGLE entry。

## 十一、前后端数据流与 UI

1. 认证中间件验签、提取 uid、回查 users、计算有效角色和 capabilities。
2. 登录/注册返回嵌套 AuthResponse；me 和 Profile 返回 AuthUser；created_at 均必填。
3. 管理员搜索使用 300ms 防抖；query/page 变化 abort 前请求，AbortError 静默，递增 request id 防旧响应。
4. 翻页请求复用当时 debounced query，不能退回空查询或读取之后变化的值。
5. 管理员 VIP mutation 用完整 AdminUserSummary 替换行；自助 mutation 用完整 VipMembership 更新会员状态并刷新 AuthUser。
6. 文件搜索或类型变化回第一页；请求序号阻止旧响应覆盖。
7. 主动登出或全局 401 清 auth、music、player、toast 和 token，保留主题。

### 11.1 页面与入口

- `/profile`：本人资料、有效角色、会员状态、UTC 到期、邮箱和密码更新。
- `/vip`：仅 NORMAL/VIP；显示固定计划、服务端 UTC 到期和按 server_now 校准的倒计时，提供激活或手动续期。ADMIN 无入口且 API 403。
- `/admin/users`：仅 MANAGE_USERS；搜索、分页、状态、30/90/365、授予/续期/撤销。
- `/users`：ADMIN 重定向 `/admin/users`，其他用户重定向 `/files`。
- `/files`：名称搜索、类型过滤、分页和 can_delete。
- `/my/music`：歌单重命名、删除、移除和队列联动。

Sidebar 只向 ADMIN 显示管理入口，只向 NORMAL/VIP 显示会员中心；所有登录用户显示资料。Profile 的会员区域只向 NORMAL/VIP 链接 `/vip`。无权深链在会话恢复后跳回 `/files`，不得短暂显示受限内容。

### 11.2 响应式与无障碍

- 管理员桌面表格显示用户名、邮箱、角色、会员状态、到期和操作；小于 768px 使用无嵌套逐用户条目。
- 搜索有可见 label，清除使用图标按钮与 tooltip，分页保留 aria-current，忙碌区使用 aria-busy。
- 时长使用单选或分段控件；撤销使用危险按钮和确认对话框，关闭后焦点返回触发器。
- 状态同时显示有效、过期或未开通文字，不能只靠颜色。
- 时间使用 `<time dateTime>` 并标明 UTC 依据。
- 所有输入有 label，错误 role=alert，触控区至少 44px，支持键盘、focus-visible 和 reduced-motion。
- 六视口沿用 375×812、390×844、768×1024、1024×768、1280×800、1440×900，不得溢出或遮挡。

## 十二、错误状态码

| HTTP | code | 场景 |
|---:|---|---|
| 400 | `INVALID_REQUEST` | JSON、分页、邮箱或字段错误 |
| 400 | `REGISTRATION_FIELD_FORBIDDEN` | 注册携带角色或会员字段 |
| 400 | `INVALID_VIP_DURATION` | 时长不是 30/90/365 |
| 401 | `AUTH_REQUIRED` | Token 缺失、伪造、过期或用户不存在 |
| 403 | `ADMIN_REQUIRED` | 非 ADMIN 访问管理 API |
| 403 | `VIP_SELF_SERVICE_UNAVAILABLE` | ADMIN 访问自助会员 API |
| 403 | `FILE_DELETE_FORBIDDEN` | 非上传者且非 ADMIN 删除 |
| 403 | `PLAYLIST_OWNER_REQUIRED` | 非歌单 owner 操作 |
| 404 | `USER_NOT_FOUND` | 用户不存在 |
| 404 | `FILE_NOT_FOUND` | 文件不存在 |
| 404 | `PLAYLIST_NOT_FOUND` | 歌单不存在 |
| 409 | `ADMIN_MEMBERSHIP_FORBIDDEN` | 对 ADMIN 操作 VIP |
| 409 | `USERNAME_CONFLICT` / `EMAIL_CONFLICT` | 唯一键冲突 |
| 409 | `PLAYLIST_ORDER_CONFLICT` | 重排集合不一致 |
| 422 | `VIP_STATE_INVALID` | 会员数据违反不变量 |
| 500 | `PERSISTENCE_ERROR` | 数据库或事务失败 |

## 十三、创建与修改文件清单

### 13.1 新增

| 文件 | 职责 |
|---|---|
| `core/include/authorization.h`、`core/src/authorization.cpp` | 能力和有效身份 |
| `core/include/admin_bootstrap.h`、`core/src/admin_bootstrap.cpp` | 唯一管理员引导 |
| `core/include/pending_chunk_deletions.h`、`core/src/pending_chunk_deletions.cpp` | 有限批分片消费器 |
| `core/include/schema_migrations.h`、`core/src/schema_migrations.cpp` | 启动迁移 |
| `core/include/auth_routes.h`、`core/src/auth_routes.cpp`、`core/include/file_routes.h`、`core/src/file_routes.cpp`、`core/include/playlist_routes.h`、`core/src/playlist_routes.cpp`、`core/include/vip_admin_routes.h`、`core/src/vip_admin_routes.cpp`、`core/include/upload_setup.h`、`core/src/upload_setup.cpp` | 拆分认证、文件、歌单、VIP/管理员路由和上传装配 |
| `core/include/email_validation.h`、`core/include/password_validation.h`、`core/include/strict_json.h` | 路由共享输入校验 |
| `db/include/chunk_lifecycle_coordinator.h`、`db/include/playlist_validation.h` | 分片删除协调和歌单排序校验 |
| `tests/test_authorization.cpp` | 能力、到期和上传限制 |
| `tests/test_admin_bootstrap.cpp` | ADMIN 环境与唯一性 |
| `tests/test_schema_migrations.cpp` | DDL、历史回填和 marker |
| `tests/test_pending_chunk_deletions.cpp` | claim、重试、并发和幂等 |
| `tests/test_docker_admin_env.sh` | Docker 管理员 env、参数拒绝和 base-url 映射 |
| `tests/test_test_script.sh` | frontend/e2e 转发、退出码和 trap |
| `frontend/src/api/admin.ts` | 管理员用户 API |
| `frontend/src/api/vip.ts` | 自助 VIP API |
| `frontend/src/session/clearUserSession.ts`、`frontend/src/session/createUnauthorizedHandler.ts` | 集中会话清理和注入式 401 处理 |
| `frontend/src/components/RoleUsersRedirect.tsx` | `/users` 的显式能力重定向 |
| `frontend/src/pages/ProfilePage.tsx` | 资料页 |
| `frontend/src/pages/VipCenterPage.tsx` | 会员中心 |
| `frontend/src/pages/AdminUsersPage.tsx` | 管理员用户页 |
| `frontend/tests/api/admin.test.ts`、`frontend/tests/api/vip.test.ts` | 管理员和会员 API 合同 |
| `frontend/tests/components/RoleUsersRedirect.test.tsx`、`frontend/tests/components/Sidebar.test.tsx` | 显式能力入口与导航 |
| `frontend/tests/pages/ProfilePage.test.tsx` | 资料页 |
| `frontend/tests/pages/VipCenterPage.test.tsx` | 会员中心 |
| `frontend/tests/pages/AdminUsersPage.test.tsx` | 搜索竞态与治理 |
| `frontend/tests/session/clearUserSession.test.ts` | 401 和登出清理 |
| `frontend/tests/e2e/user-governance.spec.ts` | 隔离真实流程 |

### 13.2 修改

`db/schema.sql`、`db/include/models.h`、`db/include/iconnection.h`、`db/include/idatabase_pool.h`、`db/include/database_pool.h`、`db/src/database_pool.cpp`、`db/src/boost_mysql_connection.cpp`；`net/http/include/auth_service.h`；`core/include/main_functions.h`、`core/src/auth_service.cpp`、`core/src/upload_policy.cpp`、`core/src/main.cpp`；根目录 `xmake.lua` 和 `net/http/xmake.lua`；`tests/test_auth_service.cpp`、`tests/test_database_pool.cpp`、`tests/test_http_request.cpp`、`tests/test_http_server.cpp`、`tests/test_step16_api.cpp`、`tests/test_upload_policy.cpp`；`docker-compose.yml`、`.env.example`、`scripts/docker.sh`、`scripts/test.sh`；`frontend/src/api/client.ts`、`frontend/src/api/auth.ts`、`frontend/src/api/files.ts`、`frontend/src/api/music.ts`、`frontend/src/types/api.ts`、`frontend/src/types/models.ts`、`frontend/src/stores/auth.ts`、`frontend/src/stores/music.ts`、`frontend/src/stores/player.ts`、`frontend/src/stores/toast.ts`、`frontend/src/main.tsx`、`frontend/src/components/ProtectedRoute.tsx`、`frontend/src/components/Sidebar.tsx`、`frontend/src/pages/FileDetailPage.tsx`、`frontend/src/pages/FileListPage.tsx`、`frontend/src/pages/MusicLibraryPage.tsx`、`frontend/src/pages/PlayerPage.tsx`、`frontend/src/pages/UserPlaylistPage.tsx`、`frontend/src/router.tsx`、`frontend/playwright.config.ts` 和 `frontend/tests/e2e/user-governance.spec.ts`。

## 十四、测试矩阵

### 14.1 Google Test

| 编号 | 场景 | 预期 |
|---|---|---|
| GT-01 | 四角色逐 capability | ADMIN 无 VIP，VIP 无管理 |
| GT-02 | VIP 到期前、等于、之后 | 仅严格大于 now 有效 |
| GT-03 | 四角色上传上限 | ADMIN 为 normal 且不为 0 |
| GT-04 | Token role 与 DB 不同 | 使用 DB 身份 |
| GT-05 | 撤销或过期后复用旧 Token | 即时 NORMAL |
| GT-06 | ADMIN 环境全无、部分、完整 | 不创建、失败、创建 |
| GT-07 | 同名普通、邮箱冲突 | 失败且不改账号 |
| GT-08 | 已有 ADMIN 后改用户名、并发创建 | 唯一约束拒绝 |
| GT-09 | 30/90/365 激活续期 | max 公式准确 |
| GT-10 | 对 ADMIN 管理 VIP | 409 且无写入 |
| GT-11 | 文件 owner、他人、ADMIN | 允许、403、允许 |
| GT-12 | 歌单跨用户 | 全部拒绝 |
| GT-13 | 重排事务失败 | 回滚且连续排序 |
| GT-14 | 历史 VIP 迁移两次 | 只延期一次 |
| GT-15 | DDL 中断重试 | 幂等补齐 |
| GT-16 | MySQL UTC 与转换 | 建连重连和往返一致 |
| GT-17 | MutationStatus | 稳定映射 403/404/409/422/500 |
| GT-18 | 文件事务和共享 chunk | 回滚且不误删 |
| GT-19 | pending 首轮失败、下一轮成功 | retry 后 complete 删除记录 |
| GT-20 | 两消费者并发 claim | 同一 hash 不重复 |
| GT-21 | 物理文件不存在 | complete 成功 |

### 14.2 Vitest

覆盖嵌套 AuthResponse、必填 created_at、有效角色、能力守卫、`getVipPlans` 解包、会员倒计时、Profile 复用 users API、管理员完整行替换、文件过滤、204、QueueEntry、深链、401 重入和集中清理。

管理员搜索使用 fake timer 和受控 Promise，覆盖 299ms 不请求、300ms 请求、防旧响应覆盖、AbortError 静默、翻页沿用同一 debounced query。

### 14.3 Playwright

| 编号 | 场景 | 预期 |
|---|---|---|
| E2E-01 | NORMAL/VIP 访问管理页 | 无内容并重定向 |
| E2E-02 | ADMIN 搜索分页 | 结果和 total 稳定 |
| E2E-03 | ADMIN 授予、续期、撤销 | 完整行即时更新 |
| E2E-04 | 撤销后候选用户复用旧 Token | 下一请求即时降权 |
| E2E-05 | 文件删除所有权 | owner/ADMIN 成功，他人 403 |
| E2E-06 | 歌单治理和跨用户访问 | owner 成功，跨用户拒绝 |
| E2E-07 | 删除当前来源歌单 | 不断播，来源转 SINGLE |
| E2E-08 | 移除待播曲目 | 下一曲跳过 |
| E2E-09 | Profile、Player 深链刷新 | 正确恢复 |
| E2E-10 | 登出后后退 | 无旧用户域状态 |
| E2E-11 | 六视口 | 无溢出、键盘可用 |
| E2E-12 | 自助会员激活续期 | UTC 和倒计时更新，无支付 |
| E2E-13 | ADMIN 访问会员中心 | 无入口、页面拒绝、API 403 |

真实到期边界由 GT-02/04/05 覆盖。E2E 不等待真实到期、不修改浏览器或后端时钟、不增加测试时钟环境变量。

## 十五、隔离 E2E 夹具

`scripts/test.sh e2e` 为每次运行生成唯一 `run_id=<YYYYMMDD_HHMMSS>_<pid>_<8hex>`，派生唯一 Docker project、管理员用户名/邮箱和普通用户名。强密码只写入 0600 临时 env，通过子进程环境传给 Playwright，命令和日志不得输出凭据。

夹具顺序固定：

1. 以隔离 project/env 调用扩展后的 `scripts/docker.sh deploy --project-name ... --env-file ...`。
2. 健康后登录 bootstrap ADMIN。
3. 通过公开 API 注册两个 NORMAL 候选。
4. 用 ADMIN API 将一个候选授予 VIP。
5. 分别上传小型合法音频、创建各自歌单并准备跨账号 ID。
6. 禁止直连 MySQL、写容器数据库或增加测试专用端点。
7. EXIT/INT/TERM trap 在成功、失败、信号退出时停止同一 project，删除隔离具名卷和临时凭据。
8. 脚本回归验证 e2e 不传给 xmake、参数转发、失败码和失败时 cleanup trap。

Playwright 只通过：

```bash
bash scripts/test.sh e2e tests/e2e/user-governance.spec.ts
```

不得直接调用 npm Playwright，也不得临时拼装等价 Docker 部署命令。

## 十六、按 TDD 严格串行实施 Task

### Task 1：角色、授权、UTC 与迁移

**Files:** Create authorization、schema migration 生产文件和对应测试；Modify schema、models、Boost connection、main、xmake。

**Interfaces:** Produces `is_effective_vip`、`effective_role`、`has_capability`、MutationResult、UTC 转换和 `run_schema_migrations`。

- [x] **RED：** 写 GT-01/02/03/14/15/16，运行 `bash scripts/test.sh test_authorization`、`bash scripts/test.sh test_schema_migrations`、`bash scripts/test.sh test_boost_mysql_connection`，确认因合同缺失失败。
- [x] **GREEN：** 实现授权、UTC 会话、幂等 DDL、事务回填和 main 启动接线；补齐 ADMIN 上传、严格行合同、事务回滚与重连测试后，定向测试通过。
- [x] **验证命令：** `test_authorization`、`test_schema_migrations`、`test_boost_mysql_connection`、`test_database_pool`、`test_upload_policy` 均通过；未提前运行正式门禁。

### Task 2：实时 Token、AuthResponse 与 client 合同

**Files:** Modify auth_service 头/实现、main、认证和 API 测试、前端 auth/types/client 测试。

**Interfaces:** Consumes Task 1；Produces EffectiveIdentity Token 验证、嵌套 AuthResponse、204 和 unauthorized injection。

- [x] **RED：** 覆盖旧 Token 降权、结构化存储错误、过期 role、嵌套响应、created_at、204、登录 401、同 token 新会话和回调重入，确认旧实现失败。
- [x] **GREEN：** 验签后只用 uid 结构化回查，统一序列化并实现 revision 感知的 client 合同；认证与实际路由测试通过。
- [x] **验证命令：** `test_database_pool`、`test_auth_service`、`test_step16_api`、`test_http_server`、`test_http_request`、`test_upload_policy` 定向测试及前端 `121` 项通过。

### Task 3：唯一 ADMIN 与 Docker 配置

**Files:** Create admin bootstrap 生产文件、Google Test、Docker 脚本测试；Modify main、xmake、docker script、compose、env example。

**Interfaces:** Consumes Task 2；Produces唯一 ADMIN bootstrap 和安全开发 env。

- [x] **RED：** 覆盖全无、部分、完整、重复、同名普通、邮箱冲突、改用户名、并发第二 ADMIN、日志脱敏与 Docker env 安全创建，确认合同缺失失败。
- [x] **GREEN：** 在迁移后和监听前执行 bootstrap，以 ADMIN slot、用户名及非空邮箱唯一键最终裁决，并完成安全 Docker 配置。
- [x] **验证命令：** `test_schema_migrations`、`test_admin_bootstrap`、`test_database_pool`、`test_step16_api`、`test_config` 和 Docker env 脚本回归通过。

### Task 4：自助与管理员 VIP API

**Files:** Create frontend admin API；Modify DB interfaces/pool、main、Step16 API 测试、mock 和 TS types。

**Interfaces:** Consumes Task 3；Produces三个自助端点、三个管理端点、VipMembership 和完整 AdminUserSummary。

- [x] **RED：** 中断恢复时生产与测试已存在，首次恢复 `test_step16_api` 通过、`test_database_pool` 因 GUEST 错误映射失败；后续新增 LIKE 字面搜索和时钟溢出测试并确认旧实现失败。
- [x] **GREEN：** 实现单连接事务和完整序列化；补齐非法角色、时钟上界和 LIKE 转义后重复测试通过。
- [x] **验证命令：** `test_step16_api`、`test_database_pool`、`test_auth_service`、`test_http_server`、`test_http_request`、`test_upload_policy` 及每轮附带的前端 125 项均通过。

### Task 5：文件治理与 pending 消费器

**Files:** Create pending 消费器生产文件和测试；Modify DB interfaces/pool/schema、upload policy、main、xmake、文件 API 与测试。

**Interfaces:** Consumes Task 4；Produces `delete_file_owned`、claim/complete/release 和 `run_pending_chunk_deletions`。

- [x] **RED：** 覆盖 owner、共享 chunk、事务回滚、首轮失败次轮成功、并发 claim、陈旧 claim、文件不存在；运行 upload、Step16、pending 三个定向测试。
- [x] **GREEN：** 实现事务入队、退避、启动及运行期有限批处理，不启动后台线程；重复三个测试。
- [x] **验证命令：** `bash scripts/test.sh test_upload_policy`、`bash scripts/test.sh test_step16_api`、`bash scripts/test.sh test_pending_chunk_deletions`。

#### 最终 P0 修复报告（20260728_172052）

- **RED 证据：** coordinator 组件测试先因缺少 `CleanupGuard`、活动状态和新获取接口编译失败；生产 `register_file_routes` 交错测试随后确认旧 DELETE 会在 upload guard 释放前调用数据库，且 `FILE_MUSIC_CHANGED` 未映射为 `409`。
- **并发修复：** `ChunkLifecycleCoordinator` 改为 `mutex + condition_variable + active_uploads + waiting_cleanup + cleanup_active` 的明确写者优先 RAII 协调器。cleanup waiter 在互斥区登记后，后续 upload 必须等待；两个 guard 均禁止复制、支持 move，并在析构时释放。
- **线性化修复：** DELETE route 在 `delete_file_owned` 前获取 `CleanupGuard`，持有到 pending 消费结束；`run_pending_chunk_deletions_guarded` 通过参数类型要求调用方已有 guard，route 不嵌套获取，启动 wrapper 继续自行获取。
- **测试覆盖：** 使用 atomic、condition variable 和 waiter 观测完成双向交错，不使用 sleep；覆盖写者优先、guard move/析构、cleanup 阻塞 upload 写入、真实文件 DELETE route、pending 消费顺序及 `FILE_MUSIC_CHANGED -> 409`。
- **最终验证：** 限定格式化相关 C++ 文件；`test_pending_chunk_deletions`、`test_database_pool`、`test_step16_api`、`test_http_server`、`test_upload_policy`、`test_file_system` 六个目标全部通过。每轮附带脚本回归通过，前端 Vitest 为 24 个文件、126 项通过。
- **未执行项：** 按本轮明确约束未运行 lint、build，也未提交 Git commit。

#### Task 5 终审修复报告（20260728_180702）

- **严格 TDD 证据：** 第一轮先增加 owner identity、同线程重复 cleanup、canonical 重绑、错误 permit 事务前拒绝和 guarded pending 拒绝测试，确认因缺少 `permit()`、bind 与新签名编译失败；第二轮先增加生产 upload setup 工厂测试，确认因 `upload_setup.h` 不存在编译失败；第三轮先增加双向生产交错与序列化状态行断言，确认旧实现输出 `HTTP/1.1 409 Internal Server Error`。
- **低层协调器与 canonical 绑定：** 将 `ChunkLifecycleCoordinator` 下沉到 `db/include/chunk_lifecycle_coordinator.h`，DB 层不依赖 core。`CleanupGuard`、`CleanupPermit` 构造保持私有并携带 owner identity；同线程重复 `acquire_cleanup_guard()` 立即抛 `std::logic_error`。`IDatabasePool`/`DatabasePool` 仅允许绑定一个 canonical coordinator，同实例重复绑定幂等，不同实例绑定失败；`main` 在构造 `FileSystem` 和执行任何文件操作前完成绑定。
- **permit 强制边界：** 删除公开的无 permit `delete_file_owned` 签名；DELETE 事务入口和 pending guarded 入口均要求 `CleanupPermit`，并在获取连接或启动事务前验证属于 DB canonical coordinator。错误 permit 返回 `INVALID_STATE/CLEANUP_PERMIT_INVALID`，无 fallback；coordinator A 的 guard 调用绑定 coordinator B 的 DB 已覆盖失败测试。
- **生产 upload setup：** 新增 `make_upload_setup(db, file_system, coordinator)`，`main` 注册和测试调用同一个返回对象。测试验证 setup 前 `active_uploads=0`、setup 后为 `1`，物理 chunk store 和真实 DB 查询 hook 执行期间仍为 `1`；签名拒绝、upload handler 异常、request reset 和 context 析构后均归零，并验证工厂拒绝非 canonical coordinator。
- **双向交错与 HTTP：** upload→DELETE 与 DELETE→upload 均通过生产 setup callback 和生产 DELETE handler，使用 atomic、condition variable、wait/notify 同步，不人工直接获取 upload guard、不使用 sleep；覆盖 DB delete/pending 消费及物理 store 的互斥边界。`409` reason phrase 修正为 `Conflict`，并断言序列化状态行以 `HTTP/1.1 409 Conflict\r\n` 开始。
- **限定格式化：** 仅对本轮涉及的 15 个 C/C++ 生产和测试文件执行 `bash scripts/format.sh <files...>`；未执行全量格式化。
- **六目标验证：** `bash scripts/test.sh test_pending_chunk_deletions`、`test_database_pool`、`test_step16_api`、`test_http_server`、`test_upload_policy`、`test_file_system` 全部通过；每轮脚本回归通过，附带前端 Vitest 均为 24 个文件、126 项通过。`git diff --check` 通过。
- **未执行项：** 按终审约束未运行 lint、build、CodeQL 或正式 pipeline，未创建 Git commit。当前环境未提供项目规则要求的 `task` 子代理工具，因此无法派发独立子代理；最终复验由主会话只读完成。

#### Task 5 最后审查修复报告（20260728_182107）

- **严格 TDD 证据：** 先增加 moved-from cleanup guard 再移动、cleanup→upload 与 upload→cleanup 同线程交叉获取、正常嵌套 upload 计数及跨线程移动析构测试；旧实现完成编译后超过 30 秒仍未结束，确认交叉获取错误进入等待。最小实现完成后 `test_pending_chunk_deletions` 通过。
- **协调器修复：** 每个 upload guard 保存获取线程，协调器按线程维护持有计数；guard 跨线程析构按原获取线程扣减。同线程双向交叉获取均在条件变量等待前立即抛出 `std::logic_error`，正常嵌套 upload 仍按 1→2→1→0 计数。
- **移动语义修复：** CleanupPermit 仅允许私有指针构造并支持 null owner，null permit 的 `belongs_to` 恒为 false；CleanupGuard 移动构造不再解引用 owner，因此再次移动 moved-from guard 安全。
- **permit 边界：** 完全移除旧无 permit 逻辑删除入口的接口、实现、mock 和直接单测，保留并通过 owner、permit 归属、事务回滚、共享 chunk、锁顺序及冲突测试；目标符号全仓搜索为 0 匹配。
- **HTTP 与格式化：** 复核生产 `409` reason 为 `Conflict`，现有测试覆盖 `HTTP/1.1 409 Conflict\r\n`；仅对本轮涉及的 7 个 C/C++ 文件执行限定格式化。
- **六目标验证：** `bash scripts/test.sh test_pending_chunk_deletions`、`test_database_pool`、`test_step16_api`、`test_http_server`、`test_upload_policy`、`test_file_system` 全部通过；每轮附带脚本回归通过，前端 Vitest 均为 24 个文件、126 项通过。
- **未执行项：** 按本轮约束未运行 lint、build、CodeQL 或正式 pipeline，未创建 Git commit。当前环境未提供 `task` 子代理工具，无法派发独立子代理。

### Task 6：歌单所有权与事务排序

**Files:** Modify DB interfaces/pool、main、Step16 测试、music API/Store 和 Vitest。

**Interfaces:** Consumes Task 5；Produces owned 查询、重命名、删除、移除和重排。

- [x] **RED：** 覆盖跨用户、重命名、删除、重复/缺失重排、中途失败和 204；运行 Step16 定向测试。
- [x] **GREEN：** 单连接锁 owner/items，连续排序并结构化返回；重复测试。
- [x] **验证命令：** `bash scripts/test.sh test_step16_api`。

### Task 7：会员、资料、管理员与文件 UI

**Files:** Create admin/vip API、集中会话、能力重定向、三个页面、API/页面测试和 `tests/test_test_script.sh`；Modify types、auth Store、守卫、Sidebar、文件页、router、test script。

**Interfaces:** Consumes Task 6；Produces `/vip`、`/profile`、`/admin/users`、frontend test 子命令。

- [x] **RED：** 写 plans 解包、倒计时、Profile users API、created_at、完整行替换，以及 fake timer+受控 Promise 的 299/300ms、AbortError、旧响应和翻页 query 测试；运行 `bash scripts/test.sh frontend tests/api/vip.test.ts tests/pages/VipCenterPage.test.tsx tests/pages/ProfilePage.test.tsx tests/pages/AdminUsersPage.test.tsx`。
- [x] **GREEN：** 实现页面和 `scripts/test.sh frontend`，注册脚本回归；重复定向命令。
- [x] **验证命令：** 仅运行上述 frontend 定向命令。

### Task 8：QueueEntry 与集中会话清理

**Files:** Create clearUserSession 和测试；Modify main、client、四 Store、音乐库/歌单/播放器页及测试。

**Interfaces:** Consumes Task 7；Produces QueueEntry、三类来源、队列联动、Store reset 和无环 401 接线。

- [x] **RED：** 覆盖来源转 SINGLE、当前不断播、待播移除、已播保留、深链、登出、401 重入和主题保留；运行 frontend 定向测试。
- [x] **GREEN：** 队列逐项存储并在 main 注入清理函数；重复测试且依赖无环。
- [x] **验证命令：** `bash scripts/test.sh frontend tests/stores/player.test.ts tests/session/clearUserSession.test.ts`。

### Task 9：隔离 E2E 与 Step 2 自动推进

**Files:** Create governance E2E；Modify Task 7 创建的脚本测试、test/docker 脚本和 Playwright config。

**Interfaces:** Consumes Task 8；Produces `bash scripts/test.sh e2e [args...]` 和确定性公开 API 夹具。

- [x] **RED：** 扩展脚本回归，覆盖转发、失败码和 cleanup trap；编写 E2E-01 至 E2E-13，E2E-04 使用管理员撤销，不使用测试时钟。
- [x] **GREEN：** 实现唯一 project/env、公开 API 夹具和全路径 trap 清理；完成 frontend 定向测试与脚本回归。
- [x] **定向验证：** `bash scripts/test.sh frontend tests/api/client.test.ts` 和两份脚本回归已覆盖参数、清理和退出码。
- [x] **合并后真实 E2E:** `bash scripts/test.sh e2e tests/e2e/user-governance.spec.ts`，13/13 通过，0 failed，隔离环境完整回收（2026-08-01）。
- [x] **自动推进:** Step 3-6 正式门禁严格依序执行完毕（2026-08-01）：Lint 通过，编译通过，CodeQL critical=0/high=0，全量测试通过。

## 十七、流水线阶段

- **Step 1：计划审批。** 已批准，已进入 Step 2。
- **Step 2：功能实施。** 已完成（2026-08-01）：Task 1-9 实现、定向 TDD、脚本回归与 Step 2 整体审查修复（含 E2E-10 flaky 修复）全部完成；隔离真实 E2E 清零。
- **Step 3：正式 Lint。** 已通过（2026-08-01）：`bash scripts/lint.sh --changed`，clang-tidy 0/0/0，cppcheck 0/0/0/0，前端 Oxlint 通过。
- **Step 4：正式编译。** 已通过（2026-08-01）：`bash scripts/compile.sh build`，后端与前端 0 error/warning。
- **Step 5：正式 CodeQL。** 已通过（2026-08-01）：任务 `d13ae231-d80b-42ac-b180-71df6271de41`，`critical=0`，`high=0`，`executionSuccessful=true`。
- **Step 6：正式全量测试。** 已通过（2026-08-01）：`bash scripts/test.sh`，Google Test 46 目标全通过，Vitest 34 文件/298 用例全通过，脚本回归全通过。
- **收尾隔离 E2E：** 已通过（2026-08-01）：`bash scripts/test.sh e2e tests/e2e/user-governance.spec.ts`，13/13 通过，0 failed，隔离环境完整回收。已提交并 push 到 master。

正式四门禁不得合并进一个实施 Task，也不得在 Step 2 提前执行或声称通过。Step 3 -> Step 4 -> Step 5 -> Step 6 必须严格依序执行；任一正式门禁失败均返回 Step 3，修复后从 `bash scripts/lint.sh --changed` 重新开始。

## 十八、严格依赖与完成定义

Task 必须严格串行：`Task 1 → Task 2 → Task 3 → Task 4 → Task 5 → Task 6 → Task 7 → Task 8 → Task 9`，禁止并行实施。Task 1-6 共享 `core/src/main.cpp`、数据库接口/实现和 `tests/test_step16_api.cpp`；Task 7-9 共享 types、Store、`scripts/test.sh` 和 E2E 配置，并行会造成接口漂移、迁移顺序错误和夹具冲突。

三组审查修复合并后的 Step 2 交接真实 E2E 清零后，自动进入 Step 3。全部功能验收成立、Step 3-6 针对同一最终工作树严格依序分别通过、最终隔离 E2E 清零——以上条件均已于 2026-08-01 满足，已提交并 push 到 master。

## 十九、验收标准

1. VIP 与 ADMIN 完全分离，ADMIN 无 VIP 权益且上传上限不为 0。
2. 所有授权使用 capability 或 owner，不存在角色大小授权。
3. 数据库唯一约束保证最多一个 ADMIN；改环境用户名和并发创建均失败。
4. NORMAL/VIP 可自助查看、激活、手动续期；ADMIN 无入口且 API 403。
5. 管理员 mutation 返回完整 AdminUserSummary，自助返回完整 VipMembership。
6. 存量卷启动迁移在监听前执行，DDL 幂等，历史 VIP 回填和 marker 同事务。
7. Token 每次回查 DB；过期持久 VIP API 返回 NORMAL+EXPIRED。
8. AuthResponse 嵌套，AuthUser.created_at 必填，Profile 复用 users API。
9. client 正确处理 204 和注入式 401 清理，无循环依赖。
10. MySQL 建连/重连固定 UTC，时间转换测试通过。
11. MutationResult 稳定区分不存在、所有权、冲突、非法状态和存储失败。
12. 文件删除事务不误删共享块；pending 消费幂等、有限批、可重试、无后台线程。
13. 歌单 owner、重命名、删除和排序事务完整。
14. QueueEntry 来源可表达删除歌单和移除当前/待播曲目。
15. 管理员搜索 300ms、AbortError、防旧响应和翻页 query 均有 Vitest。
16. E2E 使用隔离 project、0600 凭据、公开 API 夹具和失败清理，不直连 SQL。
17. Task 1-9 严格串行；三组审查修复合并后统一复跑 Step 2 交接真实 E2E，清零即自动进入 Step 3。Step 3-6 严格依序执行，任一正式门禁失败返回 Step 3；全量测试后最终隔离 E2E 清零，才可提交并 push。

## 二十、规格追踪

| 规格 | 覆盖 Task |
|---|---|
| 角色、能力、UTC、迁移 | 1 |
| Token、AuthResponse、401/204 | 2、8 |
| 唯一 ADMIN、Docker 安全值 | 3、9 |
| 自助与管理员 VIP | 4、7、9 |
| 文件事务、pending 消费 | 5 |
| 歌单所有权和排序 | 6 |
| 资料、会员、管理、搜索竞态 | 7 |
| QueueEntry、深链、会话清理 | 8 |
| 隔离公开 API E2E、自动推进 | 9 |

## 二十一、完成状态 (2026-08-01)

本节是 Step 19 最终验收记录。

- **Step 2 整体审查**：全部通过——pending 异常/连接回收、FileDetail/UserPlaylist 竞态、VIP 时间/状态安全、歌单读取去重排序、E2E-03/07/10/11 断言、visual fixture 均经独立只读审查确认已正确实现；E2E-10 `page.goBack()` 后新增 `waitForURL` 稳定等待。
- **Step 3 Lint**：`bash scripts/lint.sh --changed` 通过，clang-tidy 0/0/0，cppcheck 0/0/0/0，前端 Oxlint 通过。
- **Step 4 编译**：`bash scripts/compile.sh build` 通过，后端与前端 0 error/warning。
- **Step 5 CodeQL**：任务 `d13ae231-d80b-42ac-b180-71df6271de41`，`critical=0`，`high=0`，`executionSuccessful=true`。
- **Step 6 全量测试**：`bash scripts/test.sh` 通过，Google Test 46 目标全通过，Vitest 34 文件/298 用例全通过，脚本回归全通过。
- **最终隔离 E2E**：`bash scripts/test.sh e2e tests/e2e/user-governance.spec.ts`，13/13 通过，0 failed，隔离环境完整回收。
- **提交状态**：已 push 到 master。
