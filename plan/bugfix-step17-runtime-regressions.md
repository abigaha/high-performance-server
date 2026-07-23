# Step 17 运行时回归 Bug 修复计划

> 状态：A-F 已实现并完成验收；格式化、全量 Lint、构建、CodeQL、全量测试、Docker 和四视口 Playwright 均已通过。Step 17 正式完成。
>
> 记录日期：2026-07-23
>
> 本轮范围：已完成 A-F 的业务代码、脚本和自动化测试修复，并完成本地质量门禁、真实 Docker 四视口验收及最终 CodeQL SARIF 门禁。本计划保留实施前的诊断证据和服务暂不可用的历史记录，并补充最终通过结果。

## 一、问题概览与优先级

| 编号 | 优先级 | 问题 | 当前影响 | 当前状态 |
|---|---|---|---|---|
| A | P0 | 已认证 `GET /api/files/:id/stream` 返回 nginx `502 Bad Gateway`，后端进程重启 | 登录用户不能播放已上传文件；请求可触发后端异常退出 | 已修复；授权 `200/206/401/416`、Docker/E2E 已通过 |
| B | P0 | 无 `Content-Length` 的 chunked 上传仍累计完整请求体 | 单连接最多累计全局上限，多连接可并发消耗大量内存 | 已修复；解析器与全量测试已通过 |
| C | P1 | CodeQL 已配置地址失效后跳过 localhost | 不符合仓库规定的服务探测顺序，可能误报服务不可达 | 已修复；脚本回归及最终 CodeQL `0 critical + 0 high` 已通过 |
| D | P1 | 音频白名单只校验扩展名 | 随机数据改成音频后缀仍可进入上传链路 | 已修复；签名预检测试已通过 |
| E | P2 | 下载响应直接拼接原文件名 | 含引号等特殊字符的文件名可能生成畸形 `Content-Disposition` | 已修复；安全回退名与 RFC 5987 测试已通过 |
| F | P1 | 空闲服务器的线程池与事件循环可持续忙等待 | 默认四个空闲 worker 和零超时 epoll 循环会无效占用 CPU | 已修复；线程池与 TCP 回归测试已通过 |

这些问题中，A 会直接中断核心播放流程并使后端重启，B 具有并发内存耗尽风险，因此实施时先修复 A、B，再处理 C、D、E、F。A-F 已完成，不得用前端限制代替后端边界，也不得仅以 nginx 恢复健康作为修复完成证据；最终 CodeQL SARIF 已取得并满足严格门禁。

## 二、现场证据与范围判断

### 2.1 已认证流播故障

- E2E 上传的 WAV 使用由测试生成的最小合法 RIFF/WAVE 夹具，上传请求均返回 `201`；故障发生在随后读取已落盘文件的授权流播路径。因此 WAV 夹具不是这次 `502` 和后端重启的根因。
- nginx 日志先出现 `upstream prematurely closed connection while reading response header from upstream`，后续在后端尚未恢复时出现 `connect() failed (111: Connection refused) while connecting to upstream`。这说明第一条授权请求期间上游进程关闭连接，随后 nginx 无法连接正在重启的后端，而不是 nginx 静态资源或代理路由本身生成了业务错误。
- 容器观测值为 `RestartCount=7`、`OOMKilled=false`。重复重启与流播调用后的未定义行为一致，同时排除了本次现象由容器 OOM killer 直接终止进程的判断。
- `frontend/tests/e2e/deployment.spec.ts:151-156` 只在流程开头检查一次健康状态；`frontend/tests/e2e/deployment.spec.ts:324-331` 只验证匿名流播返回 `401`。匿名请求在进入悬空对象调用前就被鉴权短路，因此原有 4/4 结果没有执行会崩溃的已认证 `200/206` 路径。

### 2.2 已有测试为何没有发现

2026-07-22 的 Google Test、Vitest、CodeQL 和四视口 Playwright 结果仍是已执行的真实历史快照，不能删除或改写。但其覆盖边界如下：

1. 单元测试验证了流播请求的前端 Bearer Token 和 Blob URL 行为，没有让注册后的长期路由处理器在 `register_routes` 返回后真正读取分片。
2. 部署 E2E 验证了上传、匿名 `401` 和开头健康状态，却没有使用页面登录 Token 请求已上传文件，也没有在用例末尾再次检查健康状态。
3. E2E 没有记录执行前后的后端容器 `RestartCount`，因此即便 Docker 自动重启恢复了入口，也不会把进程崩溃判为失败。
4. CodeQL `0 critical + 0 high` 是已执行门禁结果，但这类闭包生命周期缺陷未在该次规则结果中被报告；静态分析通过不能替代授权运行路径验收。

## 三、Bug A：流播处理器悬空引用

### 3.1 根因

`core/src/main.cpp:307-345` 在 `register_routes` 函数栈上创建局部闭包 `handle_stream_file` 和 `handle_stream_range`。随后 `core/src/main.cpp:347-399` 将真正长期保存的路由 Handler 注册到服务器，但 Handler 通过 `&handle_stream_file`、`&handle_stream_range` 按引用捕获这两个局部对象。

`register_routes` 返回后，两个局部闭包已经销毁，路由 Handler 中保存的引用随即悬空。匿名请求会在 `core/src/main.cpp:349-351` 的权限检查处提前返回，所以仍能稳定得到 `401`；已认证请求继续执行到 `core/src/main.cpp:383` 或 `core/src/main.cpp:392` 时调用悬空对象，触发未定义行为，表现为后端关闭连接、nginx 返回 `502` 和容器重启。

### 3.2 修复设计

首选方案是把两段读取逻辑改成匿名命名空间内的普通函数，并显式接收 `FileSystem&`、分片列表、范围和输出缓冲区。路由 Handler 只长期捕获生命周期由应用装配保证的 `db`、`fs` 引用，调用普通函数，不再引用 `register_routes` 栈上的可调用对象。

可接受的备选方案是按值捕获两个局部闭包，即让长期 Handler 拥有闭包副本；闭包内部仍只持有应用生命周期内有效的 `FileSystem&`。但普通命名空间函数的所有权更清晰、更易单测，应优先采用。

同时检查区间计算的半开区间语义，保证 `Content-Length == end - start`、正文首尾字节和 `Content-Range` 一致，不在修复生命周期问题时引入 Range 偏移回归。

### 3.3 必须新增的验证

- 已认证无 Range 请求返回 `200`，`Accept-Ranges: bytes`、MIME、`Content-Length` 正确，响应正文与上传原始字节逐字节相同。
- 已认证 `Range: bytes=0-N` 请求返回 `206`，正文、`Content-Range`、`Content-Length` 和边界字节正确。
- 匿名请求继续返回 `401`，错误正文保留详细原因。
- 不可满足或非法 Range 返回 `416`，包含 `Content-Range: bytes */<总长度>`，后端保持健康。
- 同一个注册完成后的长期路由实例连续执行 `200`、`206`、`401`、`416`，用于覆盖 `register_routes` 已返回的真实生命周期。
- Playwright 使用登录会话的 Bearer Token 请求刚上传文件，断言 `200` 正文；再请求 Range 并断言 `206` 正文。
- E2E 最末尾重新请求 `/api/health`；验收前后读取后端容器 `RestartCount`，必须保持不增长。
- `bash scripts/docker.sh logs` 中不得出现本轮流播对应的 `502`、`upstream prematurely closed` 或 `connection refused`。

## 四、Bug B：chunked 上传在流式/丢弃模式下仍累计正文

### 4.1 根因与风险

上传预检从 `net/http/src/http_server.cpp:251-276` 读取 `Content-Length`；缺失时 `core/src/upload_policy.cpp:99-101` 返回拒绝。`net/http/src/http_server.cpp:467-472` 随即启用 discard streaming，使请求体应当只被消费而不保留。

Content-Length 路径正确遵守流式模式：`net/http/src/http_parser.cpp:198-225` 在 `streaming_mode_` 下使用最大 `2 MiB` 的 `chunk_buf_` 并调用 handler。可是 chunked 路径的 `net/http/src/http_parser.cpp:263-283` 完全没有判断 `streaming_mode_`，而是在 `net/http/src/http_parser.cpp:266` 对每个字节执行 `request_.body.push_back(c)`。因此，无 `Content-Length` 的 chunked 上传即使已经进入 discard streaming，仍会在内存中累计正文。

解析器在 `net/http/src/http_parser.cpp:250-252` 和 `269-271` 受 `kMaxBodySize` 约束，单连接并非无限增长，但多个慢速或并发连接可分别保留接近上限的正文，仍可能耗尽容器内存。该问题不能用 `OOMKilled=false` 否定；该观测只说明本次流播崩溃不是 OOM，不代表 chunked 路径安全。

### 4.2 修复选型

首选方案是统一 identity 和 chunked 的正文写入逻辑：

1. 非流式模式继续写入 `request_.body`，保持普通 API 兼容。
2. 流式模式只写入有界 `chunk_buf_`，达到 `kStreamChunkSize` 时调用 `flush_chunk_buf()`。
3. 收到终止块后，在进入 trailer/complete 前刷新剩余缓冲；网络 chunk 边界不得改变应用层 `2 MiB` 分片策略。
4. discard handler 只消费字节，`request_.body` 始终为空，缓冲峰值不超过内部流式分片大小；被拒绝请求的最终响应只追加/回调一次。
5. handler 失败、总大小超过上限、格式错误时清理临时缓冲并保持现有解析错误语义；网络 chunk 边界不应导致重复回调。

备选方案是在 `feed_chunk_data` 内复制 `feed_body_identity` 的流式分支。该改动较小，但会重复正文分发逻辑，后续更容易再次发生两条路径行为不一致，只有在统一 helper 会明显扩大风险时才采用。

当前上传契约仍要求有效 `Content-Length`，因此 chunked 上传最终继续返回结构化 `400 INVALID_CONTENT_LENGTH`；本次修复目标是让被拒绝请求以常量级内存安全排空，而不是放开 chunked 上传。

### 4.3 必须新增的验证

- 解析器在 chunked 非流式模式下仍能拼出完整 `request.body`。
- 解析器在 chunked 流式模式下按顺序回调全部原始字节，`request.body` 为空，跨网络 feed 和跨 HTTP chunk 的正文不丢失、不重复；每个应用层缓冲片段只回调一次，尾部不足一个分片的数据在终止块前恰好回调一次。
- 小于、等于、大于 `kStreamChunkSize` 的正文均正确刷新；终止块前的尾部缓冲得到刷新。
- discard handler 面对接近全局上限的 chunked 请求时不累计 `request.body`；超过上限仍返回 `PAYLOAD_TOO_LARGE`。
- 多连接并发发送被拒绝的 chunked 上传，容器 RSS 不随“连接数 × 请求体大小”线性累计，所有连接得到确定错误且服务保持健康。

## 五、Bug C：CodeQL 服务探测顺序错误

### 5.1 根因

仓库规定的顺序是：已设置的 `CODEQL_SERVER_URL` -> `http://localhost:8080` -> 交互询问 IP。`scripts/codeql.sh:52-61` 会探测已配置地址，但 localhost 探测被 `scripts/codeql.sh:63-70` 的 `[ -z "$configured_url" ]` 包围。只要变量曾设置，即使该地址不可达，也会跳过 localhost，并在非交互环境由 `scripts/codeql.sh:72-78` 直接失败。

### 5.2 修复与测试计划

- 已配置地址成功时立即使用，不重复探测 localhost。
- 已配置地址失败时始终继续探测 localhost；localhost 成功时更新并导出 `CODEQL_SERVER_URL=http://localhost:8080`。
- 两者都失败时，交互终端询问 IP 并允许重试或取消；非交互环境给出包含两个已尝试地址的错误。
- 将探测函数的 curl 调用做成可替换测试边界，覆盖“配置成功”“配置失败/localhost 成功”“两者失败/交互重试”“两者失败/非交互退出”四条路径，并断言探测顺序；shell 回归测试在临时 `PATH` 中注入可编排返回值的假 `curl`，完全不访问真实网络。
- README 的 CodeQL 说明随脚本修复同步，不能继续描述当前错误行为。

## 六、Bug D：音频类型只检查扩展名

### 6.1 根因与安全边界

`core/src/upload_policy.cpp:19-29` 定义扩展名与 MIME 映射，`core/src/upload_policy.cpp:74-82` 仅根据最后一个扩展名选择类型，`core/src/upload_policy.cpp:84-123` 的上传预检没有读取文件内容。因此把随机数据重命名为 `.mp3`、`.wav` 等白名单后缀即可通过类型检查并落盘。

前端扩展名/MIME 检查只能改善用户体验，浏览器 MIME 也不可信。服务端必须在首个分片落盘前检查内容签名；但此检查只是格式预检，不是完整音频解码、恶意媒体检测、内容合规扫描或配额/资源耗尽防护，文档和错误信息不得把它描述为“确认文件一定可播放/安全”。

### 6.2 修复设计

1. 以九种扩展名为键建立表驱动、可审计的最小签名规则，例如 MP3 的 ID3/MPEG 帧头、Ogg 的 `OggS`、WAV 的 `RIFF....WAVE`、FLAC 的 `fLaC`、AAC 的 ADTS、M4A 的 ISO BMFF `ftyp`、WMA 的 ASF GUID、APE 的 `MAC `、OPUS 的 Ogg/`OpusHead` 组合。
2. 上传上下文先在内存中保留一个有严格上限的探测前缀；获得足够字节后同时校验扩展名期望与容器/魔数签名。
3. 校验通过后，把探测前缀原样送回哈希和分片流水线，确保正文不丢失、不重复；校验失败返回结构化 `415`，在计算最终哈希、写分片和写数据库之前结束，分片与数据库新增记录均为零。
4. 对允许前置元数据或签名位置不固定的格式设置有限探测窗口，拒绝超出窗口仍无法确认的内容，避免为了“识别”而无限缓存。
5. 随机数据、扩展名与真实容器不匹配、截断头和空数据均必须落盘前拒绝；合法最小夹具和现实样本要避免误杀。

### 6.3 测试计划

- 每种白名单格式至少一个合法签名夹具和一个截断/错误签名夹具。
- `.wav` 包装随机文本、`.mp3` 包装 PNG/随机数据、扩展名与另一音频容器不匹配均返回 `415`，分片目录与数据库无新增记录。
- 签名跨 TCP feed 或内部分片边界时仍能正确识别。
- 合法文件的最终哈希、大小和下载正文与原始输入完全一致。
- 大量无效内容只占用固定探测窗口，不产生按文件大小增长的预检缓存。

## 七、Bug E：下载文件名响应头生成不安全

### 7.1 根因

上传侧能够解析 RFC 5987 `filename*`，合法双引号、反斜杠等特殊字符可保存在文件名中；下载侧却在 `core/src/main.cpp:299-304` 直接执行 `"attachment; filename=\"" + record->file_name + "\""`。原名中的双引号会提前结束 quoted-string，反斜杠也会改变转义语义，生成畸形参数。上传解析当前已拒绝 CR/LF 与其他控制字符，因此控制字符不是本次已确认存量问题的触发条件；但响应头构造器仍必须独立防御这些字符，不能依赖数据库内容永远来自当前上传入口。

### 7.2 修复与测试计划

- 新增统一的 `Content-Disposition` 生成 helper，下载等所有响应都复用同一实现。
- `filename=` 使用经过控制字符、引号、反斜杠和路径分隔符处理的 ASCII 回退名；不得包含 CR/LF。
- `filename*=` 使用 UTF-8 与 RFC 5987 百分号编码保留真实文件名，参数顺序和字符集声明固定，避免各路由自行拼接。
- 对中文、空格、百分号、分号、双引号、反斜杠、CR/LF、纯非 ASCII 和超长名称做单元测试；序列化响应必须保持单一合法头部。
- 浏览器下载验收确认保存名合理；无法原样放入 ASCII 回退时，以 `filename*` 为权威，不因回退名简化而丢失 UTF-8 名称。

## 八、Bug F：空闲 CPU 忙等待

### 8.1 根因与风险

`net/tcp/tcp_server/include/tcp_server.h` 的 `TcpServer::Config::thread_count` 默认值为 `4`，`net/tcp/tcp_server/src/tcp_server.cpp` 在初始化时按该值创建 `LockFreeThreadPool`。`net/thread-pool/src/thread_pool.cpp` 的每个 worker 都调用 `tasks_.pop`；`net/thread-pool/include/lock_free_queue.hpp` 的 `pop` 在队列为空时以 `std::this_thread::yield()` 循环等待。因此无任务时默认四个 worker 都会反复让出并重新竞争 CPU；`yield` 只是调度提示，不是阻塞等待，空闲进程仍可能持续消耗一个或多个逻辑核。

事件循环还把 `config_.epoll_timeout_ms` 直接传给 `epoll_wait`。配置被设置为 `0` 时，空闲 `epoll_wait` 立即返回，`while (running_)` 随即再次调用，形成独立的热循环。两个问题叠加会在没有连接和没有任务时制造持续 CPU 使用、功耗和调度抖动，并使高负载时的请求处理与关闭响应更不稳定。

### 8.2 修复与测试计划

- 保留无锁队列的数据路径，但为 worker 增加可被 `enqueue` 和 `stop` 唤醒的阻塞等待机制；空队列不得靠无限 `yield` 轮询，停止后所有 worker 必须及时退出且不得遗漏已入队任务。
- 明确 `epoll_timeout_ms` 的配置契约：拒绝或规范化 `0` 为一个有界正超时，默认保持有限阻塞；停止路径依赖既有 `eventfd` 唤醒，不以零超时轮询换取退出速度。
- 补充线程池空闲测试：默认四 worker 在空队列期间进入等待，投递单个任务后及时执行，`stop`/析构在无任务和有待处理任务时均可确定完成。
- 补充 TCP 服务器配置与空闲循环测试：`epoll_timeout_ms=0` 得到可观察的安全行为，空闲服务器仍能接受连接、处理请求并由 wake fd 及时停止；测试不能以不稳定的机器级 CPU 百分比作为唯一断言。
- 在受控运行环境增加空闲窗口采样，验证修复后进程 CPU 时间增量有明确上界，并与有请求时的吞吐、延迟和优雅关闭结果一并记录。

## 九、实施阶段与依赖顺序

### 阶段 1：先消除进程崩溃和内存风险

- [x] 修复 Bug A 的 Handler 生命周期，补长期路由实例的 `200/206/401/416` 集成测试。
- [x] 修复 Bug B 的 chunked 流式分发与终止刷新，补解析器边界和并发内存测试。
- [x] 已执行相关 Google Test；P0 通过后完成 Docker 部署验收。

### 阶段 2：收紧上传与响应契约

- [x] 实现 Bug D 的有界流式签名预检，保证落盘前拒绝和正文完整性。
- [x] 实现 Bug E 的统一安全下载文件名生成器，并覆盖特殊字符。
- [x] 更新前后端错误展示或契约测试，后端详细错误继续保留。

### 阶段 3：修复工具探测、空闲忙等待并扩充部署验收

- [x] 修复 Bug C 的 CodeQL 探测顺序及脚本回归测试。
- [x] 通过可达服务执行最终 CodeQL 分析，取得 SARIF `0 critical + 0 high`。
- [x] 修复 Bug F 的空队列阻塞等待和 `epoll_timeout_ms=0` 热循环风险，补线程池与 TCP 空闲回归测试。
- [x] 扩充 Playwright 已认证 `200/206/401/416` 流播、下载响应头、末尾健康检查和正文断言。
- [x] 在 Docker 验收前后记录后端 `RestartCount`，并复核完整 nginx/后端日志。

### 阶段 4：同步文档并关闭计划

- [x] 更新本计划执行记录和 Step 17 当前状态说明，保留实施前诊断事实。
- [x] 已通过可达 CodeQL 服务记录 SARIF 结果；任务 `ff62db5b-e741-4d78-ac00-33100da6ce8a` 满足 `0 critical + 0 high`，Step 17 已标为正式完成。

## 十、完整测试矩阵

| 范围 | 场景 | 预期 |
|---|---|---|
| 授权流播 | 完整文件 | `200`，正文、长度、MIME 正确 |
| 授权流播 | 合法单区间 | `206`，正文和 Range 头精确 |
| 授权流播 | 非法/不可满足区间 | `416`，服务不重启 |
| 授权流播 | 匿名访问 | `401`，详细错误保留 |
| 运行健康 | 流播前后 | `/api/health` 均为 `200`，`RestartCount` 不增长 |
| chunked 解析 | 普通模式 | `request.body` 完整保留 |
| chunked 解析 | streaming/discard | handler 收到完整数据，`request.body` 为空，缓冲有界 |
| chunked 解析 | 超上限、多连接 | 确定拒绝，不出现连接数乘请求体大小的常驻累计 |
| 类型预检 | 九种合法签名 | 匹配扩展名时允许，原始正文完整 |
| 类型预检 | 随机/截断/错配数据 | `415`，不写分片、不写数据库 |
| 下载文件名 | 中文、空格、引号、控制字符 | 生成合法 `filename`/`filename*`，无头部注入或畸形参数 |
| CodeQL 探测 | 配置地址失败、localhost 成功 | 自动回退 localhost，不询问、不失败 |
| 空闲线程池 | 默认四 worker、无任务、投递与停止 | 空队列阻塞等待，投递后及时执行，停止不挂起、不丢任务 |
| 空闲事件循环 | `epoll_timeout_ms=0`、无连接、唤醒关闭 | 无零超时热循环，仍可处理连接并及时停止 |
| 空闲资源 | 无连接无任务的受控采样窗口 | CPU 时间增量有上界，不以机器级瞬时百分比作唯一判据 |
| 浏览器 | 四视口完整用户流 | 上传、授权播放、Range、末尾健康均通过 |

## 十一、正式质量门禁与部署验收

修复后的最终工作树必须按以下顺序执行，不得复用 2026-07-22 修复前的门禁快照：

```bash
bash scripts/format.sh all
bash scripts/lint.sh --all
bash scripts/compile.sh build
bash scripts/codeql.sh run
bash scripts/test.sh
bash scripts/docker.sh deploy
bash scripts/docker.sh health
cd frontend
PLAYWRIGHT_BASE_URL=http://127.0.0.1:18080 npm run test:e2e
cd ..
bash scripts/docker.sh health
bash scripts/docker.sh logs
bash scripts/docker.sh status
```

前五步等价于正式 `bash scripts/pipeline.sh all` 的固定顺序；可以使用聚合入口，但执行记录必须清楚列出每一阶段结果。门禁输出不得截断，CodeQL 必须为 `0 critical + 0 high`，Google Test 与 Vitest 必须 100% 通过，Playwright 所有配置视口必须通过。

Docker/Playwright 之外还必须在执行前后读取后端容器的完整状态，记录 `RestartCount` 和 `OOMKilled`。修复验收要求 `RestartCount` 前后相同、`OOMKilled=false`，并且 nginx 与后端日志中没有测试期间新增的崩溃、`502`、`upstream prematurely closed` 或 `connection refused`。

## 十二、完成定义与当前状态

只有同时满足以下条件，Step 17 和本修复计划才可改为完成：

1. 已认证流播的 `200`、`206`、匿名 `401`、非法 Range `416` 均有自动化正文与响应头断言。
2. Docker 中连续执行授权流播后入口保持健康，后端 `RestartCount` 不增长。
3. chunked streaming/discard 不再累计 `request.body`，并发拒绝路径内存有界。
4. 随机数据仅改音频后缀不能落盘，扩展名与签名不一致得到结构化详细错误。
5. 下载 `Content-Disposition` 对 UTF-8 和特殊字符合法、安全、可回归。
6. CodeQL 严格按“配置地址 -> localhost -> 询问 IP”的顺序探测。
7. 默认四个空闲 LockFreeThreadPool worker 不再以 `yield` 忙等待，`epoll_timeout_ms=0` 不再导致事件循环热循环；投递、关闭和连接处理回归均通过，空闲验证不以机器级瞬时 CPU 百分比作为唯一判据。
8. 格式化、全量 Lint、构建、CodeQL、全部测试、Docker、四视口 Playwright、日志与重启计数复核全部通过同一份最终代码。
9. 本计划记录真实门禁结果与未满足项，不把外部阻塞误记为通过，且不包含临时 E2E/QPS 产物。

### 12.1 2026-07-23 当前满足状态

| 完成条件 | 当前状态 |
|---|---|
| 1 | 已满足：已认证 `200/206`、匿名 `401`、不可满足 Range `416` 均有正文和响应头自动化断言。 |
| 2 | 已满足：Docker 授权流播后健康检查成功，`RestartCount=0`。 |
| 3 | 已满足：chunked streaming/discard 不累计 `request.body`，解析器与拒绝路径测试通过。 |
| 4 | 已满足：九种音频签名预检与错误签名拒绝测试通过。 |
| 5 | 已满足：下载头同时验证安全 `filename=` 与 RFC 5987 `filename*=`。 |
| 6 | 已满足：CodeQL 探测顺序已修复，并有离线脚本回归测试。 |
| 7 | 已满足：线程池空队列改为阻塞等待，零 epoll 超时已规范化，线程池与 TCP 回归测试通过。 |
| 8 | 已满足：格式化、全量 Lint、构建、CodeQL、测试、Docker、四视口 Playwright、日志与容器状态复核均通过；CodeQL 任务 `ff62db5b-e741-4d78-ac00-33100da6ce8a` 的 SARIF 为 `0 critical + 0 high`。 |
| 9 | 已满足：本计划已记录真实门禁结果，包括服务暂不可用的历史情况和最终 CodeQL 通过记录。 |

因此 Step 17 已完成实施、全部质量门禁和部署验收，正式完成。

## 十三、风险与回滚

| 风险 | 控制措施 | 回滚策略 |
|---|---|---|
| 流播重构改变 Range 边界 | 用固定字节夹具同时断言首尾、长度和响应头 | 只回滚流播 helper 改动，保留新增测试用于复现 |
| chunked 统一分发导致丢字节/重复字节 | 对跨 feed、跨 HTTP chunk、跨内部缓冲边界做哈希和正文断言 | 回滚到最小的 chunked 流式分支实现，不恢复正文累计行为 |
| 魔数规则误杀合法音频 | 使用真实样本与最小合法夹具，探测窗口有明确上限 | 按单格式回滚/放宽规则，不整体关闭签名预检；未识别格式保持拒绝 |
| 下载名兼容性差异 | 同时发送安全 ASCII `filename` 和 UTF-8 `filename*` | 保留统一 helper，调整回退策略，不恢复原始字符串直接拼接 |
| CodeQL 回退连接到非预期本机服务 | 探测必须验证服务健康响应并打印最终选择 | 回滚选择逻辑前保留探测顺序测试，允许显式变量成功时优先 |
| worker 阻塞机制丢失唤醒或停止变慢 | 覆盖空队列、并发投递、停止和析构，使用谓词/计数避免虚假唤醒 | 仅回滚等待封装，不恢复无限 `yield`；保留队列正确性测试定位丢失唤醒 |
| 拒绝或规范化零 epoll 超时改变低延迟预期 | 用连接延迟、eventfd 唤醒和关闭时延同时验收，明确最小正超时 | 仅调整最小超时阈值和配置诊断，不恢复零超时热循环 |
| Docker 自动重启掩盖崩溃 | 用例末尾健康检查，并比较执行前后 `RestartCount` | 验收失败立即停止发布，保留容器和完整日志供复盘 |

回滚不等于验收通过。若任一 P0 修复被回滚，Step 17 必须继续标记阻塞，不能仅凭旧的 4/4 报告恢复“已完成”状态。

## 十四、历史诊断结论

实施前的诊断轮次只完成了故障复现、证据核对、根因分析、修复设计和验收计划。已确认 A-F 六个问题的代码路径、运行时影响和覆盖缺口，并建立后续实现、测试、质量门禁、部署验收及回滚标准。2026-07-22 的四视口 E2E `4/4` 和分项质量门禁是历史执行事实，但不构成授权流播成功路径的通过证据；P0 修复前禁止宣告 Step 17 正式完成。

## 十五、2026-07-23 实际执行记录

本次执行已完成 A-F 的代码、脚本和测试更新。以下结果均对应当前修复工作树；历史诊断记录未被替换为通过结论。

| 阶段 | 实际命令或验收动作 | 结果 |
|---|---|---|
| 格式化 | `bash scripts/format.sh all` | 退出码 `0`。 |
| 全量 Lint | `bash scripts/lint.sh --all` | 退出码 `0`；clang-tidy、cppcheck 均为零问题，前端检查通过。 |
| 构建 | `bash scripts/compile.sh build` | 退出码 `0`。 |
| CodeQL | `CODEQL_SUBMIT_TIMEOUT=900 bash scripts/codeql.sh run` | 服务恢复后退出码 `0`；任务 `ff62db5b-e741-4d78-ac00-33100da6ce8a` 成功完成，SARIF `critical=0`、`high=0`。此前服务暂不可达的尝试保留为历史记录，不作为最终门禁结果。 |
| 全量测试 | `bash scripts/test.sh` | 通过：`42/42 + 18/71`。 |
| Docker 与四视口 E2E | Docker 部署、健康检查、日志和状态复核；`PLAYWRIGHT_BASE_URL=http://127.0.0.1:18080 npm run test:e2e` | 四视口 `4/4` 通过；`RestartCount=0`、`OOMKilled=false`，没有新增 `502`、`upstream prematurely closed` 或 `connection refused`。 |

### 最终 CodeQL 闭环

CodeQL 服务恢复后，项目脚本成功连接 `http://localhost:8080`、提交并轮询任务 `ff62db5b-e741-4d78-ac00-33100da6ce8a`。服务端源码提取成功，C/C++ 提取器仅输出 `build-mode=none` 的说明性 note；SARIF 显示 `critical=0`、`high=0`，没有需要修复的问题。至此不存在外部阻塞或未完成项，Step 17 正式完成。
