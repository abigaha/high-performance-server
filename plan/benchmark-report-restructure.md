# 基准测试报告分层与续跑实施计划

> **供代理执行：** 必须使用 `subagent-driven-development` 按任务逐项实施；所有步骤采用测试先行，任务清单使用复选框维护。

**目标：** 将性能报告重构为 `benchmark/report/` 下按实际目标与运行时间分层的可续跑结果，并对 RPS 提供隔离 Docker 测试环境的标准化生命周期。

**架构：** `scripts/benchmark.sh` 以单个模块或 RPS 矩阵单元为最小持久化单位，在每个时间戳目录写入原始输出、文本结果和 `manifest.json`。运行指纹决定已通过结果能否复用；旧报告只由一次性迁移工具复制到只读历史区域，运行与对比逻辑绝不回退读取旧格式。

**技术栈：** Bash、Python 3、Docker Compose、xmake、Google Benchmark、Google Test。

## 当前状态（2026-08-04）

- Task 1-7 已完成：报告分层、原子 manifest、目标级和 RPS 单元续跑、隔离 Docker 生命周期、历史只读迁移、文档/CodeQL 排除及静态分析长任务策略均已实现，并由脚本回归覆盖。
- Task 8 已完成：动态发现的微基准和 QPS `full` 报告已生成；RPS `full` 与 `overload` 均完成并写入新结构目录；目录、manifest、`diff` 和迁移结果已复核。
- 本轮实际性能记录：RPS run `20260804_184656` 的 `full` 为 `40/40` cells passed，`overload` 为 `25/25` cells completed，其中 `15` 个按设计记录为 overloaded、无 failed cell。
- 本轮正式质量流水线 `bash scripts/pipeline.sh all` 已通过：后端 46/46、前端 34 个文件/298 用例、脚本回归全部通过；clang-tidy、cppcheck、编译、前端构建均通过，CodeQL 为 `0 critical + 0 high`。
- `benchmark/report/` 是当前报告根目录；`benchmark/reports/` 仅作为历史只读迁移源，未被覆盖或删除。
- 历史运行中出现过的超时、失败和 CodeQL 服务超时仍保留在对应 manifest 中，用于追溯；不代表当前门禁或最终性能记录失败。

## 全局约束

- 使用 `benchmark/report/`，不得再向 `benchmark/reports/` 写入新报告。
- 模块目录使用实际目标名：`bench_*`、`qps_*`。
- 新报告目录固定为 `micro/<目标>/<时间戳>`、`qps/<目标>/<时间戳>`、`rps/<profile>/<时间戳>`；时间戳为 `YYYYMMDD_HHMMSS`，禁止 `latest`。
- QPS 正式范围为全部动态发现目标的 `full`；RPS 正式范围为 `full` 和 `overload`，不新增跨微基准、QPS、RPS 的 `all` 命令。
- 已通过项目仅在运行指纹相同时跳过；失败、超时、运行中、清单损坏或指纹变化的项目必须重跑。
- RPS 显式 `RPS_BASE_URL` 属于外部环境，脚本只做预检，不部署、不清理；未设置时创建独立 Compose 项目并在命令结束时 `down --volumes`。
- 独立环境使用工作区外的 `0700` 临时目录和 `0600` 临时 env 文件；不得输出或提交密钥、token、密码。
- 历史迁移只复制，不移动、删除、覆盖或硬链接旧原件；旧格式仅供人工只读，运行脚本和 `diff` 不适配。
- 所有构建、测试、格式化、Lint、CodeQL 均使用仓库 `scripts/` 中既有入口；提交和推送仅在用户明确要求时执行。

---

### Task 1：建立报告与续跑脚本回归测试

**文件：**
- 新建：`tests/test_benchmark_script.sh`
- 修改：`scripts/test.sh:22-28`

**接口：**
- 消费：现有 `scripts/benchmark.sh` 非交互入口。
- 产出：以临时目录、伪造 `xmake`、`timeout`、目标二进制、`curl` 和 `wrk` 运行的脚本回归测试；任何真实 Docker 服务或基准均不得被此测试访问。

测试脚本默认执行全部场景；`BENCHMARK_TEST_GROUP=micro_qps` 仅执行微基准、QPS 和相关 `diff` 场景，`BENCHMARK_TEST_GROUP=rps` 仅执行 RPS 场景。筛选不得改变默认全量覆盖或放宽任何断言。

- [x] **步骤 1：编写失败的微基准/QPS 路径测试。**

```bash
# 预置两个 bench_* 或 qps_* 伪目标，调用 benchmark.sh 后断言：
# benchmark/report/micro/bench_alpha/<run-id>/manifest.json
# benchmark/report/qps/qps_alpha/<run-id>/manifest.json
# 均存在，旧 benchmark/reports/ 下没有新文件。
```

- [x] **步骤 2：运行新测试并确认失败原因是尚未实现分层清单。**

运行：

```bash
bash tests/test_benchmark_script.sh
```

预期：失败于新路径或 `manifest.json` 不存在，而非测试夹具错误。

- [x] **步骤 3：补充失败的续跑与指纹测试。**

```bash
# 首次让 beta 失败、alpha 通过；相同 --resume ID 仅重跑 beta。
# 修改 alpha 的伪二进制或 BENCH_FLAGS/QPS_PROFILE 后续跑，alpha 必须重跑。
# 预置旧 benchmark/reports/*.json，断言 resume 与 diff 均不读取它。
```

- [x] **步骤 4：补充失败的 RPS 单元与 diff 测试。**

```bash
# 伪造 full 矩阵中一个 cell 失败；--resume ID 只执行该 cell。
# 变更 RPS_TARGET_FINGERPRINT 或矩阵参数后，所有 cell 重跑。
# diff micro bench_alpha、diff qps qps_alpha full、diff rps full
# 只选择新目录中最近两份 complete manifest。
```

- [x] **步骤 5：将回归脚本接入 `run_script_regression_tests`。**

```bash
bash "$PROJECT_ROOT/tests/test_benchmark_script.sh"
```

- [x] **步骤 6：运行并确认红灯阶段。**

运行：

```bash
bash scripts/test.sh
```

预期：新增脚本回归测试失败，其他基线测试仍保持通过。

### Task 2：实现模块级报告、清单、续跑与新结构对比

**文件：**
- 修改：`scripts/benchmark.sh:12-1288`
- 修改：`tests/test_benchmark_script.sh`（增加不改变默认全量覆盖的测试域筛选）
- 验证：`tests/test_benchmark_script.sh`

**接口：**
- 消费：任务 1 的参数与路径断言。
- 产出：`micro [--debug] [--run-id ID|--resume ID]`、`qps [smoke|full] [--debug] [--run-id ID|--resume ID]`、`diff micro <bench_target>`、`diff qps <qps_target> <profile>`、`diff rps <profile>`；测试脚本支持 `BENCHMARK_TEST_GROUP=micro_qps`，但默认仍执行所有场景。

- [x] **步骤 1：实现运行 ID、目录和原子清单辅助函数。**

```bash
# run-id 格式：^[0-9]{8}_[0-9]{6}$
# 新运行拒绝覆盖已有 <目标>/<run-id>。
# manifest 以临时文件写入后 mv 到 manifest.json；不得留下半写入 JSON。
# 清单字段：schema_version、kind、target/profile、run_id、state、attempt、
# started_at、finished_at、run_fingerprint、fingerprint_inputs、environment、
# status、exit_code、elapsed_seconds、raw_file、metrics。
```

- [x] **步骤 2：实现微基准按实际 `bench_*` 目标持久化。**

```bash
# 每个 bin/bench_* 独立写入：
# benchmark/report/micro/<bench_target>/<run-id>/raw.txt
# benchmark/report/micro/<bench_target>/<run-id>/report.txt
# benchmark/report/micro/<bench_target>/<run-id>/manifest.json
# 相同指纹且 state=passed 时在 --resume 中跳过。
```

- [x] **步骤 3：运行微基准和 QPS 子集并确认通过。**

运行：

```bash
BENCHMARK_TEST_GROUP=micro_qps bash tests/test_benchmark_script.sh
```

预期：微基准路径、QPS 路径、失败保留、相同指纹跳过、指纹变化重跑和新结构 micro/QPS `diff` 断言通过；RPS 场景尚由任务 4 实现。

- [x] **步骤 4：实现 QPS 按实际 `qps_*` 目标持久化。**

```bash
# 在通过 discover_qps_targets 与 validate_qps_artifacts 后，
# 每个 qps_* 目标写入其独立目录和 manifest；profile 是清单字段。
# --resume 只跳过同指纹 state=passed 的目标。
```

- [x] **步骤 5：替换旧 `cmd_diff` 的平铺文件扫描。**

```bash
# diff 仅搜索 benchmark/report 下 schema_version 匹配、state=passed 的 manifest。
# micro/QPS 按目标筛选；QPS 再按 profile 筛选；RPS 按 profile 筛选。
# 旧 benchmark/reports 与 _legacy_aggregate 均不参与搜索。
```

- [x] **步骤 6：运行微基准和 QPS 子集回归并确认绿灯。**

运行：

```bash
BENCHMARK_TEST_GROUP=micro_qps bash tests/test_benchmark_script.sh
```

预期：所有 micro/QPS 模块级路径、续跑、指纹失效和对应 `diff` 断言通过；默认全量测试仍因 RPS 场景未实现保持 RED。

**Task 2 审查状态：** PASS — manifest 原子性（tmp+mv、pre/post 安全验证、signal/错误路径清理）和目录链接边界（is_safe_report_component regex、report_run_dir_is_safe symlink 拒绝、is_single_link_regular_file nlink==1 硬链接防护）均无确认问题。审查已闭环，可进入 Task 3。

### Task 3：建立可复用的隔离 Docker 测试环境生命周期

**文件：**
- 新建：`scripts/lib/isolated_docker_env.sh`
- 修改：`scripts/docker.sh:14-34,524-620,666-733`
- 修改：`scripts/test.sh:44-160`
- 修改：`tests/test_test_script.sh`

**接口：**
- 消费：`scripts/docker.sh deploy|base-url|down --project-name --env-file`。
- 产出：`hps_start_isolated_environment <prefix> <run-id>`、`hps_cleanup_isolated_environment`、`hps_runtime_fingerprint`；`docker.sh runtime-fingerprint --project-name <名称> --env-file <路径>` 只输出不含密钥的运行时指纹。

- [x] **步骤 1：为 Docker 生命周期补充失败测试。**

```bash
# 断言独立 project 使用 HPS_HTTP_PORT=0、唯一项目名、0600 env、
# 一次 deploy/base-url/down --volumes；env 缺失时 down 省略 --env-file 仍可清理。
# 断言部署、URL 解析、测试进程和信号失败均清理容器/网络/卷并删除 env。
# 捕获输出，断言测试密钥不出现。
```

- [x] **步骤 2：运行脚本回归测试并确认新断言失败。**

运行：

```bash
bash tests/test_test_script.sh
```

预期：失败原因是通用隔离环境 helper 与运行时指纹接口尚不存在。

已确认：既有断言全部通过（含 `test.sh frontend/e2e 参数路由回归通过`），新增断言在预期处失败：`缺少 scripts/lib/isolated_docker_env.sh`。

- [x] **步骤 3：抽取 `scripts/test.sh` 的 E2E 生命周期为 sourceable helper。**

```bash
# helper 固定在工作区外创建 0700 目录和 0600 env。
# cleanup 始终调用 docker.sh down --project-name <唯一名> --volumes；
# env 已丢失时省略 --env-file。原始测试失败码优先于清理失败码。
```

- [x] **步骤 4：为 `docker.sh` 增加只读 `runtime-fingerprint`。**

```bash
# 从当前 project 的 high-performance-server 容器/镜像与已验证 Compose 配置
# 生成稳定的 SHA-256 输入；不得输出 env 内容、密码或 token。
```

- [x] **步骤 5：将 E2E 现有逻辑切换到 helper 并运行测试。**

运行：

```bash
bash tests/test_test_script.sh
```

预期：现有 E2E 生命周期断言与新增资源、密钥、异常路径断言均通过。

### Task 4：实现 RPS 多 profile、单元级续跑和托管环境

**文件：**
- 修改：`scripts/benchmark.sh:540-1115,1211-1288`
- 验证：`tests/test_benchmark_script.sh`
- 依赖：任务 2、任务 3。

**接口：**
- 消费：`hps_start_isolated_environment`、`hps_cleanup_isolated_environment`、`hps_runtime_fingerprint`。
- 产出：`rps [smoke|full|overload ...] [--run-id ID|--resume ID]`；多 profile 在同一命令中共享一次托管环境。保留 `load` 作为 `rps` 兼容别名。

- [x] **步骤 1：补充失败的多 profile 生命周期与 RPS 单元续跑测试。**

```bash
# bash scripts/benchmark.sh rps full overload --run-id <ID>
# 未设置 RPS_BASE_URL 时只部署一次、两个 profile 共享 URL、最后只清理一次。
# 设置 RPS_BASE_URL 时不部署、不清理；显式环境必须提供 RPS_TARGET_FINGERPRINT。
```

- [x] **步骤 2：实现 profile 参数解析和 RPS 报告根目录。**

```bash
# full -> benchmark/report/rps/full/<run-id>/
# overload -> benchmark/report/rps/overload/<run-id>/
# 保留 smoke 的兼容目录 benchmark/report/rps/smoke/<run-id>/，
# 但正式执行清单只调用 full 与 overload。
```

- [x] **步骤 3：实现单元级状态持久化与恢复。**

```bash
# 每个确定性 cell_id 在完成后立刻原子更新 RPS manifest。
# 同指纹且 passed 的单元跳过；overload 的 overloaded 视为完成；
# failed、timeout、running 重新执行。每次恢复都重新预检，凭据只驻留内存。
```

- [x] **步骤 4：实现托管环境选择和运行指纹。**

```bash
# RPS_BASE_URL 非空：验证健康，使用 RPS_TARGET_FINGERPRINT，不执行 Docker 操作。
# RPS_BASE_URL 为空：启动唯一 hps_rps_<run-id> 项目，使用动态端口和 runtime-fingerprint；
# 不把项目名、临时 env、随机用户、token 或密码写入报告清单。
```

- [x] **步骤 5：确保所有退出路径清理独立环境。**

```bash
# 成功、基准失败、部署失败、INT、TERM 均保留报告，随后 down --volumes。
# 清理失败不掩盖原始基准失败；若仅清理失败，命令返回清理失败码并报告项目名。
```

- [x] **步骤 6：运行 RPS 回归脚本并确认通过。**

运行：

```bash
BENCHMARK_TEST_GROUP=rps bash tests/test_benchmark_script.sh
```

预期：full/overload 共用托管环境、外部 URL 不被清理、单元续跑和指纹失效断言通过。

- [x] **步骤 7：运行默认全量回归测试并确认全部场景转绿。**

运行：

```bash
bash tests/test_benchmark_script.sh
```

预期：micro、QPS、RPS 和全部 `diff` 场景均通过。

### Task 5：迁移历史报告为只读资料

**文件：**
- 新建：`benchmark/tools/migrate_legacy_reports.py`
- 新建：`tests/test_migrate_legacy_reports.sh`
- 修改：`scripts/test.sh:22-28`

**接口：**
- 消费：`benchmark/reports/` 中的历史 TXT 报告。
- 产出：默认 `--dry-run` 的迁移器；显式 `--source`、`--destination`、`--apply`、`--no-clobber` 参数；不生成可供运行或 `diff` 消费的 manifest。

- [x] **步骤 1：编写失败的历史迁移夹具测试。**

```bash
# 覆盖可分段 micro、可唯一归属 QPS、歧义 QPS 和 load_*。
# 断言 dry-run 不写文件；apply 仅复制；原件 SHA-256 和大小不变；
# 不可拆分文件逐字节写入 _legacy_aggregate。
```

- [x] **步骤 2：运行迁移测试并确认失败。**

运行：

```bash
bash tests/test_migrate_legacy_reports.sh
```

预期：失败于迁移器不存在。

- [x] **步骤 3：实现严格解析、白名单与迁移清单。**

```python
# 仅当所有非头部字节可唯一映射到实际 bench_/qps_ 模块段时拆分。
# 新目标为 <类型>/<模块>/<原始时间戳>/legacy_<原始文件名>.txt。
# 无法可靠拆分者复制到 <类型>/_legacy_aggregate/<原始时间戳>/。
# 迁移清单记录原路径、SHA-256、字节范围、段标题、目标路径和迁移时间。
```

- [x] **步骤 4：运行迁移测试并确认通过。**

运行：

```bash
bash tests/test_migrate_legacy_reports.sh
```

预期：可拆分报告只提取原始字节段，遗留报告逐字节保留，原目录无改动。

- [x] **步骤 5：在真实历史目录执行显式迁移。**

运行：

```bash
python3 benchmark/tools/migrate_legacy_reports.py --source benchmark/reports --destination benchmark/report --apply --no-clobber
```

预期：生成迁移清单和新只读副本；不删除 `benchmark/reports/` 中的任何文件。

### Task 6：同步文档、CodeQL 排除和使用说明

**文件：**
- 修改：`README.md:420-451`
- 修改：`benchmark/README.md:1-194`
- 修改：`scripts/codeql.sh:187-205`
- 修改：`tests/test_codeql_discovery.sh`（增加 `package_source` 的报告排除断言）

**接口：**
- 消费：任务 2 至任务 5 的实际命令与目录。
- 产出：与实现一致的使用说明；CodeQL 同时排除旧 `benchmark/reports/` 与新 `benchmark/report/`。

- [x] **步骤 1：先为报告目录的 CodeQL 排除写失败回归断言。**

```bash
# 断言源码包排除 benchmark/reports/ 与 benchmark/report/，
# 仍包含 benchmark/*.cpp、benchmark/tools/migrate_legacy_reports.py 和 xmake.lua。
```

- [x] **步骤 2：最小修改 CodeQL 排除规则并运行其回归测试。**

运行：

```bash
bash tests/test_codeql_discovery.sh
```

预期：新旧报告产物不进入源码包，迁移工具仍被纳入源码分析。

- [x] **步骤 3：更新根 README 与基准 README。**

```text
说明新层级、实际目标名、run-id/resume、RPS full/overload、托管环境、
外部 RPS_BASE_URL、历史只读迁移和按目标/profile 的 diff 用法。
```

- [x] **步骤 4：人工核对文档中不存在 `benchmark/reports/` 作为新写入路径。**

运行：

```bash
rg -n "benchmark/reports|benchmark/report|--resume|RPS_TARGET_FINGERPRINT" README.md benchmark/README.md
```

预期：旧路径仅以历史只读和迁移源的语义出现。

### Task 7：实施静态分析长任务超时策略

**文件：**
- 修改：`scripts/codeql.sh`
- 修改：`scripts/lint.sh`
- 修改：`tests/test_codeql_discovery.sh`
- 新建：`tests/test_lint_script.sh`
- 修改：`scripts/test.sh:22-28`
- 修改：`README.md` 的 Lint、CodeQL 和流水线章节
- 修改：`AGENTS.md` 的 CodeQL/Lint 接口约束

**接口：**
- `CODEQL_SUBMIT_TIMEOUT`：默认 `1800` 秒，范围 `600..7200`。
- `CODEQL_POLL_TIMEOUT`：默认 `1800` 秒，范围 `600..7200`。
- `CODEQL_POLL_INTERVAL`：默认 `5` 秒，必须为正整数；`0` 明确拒绝。
- `CODEQL_POLL_ATTEMPTS`：显式设置时保留兼容语义；未设置时按 `ceil(CODEQL_POLL_TIMEOUT / CODEQL_POLL_INTERVAL)` 动态计算。
- `CLANG_TIDY_TIMEOUT_SECONDS`：默认 `1800` 秒，范围 `600..7200`，按翻译单元生效。
- `CPPCHECK_TIMEOUT_SECONDS`：默认 `1800` 秒，范围 `600..7200`，按整次 cppcheck 生效。

- [x] **步骤 1：为 CodeQL timeout 和轮询参数编写失败回归测试。**

```bash
# 使用伪 curl/伪 submit/poll 函数，不实际等待。
# 断言默认提交/轮询均为 1800；599 拒绝；600/7200 接受；7201 拒绝。
# 未设置 CODEQL_POLL_ATTEMPTS 时，1800/5 得到约 360 次。
# CODEQL_POLL_INTERVAL=0、负数、非数字均在网络请求前拒绝。
```

- [x] **步骤 2：运行 CodeQL RED 测试。**

运行：

```bash
bash tests/test_codeql_discovery.sh
```

预期：新 timeout 默认值、边界或轮询计算断言失败，既有服务器探测断言保持通过。

- [x] **步骤 3：实现 CodeQL 提交和轮询总预算。**

```bash
# 提交与轮询使用独立总预算；短连接/发现/单次结果请求仍保持短 timeout。
# 每次轮询不得超过总预算；显式 attempts 只能作为兼容上限，不能绕过参数校验。
# 错误信息包含变量名、实际值和合法范围，不输出密钥或响应敏感内容。
```

- [x] **步骤 4：实现 clang-tidy/cppcheck timeout 校验与执行包装。**

```bash
# clang-tidy 每个翻译单元用 timeout --signal=TERM --kill-after=60s 包装。
# cppcheck 整次调用用同样的 timeout 包装。
# 超时返回非零，明确显示工具、目标文件或扫描范围。
```

- [x] **步骤 5：新增静态分析伪工具回归测试并接入测试入口。**

运行：

```bash
bash tests/test_lint_script.sh
bash scripts/test.sh
```

预期：默认值、边界、非法配置、超时和参数透传全部通过；不等待真实的 600 秒。

- [x] **步骤 6：同步文档并执行局部质量检查。**

运行：

```bash
bash scripts/lint.sh --changed
```

预期：timeout 变量、动态轮询规则、短网络 timeout 区别和失败语义在文档及脚本帮助中一致。

### Task 8：执行实际性能任务和完整质量闭环

**文件：**
- 仅生成：`benchmark/report/**` 的新测试报告与迁移产物。
- 不修改：`benchmark/reports/**` 的旧原件。

**接口：**
- 消费：前七个任务已验证的脚本入口。
- 产出：全部微基准、全部 QPS `full`、RPS `full` 与 `overload` 的实际报告。

- [x] **步骤 1：格式化并执行脚本回归、迁移回归与全量测试。**

运行：

```bash
bash scripts/format.sh all
bash scripts/test.sh
```

预期：Google Test、Vitest 和所有脚本回归测试通过。

- [x] **步骤 2：运行全部微基准。**

运行：

```bash
bash scripts/benchmark.sh micro
```

预期：动态发现的每个 `bench_*` 目标都有 passed manifest。

- [x] **步骤 3：运行全部 QPS 的完整阶梯。**

运行：

```bash
bash scripts/benchmark.sh qps full
```

预期：动态发现的每个 `qps_*` 目标都有 full profile 的 passed manifest。

- [x] **步骤 4：运行 RPS `full` 与 `overload`。**

运行：

```bash
bash scripts/benchmark.sh rps full overload
```

预期：未设置 `RPS_BASE_URL` 时创建并最终销毁专用 Docker 项目；full 全部严格单元通过，overload 全部单元完成且允许 `overloaded` 状态。

- [x] **步骤 5：验证目录、清单、差异对比与迁移结果。**

运行：

```bash
bash scripts/benchmark.sh diff micro <实际_bench_目标>
bash scripts/benchmark.sh diff qps <实际_qps_目标> full
bash scripts/benchmark.sh diff rps full
```

预期：每个命令只读取新结构中相同选择器的最近两份 complete manifest；若报告数量不足，明确返回“至少需要两个”而非读取旧格式。

- [x] **步骤 6：执行正式质量流水线。**

运行：

```bash
bash scripts/pipeline.sh all
```

实际：格式化、Lint、编译、CodeQL、测试依序通过；CodeQL 为 `0 critical + 0 high`。

## 自检结果

- 范围覆盖：报告分层、目标级与单元级续跑、RPS 隔离环境、历史只读迁移、新结构 `diff`、文档、CodeQL 排除、实际基准和正式门禁均有对应任务。
- 占位符检查：无待定实现项；迁移不删除旧原件，Docker 资源删除仅限唯一的托管项目。
- 接口一致性：`run-id`、`resume`、`manifest.json`、`RPS_TARGET_FINGERPRINT` 与模块/profile 选择器在所有任务中使用同一含义。
