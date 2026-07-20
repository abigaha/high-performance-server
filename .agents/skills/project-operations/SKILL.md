---
name: project-operations
description: 统一执行本仓库 C++/xmake 项目的构建、运行、清理、GDB 调试、xmake 目标创建和开发环境检查。用户要求 build、run、clean、debug、add-target、env-check，或用中文提出编译项目、运行目标、清理产物、调试程序、添加构建目标、检查工具链时使用。
---

# 项目操作

将请求归一化为 `build`、`run`、`clean`、`debug`、`add-target` 或 `env-check`，保留目标名和程序参数。执行前读取仓库根目录的 `AGENTS.md`，以其中规则为准；始终使用中文报告结果。

## 解析目标

仅在 `run`、`debug` 和 `add-target` 需要目标时执行以下流程：

1. 先运行 `xmake show -l targets` 获取实际目标；若命令不可用或配置失败，再搜索全部 `xmake.lua` 中的 `target(...)`、动态目标循环和 `set_kind(...)`。
2. `run` 和 `debug` 只选择 `binary` 目标，排除 `shared`、`static`、`headeronly` 及仅用于依赖的目标。
3. 用户给出目标时，先验证目标存在且类型适合；名称近似时列出匹配项，不擅自运行另一个目标。
4. 用户未给出目标时，结合请求语义、默认目标和可执行目标清单推断。只有一个合理候选时直接使用；有多个且无法消歧时，展示候选及用途后再询问；没有候选时说明检查结果。禁止未检查目标就直接询问。
5. 将目标参数与传给程序的参数分开；先确认 xmake 参数边界，避免把程序参数误当成目标。

## 执行操作

### build

- 普通构建运行 `bash scripts/compile.sh build`。
- 用户明确要求清缓存、重新配置或全量重建时运行 `bash scripts/compile.sh --clean`。
- 不直接调用 `xmake` 代替构建脚本。编译失败时定位并修复业务代码，再重复运行同一脚本，直至达到 0 error、0 warning；脚本能力不足时先改进 `scripts/compile.sh`。

### run

- 按“解析目标”确定可执行目标，再运行 `xmake run <target>`；存在程序参数时按 xmake 支持的参数边界追加。
- 保留完整原始输出和退出码。失败时先判断是构建、运行环境还是程序错误；只有请求包含修复授权时才修改代码。

### clean

- 运行 `xmake clean` 清理产物，再运行 `xmake f -c -y` 清理配置缓存并重新配置。
- 这是清理操作，不在结束时自动构建，除非用户同时要求重建；重建必须使用 `bash scripts/compile.sh build`。

### debug

- 按“解析目标”确定可执行目标，使用 `xmake run -d <target>` 进入 GDB；需要程序参数时按 xmake 支持的参数边界追加。
- 先复现问题，再收集断点、调用栈、线程和变量等证据。输出中文诊断；用户要求修复时修改业务代码，并使用 `bash scripts/compile.sh build` 重新编译后复验。

### add-target

1. 从请求中提取目标名、种类、源码位置、依赖和用途；缺少目标名时先检查现有目标与目录，并尝试从明确的模块或文件名推断，无法唯一推断才询问。
2. 搜索全部 `xmake.lua` 和候选源码目录，拒绝重复目标、路径冲突以及 `_latest`、`latest` 命名。QPS 二进制遵守 `qps_<模块名>` 格式。
3. 优先修改所属模块最近的 `xmake.lua`；只有跨模块或根级自动发现目标才修改根配置。复用现有 `set_kind`、`add_files`、`add_includedirs`、`add_deps`、输出目录和运行目录模式。
4. 仅创建目标所需的最小源码目录与文件，遵守 C++20 和仓库命名规范，不生成占位文档。
5. 运行 `bash scripts/compile.sh build` 验证新目标；若新增的是测试、基准或 QPS 目标，再使用仓库对应脚本验证其行为。

### env-check

1. 依次检查 `g++ --version`、`xmake --version`、`cppcheck --version`、可用的 `clang-tidy` 版本和 `gdb --version`，记录命令是否存在及首行版本信息。
2. 检查 CodeQL 连通性时，若 `CODEQL_SERVER_URL` 已设置则先探测该地址；否则探测 `http://localhost:8080`。仅当前两者都失败时才询问服务器 IP，端口固定为 `8080`，并在当前进程环境中设置 `CODEQL_SERVER_URL` 后重试。环境检查只验证连通性，不提交分析任务。
3. 用中文表格完整列出组件、状态、版本或地址及失败原因；不得用过滤命令截断脚本或工具的重要诊断输出。

## 完成报告

报告实际执行的操作、目标、命令结果和退出状态。涉及修改时列出文件与验证结果；未执行或失败的检查必须明确说明原因，不把“命令存在”表述为质量门禁通过。
