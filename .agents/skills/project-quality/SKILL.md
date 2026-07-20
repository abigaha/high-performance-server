---
name: project-quality
description: 统一执行本仓库的测试、C/C++ 格式化、clang-tidy 与 cppcheck 静态检查、两阶段质量检查、CodeQL 安全分析及 Git 变更只读审查。用户要求 test、format、lint、check、codeql、review，或要求验证测试通过率、代码风格、静态分析与安全质量门禁时使用。
---

# 项目质量门禁

## 选择模式

根据用户明确指定的模式执行 `test`、`format`、`lint`、`check`、`codeql` 或 `review`。保留用户提供的测试名、路径、`--changed` 和并发数等参数；参数含空格时正确引用。

一旦选择 `review`，将整个调用置于只读隔离模式。即使请求同时包含其他模式，也只执行审查并在报告中注明未执行项；要求用户通过另一次调用执行会运行脚本或修改文件的操作。

## 遵守共同约束

- 从仓库根目录执行，先遵守 `AGENTS.md`。
- 始终调用现有 `scripts/*.sh`，不得临时拼装等价的 `xmake test`、clang-format、clang-tidy、cppcheck、curl、打包或 SARIF 解析命令。
- 脚本能力不足时，先补强对应脚本，再运行脚本；脚本报告代码问题时，修复业务代码，不得修改脚本来迁就错误代码。
- 完整展示脚本原始输出，不得用 `head`、`tail`、`grep -v`、`sed` 等逐行过滤或截断报告。
- 除 `review` 和 `format` 外，失败后分析并修复代码，重新执行同一门禁直至通过；无法继续时明确报告阻塞原因和未通过项。
- 全程使用中文报告；命令、路径、规则标识符和代码保持原样。

## 执行模式

### test

- 无测试名时执行 `bash scripts/test.sh`。
- 指定测试名时执行 `bash scripts/test.sh "<测试名>"`。
- 要求 Google Test 100% 通过；失败时修复代码并重跑。

### format

- 无路径时执行 `bash scripts/format.sh all`，避免进入交互菜单。
- 指定文件或目录时执行 `bash scripts/format.sh <路径...>`。
- 仅格式化 C/C++ 文件，并采用脚本内置的工具探测与排除规则；完成后报告实际范围。

### lint

- 全量检查执行 `bash scripts/lint.sh`。
- 增量检查执行 `bash scripts/lint.sh --changed`。
- 局部检查传入文件或目录；并发数按需传入 `-j N` 或 `--jobs N`。
- 要求 clang-tidy 为 0 error、0 warning、0 style，cppcheck 为 0 error、0 warning、0 style、0 performance。

### codeql

- 执行 `bash scripts/codeql.sh run`，避免进入交互菜单。
- 让脚本负责服务器发现、编译数据库处理、源码打包、任务轮询和 SARIF 解析，不得绕过脚本。
- 要求 0 critical、0 high。发现问题时修复代码并重跑。
- 按“规则 ID / 严重级别 / 文件:行 / 描述”报告问题，并报告 CodeQL 执行状态或连接阻塞。

### check

1. 执行 `bash scripts/lint.sh`；未通过时修复代码并重复此阶段。
2. 执行 `bash scripts/codeql.sh run`；发现问题时修复代码并返回第一阶段重新验证。
3. 分阶段报告结果。总门禁要求 lint 全部为零且 CodeQL 为 0 critical、0 high。

### review

- 仅允许执行 `git status`、`git diff`、`git log` 及其只读选项；可用 `git diff --cached` 检查暂存内容，用 `git diff --no-index` 查看未跟踪文本文件。
- 禁止读取其他命令输出、运行任何质量脚本、构建或测试，禁止修改文件。
- 先输出按严重、高、中、低排序的具体发现，每项注明文件与行号；再列出假设、覆盖缺口和简短总结。
- 没有发现时明确说明，并指出因只读命令限制而未覆盖的风险；只输出中文审查报告。
