# P8 LocalShell Isolated Executor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 Windows LocalShell Session 增加独立、固定、可取消的本机诊断命令执行能力，不向当前 PTY/ConPTY 注入任何字节，并保持现有 MCP 五工具与 `schemaVersion=1` 兼容。

**Architecture:** 新增同目录控制台 helper `novaterm-local-diag.exe`，只接受四个固定内部诊断 ID，通过 Windows/Qt 系统 API 生成有界文本，不接受 shell、脚本、路径、环境、cwd 或附加参数。`LocalSessionCommandExecutor` 使用 `QProcess` 启动 helper，独立收集 stdout/stderr、退出码、超时与取消证据；`CommandPlatformProfile` 把 LocalShell Windows Session 映射到固定 helper recipe。Linux/macOS 本阶段保持 `executionEnabled=false`，不回退到 shell 注入或 PATH 搜索。

**Tech Stack:** C++17、Qt 6.8 Core、QProcess、QStorageInfo、Windows API、CMake 3.20+、QTest。

**Spec:** `docs/architecture/stages/P8_AI_MCP_Interface.md` §5.4.1、§5.4.2、§5.4.4、§7.4、§7.5、§9、§11、§14.1。

## Global Constraints

- LocalShell 命令必须运行在独立子进程中；不得调用 `TerminalSession::writeUserInput()`、`ITransport::write()` 或当前 PTY/ConPTY。
- Helper 仅接受 `system.identity`、`system.uptime`、`memory.summary`、`filesystem.usage`，参数数量和内容固定；未知或额外参数以非零退出拒绝。
- 不调用 `cmd /c`、PowerShell、解释器、脚本、PATH 搜索、重定向、管道或任意外部网络程序。
- Helper 使用应用目录下的绝对路径；缺失时返回 `COMMAND_UNAVAILABLE`，不尝试替代程序。
- 原始 stdout/stderr 合计最多 64 KiB，转换后的 UTF-8 仍由 MCP 层另限 64 KiB。
- 每个 Local Executor 同时最多一个请求；总并发、票据、授权、去重、目标保护继续由现有 Facade/McpService 约束。
- 超时或取消必须终止 helper 并等待进程退出；只有确认进程结束后才能设置 `terminationConfirmed=true`。
- 本阶段只在 Windows 注册 `windows-local-v1`；其他平台不发布 LocalShell 命令能力。
- MCP 工具名、输入输出 schema 和 `schemaVersion=1` 不变。
- 使用 `scripts/build-novaterm.bat` 编译；只运行 MCP、Session、TerminalSession 和新增 helper 相关测试，不运行全套。
- 完成代码提交时按项目约定把版本从 `0.2.21` 递增到 `0.2.22`。

## Review Focus

- Helper 参数中混入额外字段、路径或 shell 片段时必须在启动诊断前拒绝。
- QProcess 同步启动失败、崩溃、非零退出、超时和取消必须映射为不同的结构化完成证据。
- Session 关闭、换绑或 MCP 取消时 helper 不得成为遗留后台进程。
- Local 命令输出不得出现在 TerminalCore、当前终端屏幕或 LocalShellTransport::write()。
- `targetFingerprint` 只能使用散列后的稳定本机标识和 Profile 版本，不返回 machineUniqueId 原文。

---

### Task 1: 固定本机诊断 Helper

**Files:**
- Create: `tools/novaterm-local-diag/CMakeLists.txt`
- Create: `tools/novaterm-local-diag/main.cpp`
- Create: `src/session/LocalDiagnosticProtocol.h`
- Modify: `CMakeLists.txt`
- Modify: `tests/mcp/McpTests.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `NovaTerm::LocalDiagnostic::isKnownCommand(QStringView)` 与四个固定命令 ID。
- Produces: `novaterm-local-diag <commandId>`，成功时 stdout 为 UTF-8 文本、退出码 0；非法参数退出码 2；采集失败退出码 1 且只写有界 stderr。

- [ ] **Step 1: 写失败测试固定 helper 参数边界和四项成功输出**

  在 `McpTests.cpp` 增加子进程测试：逐项启动 `NOVATERM_LOCAL_DIAG <commandId>`，断言退出码 0、stdout 非空且 stderr 为空；再传未知 ID、额外参数和 shell 片段，断言退出码 2 且不执行任何外部命令。

- [ ] **Step 2: 用指定脚本构建并确认因 helper 目标/路径不存在而失败**

  Run: `scripts\build-novaterm.bat`

  Expected: `novaterm_mcp_tests` 编译或依赖阶段失败，指出 `NOVATERM_LOCAL_DIAG` 或 helper 目标不存在。

- [ ] **Step 3: 实现 Windows helper 的四项系统 API 采集**

  - identity：`QSysInfo::prettyProductName/kernelType/kernelVersion/currentCpuArchitecture`。
  - uptime：`GetTickCount64()` 转换为稳定的秒数文本。
  - memory：`GlobalMemoryStatusEx()` 输出总量、可用量与负载百分比。
  - filesystem：`QStorageInfo::mountedVolumes()`，只输出 ready、valid 且非只读过滤无关的卷信息，逐项输出 rootPath/bytesTotal/bytesAvailable；总输出在 helper 内预先限制到 64 KiB。

- [ ] **Step 4: 添加 Qt console target 与测试依赖**

  使用 `qt_add_executable(novaterm-local-diag main.cpp)`，链接 `Qt6::Core`，设置 console 子系统；`novaterm_mcp_tests` 通过编译定义取得 `$<TARGET_FILE:novaterm-local-diag>` 并显式依赖该目标。非 Windows 目标仍可编译，但返回“不支持的平台”退出码 1，且不注册 MCP 能力。

- [ ] **Step 5: 使用指定脚本构建并运行 helper 测试**

  Run: `scripts\build-novaterm.bat`

  Run: `ctest --test-dir build/Release -C Release -R '^novaterm_mcp_tests$' --output-on-failure`

  Expected: helper 四项成功，非法参数三类拒绝，MCP 测试通过。

### Task 2: LocalSessionCommandExecutor

**Files:**
- Create: `src/session/LocalSessionCommandExecutor.h`
- Create: `src/session/LocalSessionCommandExecutor.cpp`
- Modify: `src/session/CommandExecutionTypes.h`
- Modify: `src/mcp/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `tests/mcp/McpTests.cpp`

**Interfaces:**
- Consumes: `CommandExecutionRequest.commandId` 和 Task 1 的固定命令 ID。
- Produces: `LocalSessionCommandExecutor::execute/cancel`，通过既有 `finished(CommandExecutionResult)` 返回结构化证据。
- Produces: `CommandExecutionResult` 的 Local 语义：成功/非零/启动失败/超时/取消/输出超限均保留 `executionMayHaveStarted`、`terminationConfirmed`、`exitCode` 与独立 stdout/stderr。

- [ ] **Step 1: 写失败测试固定独立执行、输出与取消行为**

  使用测试自身的 fixture 模式作为可控 helper：覆盖成功退出、退出码 42、stdout/stderr 合计超限、超时、显式取消和并发第二请求拒绝；测试同时断言 LocalFake::written 始终为空。

- [ ] **Step 2: 构建并确认因 LocalSessionCommandExecutor 不存在而失败**

  Run: `scripts\build-novaterm.bat`

  Expected: 编译失败，错误指向新的 Executor API 尚不存在。

- [ ] **Step 3: 扩展请求 DTO 为语义命令 ID**

  给 `CommandExecutionRequest` 增加由服务端策略填写的 `QString commandId`；保留 SSH 的固定 `QByteArray command`，Local Executor 只读取 commandId 并拒绝空值或未知值。MCP 原始 JSON 不直接进入 Executor。

- [ ] **Step 4: 实现有界异步 QProcess 生命周期**

  Executor 固定 helper 绝对路径和最小环境，不设置 shell；启动后立即读取 stdout/stderr，任何一次读取使 `executionMayHaveStarted=true`。用单次 `QTimer` 实现 timeout；取消/超时时先 `terminate()`，短暂等待后 `kill()`，只有 `QProcess::finished` 或 `waitForFinished` 证明确认结束才设置 terminationConfirmed。输出达到预算时保留有界前缀、置 outputTruncated 并终止子进程。

- [ ] **Step 5: 用指定脚本构建并运行 Executor 测试**

  Run: `scripts\build-novaterm.bat`

  Run: `ctest --test-dir build/Release -C Release -R '^novaterm_mcp_tests$' --output-on-failure`

  Expected: Local Executor 的六类结果全部通过，LocalFake 零写入。

### Task 3: Windows Local CommandPlatformProfile 与 MCP 接入

**Files:**
- Create: `src/session/CommandPlatformProfile.h`
- Create: `src/session/CommandPlatformProfile.cpp`
- Modify: `src/mcp/CommandPolicy.h`
- Modify: `src/mcp/CommandPolicy.cpp`
- Modify: `src/session/SessionDirectory.cpp`
- Modify: `src/session/LocalSessionCommandExecutor.cpp`
- Modify: `src/mcp/McpService.cpp`
- Modify: `src/mcp/McpAccess.cpp`
- Modify: `tests/mcp/McpTests.cpp`

**Interfaces:**
- Produces: `CommandPlatformProfile::forSession(TransportKind, RuntimeConfig)`，Windows LocalShell 返回 `windows-local-v1` 与四项固定 helper recipe；SSH 返回现有 `linux-diagnostics-v1`；其他组合返回 unavailable。
- Produces: 每个 Profile 的 `supportedCommandIds()`、`policyVersion()` 与非秘密 `targetFingerprint()` 输入。
- Preserves: commandTicket、policyVersion、arguments `{}`、TargetGuard、执行记录和 schema v1。

- [ ] **Step 1: 写失败端到端测试固定 LocalShell MCP 行为**

  创建 Running LocalShell fixture，授予四项命令；断言 list_sessions 含 list/execute capability、list_commands 返回四项固定模板、execute_command 调用 Local Executor 并得到结构化结果；再次断言当前 LocalFake/PTY 输入为空。同票据重放不得启动第二个 helper。

- [ ] **Step 2: 运行 MCP 测试并确认 LocalShell 仍为 disabled**

  Run: `ctest --test-dir build/Release -C Release -R '^novaterm_mcp_tests$' --output-on-failure`

  Expected: LocalShell `executionEnabled=false` 或 commands 为空。

- [ ] **Step 3: 实现最小可信 Profile 与按 Profile 过滤目录**

  `CommandPolicy::catalog(profile)` 只返回该 Profile 支持的 commandId；ticket 内部加入 profileVersion 和 Facade binding generation。Local Session 的目标指纹为 `SHA-256(machineUniqueId + profileVersion)`，machineUniqueId 为空时不开放执行，不降级到主机名或随机值。

- [ ] **Step 4: SessionDirectory 为 Windows LocalShell 安装 Executor**

  仅在 `Q_OS_WIN`、Session Running 且 helper 路径存在时创建 `LocalSessionCommandExecutor`；SSH 路径保持现状。Linux/macOS、Serial、Telnet、Custom 返回空 Executor。Session epoch/Transport 换绑继续触发 Facade reset，取消在途 helper。

- [ ] **Step 5: 运行 MCP、Session 与 TerminalSession 精确测试**

  Run: `scripts\build-novaterm.bat`

  Run: `ctest --test-dir build/Release -C Release -R '^(novaterm_mcp_tests|novaterm_session_tests|novaterm_terminal_session_tests|novaterm_ssh_transport_check)$' --output-on-failure`

  Expected: 4/4 相关目标通过；SSH 票据、输出上限、取消、quarantine 与 schema fixture 无回归。

### Task 4: 文档、版本与提交准备

**Files:**
- Modify: `docs/architecture/stages/P8_AI_MCP_Interface.md`
- Modify: `docs/MCP_Usage.md`
- Modify: `CMakeLists.txt`
- Modify: `docs/superpowers/plans/2026-09-19-p8-local-shell-executor.md`

**Interfaces:**
- Consumes: Tasks 1–3 的构建与测试证据。
- Produces: P8 §14 对 Windows LocalShell Isolated Executor 的准确实施记录；使用说明明确本地诊断不继承当前 Shell 的 cwd/alias/history，Linux/macOS Local 命令仍未开放。

- [ ] **Step 1: 同步 P8 与用户说明**

  只把 Windows LocalShell helper/Executor/Profile 写入“已实现”；Serial/Telnet InteractiveFramed、CommandLease、Custom Executor、Linux/macOS Local 验收继续列为未实现。说明 helper 不向当前终端输入，不读取凭据，不接受任意参数。

- [ ] **Step 2: 递增版本到 0.2.22**

  修改根 `project(NovaTerm VERSION ...)`，不改 MINOR。

- [ ] **Step 3: 最终相关模块验证**

  Run: `scripts\build-novaterm.bat`

  Run: `ctest --test-dir build/Release -C Release -R '^(novaterm_mcp_tests|novaterm_session_tests|novaterm_terminal_session_tests|novaterm_ssh_transport_check)$' --output-on-failure`

  Expected: 指定脚本成功，四个相关测试目标通过；不运行全套。

- [ ] **Step 4: 差异与安全边界检查**

  Run: `git diff --check`

  Run: `rg -n "writeUserInput|ITransport::write|cmd(\.exe)? /c|powershell|shell -c" src/session/LocalSessionCommandExecutor.* tools/novaterm-local-diag`

  Expected: Local Executor/helper 不包含交互会话写入或 shell/解释器入口；文档未把其他平台或 InteractiveFramed 写成已完成。
