# P8 Session Command Facade Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 完成 P8.4a：把当前 SSH 专用的 MCP 命令执行路径迁移到 Session 级通用命令 DTO、Executor 与 Facade，同时保持五个 MCP 工具、`schemaVersion=1` 和现有 SSH 行为兼容。

**Architecture:** `McpService` 只调用 `TerminalSession` 暴露的 `SessionCommandFacade`，不再识别或转换 `SshTransport`。Facade 根据 Session 当前绑定的 Transport 创建受控 Executor；本阶段只实现 SSH Executor，LocalShell、Serial、Telnet 和 Custom 明确返回不可用，不注入任何字节。SSH Executor 适配现有独立 exec channel，并把结果归一化为通用 `CommandExecutionResult`。

**Tech Stack:** C++17、Qt 6.8 Core/Network、Qt signals/slots、CMake 3.20+、QTest、libssh。

**Spec:** `docs/architecture/stages/P8_AI_MCP_Interface.md` §3、§5.4.2、§7.4、§7.5、§11、§14.1。

## Global Constraints

- 保持 Parser 单写、View 拥有 Session、Transport 只负责字节链路；不向 `ITransport` 增加 MCP 命令接口。
- 保持 `novaterm_list_sessions`、`novaterm_read_context`、`novaterm_search_context`、`novaterm_list_commands`、`novaterm_execute_command` 与 `schemaVersion=1` 不变。
- 本阶段只有 SSH Executor 可执行；LocalShell、Serial、Telnet、Custom 不得发送任何命令字节。
- 不开放自由 shell、任意按键、凭据读取、删除、覆盖、提权、解释器、任意文件读取或网络命令。
- 命令仍由 `CommandPolicy` 生成固定 recipe；MCP 请求只携带 `commandId + {}`。
- 保持原始 stdout/stderr 合计 64 KiB、转换后 UTF-8 合计 64 KiB、5 秒执行预算、票据去重与目标 quarantine 语义。
- 所有新增 C++ 文件使用中文 Doxygen，成员使用 `_camelCase`，查询函数添加 `[[nodiscard]]`。
- 不修改用户尚未提交的 P8 设计内容；只在完成和验证后向 §14 增补实际实施事实。
- 本计划不创建 Git 提交；若用户之后要求提交，必须先把根 `CMakeLists.txt` 的 PATCH 版本递增，并避免把无关用户改动并入提交。

## Review Focus

- Session 重连或 Transport 换绑后，旧 Executor 的迟到结果必须被代际校验丢弃，不能完成新世代的 MCP 请求。
- 非 SSH Session 即使已获得 execute_commands 授权，也必须零字节注入并返回兼容的不可执行结果。
- 同一 commandTicket 重试只能查询原记录，Facade 重构不能造成第二次 SSH 提交。
- MCP 取消、服务停止和 Session 销毁必须通过 Facade 取消当前 Executor 请求；关闭通道不等于确认远端终止。
- SSH Executor 的 target fingerprint 只能由已认证主机密钥、端口和用户名组成，不得读取密码、私钥或 credentialRef。

---

### Task 1: 通用命令执行 DTO

**Files:**
- Create: `src/session/CommandExecutionTypes.h`
- Modify: `src/transport/SshTransport.h`
- Modify: `src/transport/SshTransport.cpp`
- Delete: `src/transport/SshCommandTypes.h`
- Modify: `tests/mcp/McpTests.cpp`

**Interfaces:**
- Produces: `CommandExecutionMode`、`CommandExecutionOutcome`、`CommandExecutionLimits`、`CommandExecutionRequest`、`CommandExecutionResult`、`CommandExecutorCapabilities`。
- Preserves: `SshTransport::executeBoundedCommand(quint64, QByteArray, CommandExecutionLimits)` 与 `boundedCommandFinished(const CommandExecutionResult&)` 的异步行为。

- [ ] **Step 1: 写失败测试，固定通用 DTO 的 SSH 完成证据**

  在 `tests/mcp/McpTests.cpp` 的 SSH bounded command 测试中改用 `CommandExecutionResult` 与 `CommandExecutionOutcome`，断言正常退出、非零退出、输出超限和超时仍分别保留 `executionMayHaveStarted`、`terminationConfirmed`、`exitCode` 与 `outputTruncated`。

- [ ] **Step 2: 构建并确认测试先因通用类型不存在而失败**

  Run: `cmake --build build/Release --target novaterm_mcp_tests`

  Expected: 编译失败，错误指向 `CommandExecutionTypes.h` 或通用类型尚未定义。

- [ ] **Step 3: 添加最小通用 DTO 并迁移 SshTransport**

  `CommandExecutionRequest` 必含 `requestId`、由策略生成的 `QByteArray command` 和 `CommandExecutionLimits limits`；`CommandExecutionResult` 保留当前全部结构化完成证据。只做类型泛化，不改变 libssh worker、队列、超时和输出上限算法。

- [ ] **Step 4: 运行 MCP 目标并确认直接 SSH 测试通过**

  Run: `cmake --build build/Release --target novaterm_mcp_tests`

  Run: `$env:Path='C:\Programs\Qt\6.8.3\msvc2022_64\bin;'+$env:Path; $env:QT_PLUGIN_PATH='C:\Programs\Qt\6.8.3\msvc2022_64\plugins'; ctest --test-dir build/Release -C Release -R '^novaterm_mcp_tests$' --output-on-failure`

  Expected: `novaterm_mcp_tests` 通过，SSH bounded command 四种结果语义不变。

### Task 2: SessionCommandExecutor 与 SessionCommandFacade

**Files:**
- Create: `src/session/ISessionCommandExecutor.h`
- Create: `src/session/SshSessionCommandExecutor.h`
- Create: `src/session/SshSessionCommandExecutor.cpp`
- Create: `src/session/SessionCommandFacade.h`
- Create: `src/session/SessionCommandFacade.cpp`
- Modify: `src/session/TerminalSession.h`
- Modify: `src/session/TerminalSession.cpp`
- Modify: `src/mcp/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/mcp/McpTests.cpp`

**Interfaces:**
- Consumes: Task 1 的 `CommandExecutionRequest/Result` 与 `CommandExecutorCapabilities`。
- Produces: `SessionCommandFacade::configure(ITransport*, TransportKind, quint64 generation)`、`isAvailable()`、`capabilities()`、`targetFingerprint()`、`execute(const CommandExecutionRequest&)`、`cancel(quint64)` 和 `finished(const CommandExecutionResult&)`。
- Produces: `TerminalSession::commandFacade()`，只返回 Session 拥有的非 owning 门面指针。

- [ ] **Step 1: 写失败测试，固定路由和安全拒绝行为**

  添加以下测试：SSH Session 的 Facade 报告 `Isolated`、`isolatedOutput=true`、`reliableExitCode=true`；LocalFake/LocalShell Session 的 Facade 不可用；对不可用 Facade 调用 execute 返回 false，且 `LocalFake::written` 保持为空。

- [ ] **Step 2: 运行目标并确认因 Facade/Executor 不存在而失败**

  Run: `cmake --build build/Release --target novaterm_mcp_tests`

  Expected: 编译失败，错误指向 `SessionCommandFacade` 或 `commandFacade()` 尚未定义。

- [ ] **Step 3: 实现最小 Executor 接口和 SSH 适配器**

  `ISessionCommandExecutor` 是 QObject 多态接口，公开 capabilities/targetFingerprint/execute/cancel 与 finished 信号；析构为 public virtual。`SshSessionCommandExecutor` 只持有 `QPointer<SshTransport>`，把通用 request 映射到 `executeBoundedCommand()`，并转发通用结果。fingerprint 使用已认证主机密钥、端口和 trim 后用户名做 SHA-256，不读取认证秘密。

- [ ] **Step 4: 实现 Facade 生命周期和代际保护**

  `TerminalSession` 构造时创建 Facade；attach、重连 generation 变化、clearAttachment、resetForReuse 和析构路径都调用 configure。Facade 只为 `TransportKind::Ssh` 创建 SSH Executor；每次 configure 递增内部 generation，完成回调必须同时匹配 Executor 对象和 generation，旧回调直接丢弃。

- [ ] **Step 5: 更新 CMake 显式源列表并运行测试**

  把新 Session 命令文件加入 `novaterm_mcp_runtime`；检查 `novaterm_mcp_tests` 不重复编译同一实现。重新配置一次以确保新增文件进入所有目标。

  Run: `cmake -S . -B build/Release`

  Run: `cmake --build build/Release --target novaterm_mcp_tests`

  Run: `$env:Path='C:\Programs\Qt\6.8.3\msvc2022_64\bin;'+$env:Path; $env:QT_PLUGIN_PATH='C:\Programs\Qt\6.8.3\msvc2022_64\plugins'; ctest --test-dir build/Release -C Release -R '^novaterm_mcp_tests$' --output-on-failure`

  Expected: 新增 Facade 测试与既有 MCP 测试全部通过。

### Task 3: McpService 改走 Session 级门面

**Files:**
- Modify: `src/mcp/McpService.cpp`
- Modify: `src/session/SessionDirectory.cpp`
- Modify: `tests/mcp/McpTests.cpp`

**Interfaces:**
- Consumes: `TerminalSession::commandFacade()` 与 Task 2 的 Facade 接口。
- Preserves: 当前五工具 input/output schema、错误 envelope、commandTicket、policyVersion、执行记录和 quarantine。

- [ ] **Step 1: 写失败测试，固定 MCP 层不依赖 Transport 类型的行为**

  添加回归：已授权但当前 Facade 不可用的非 SSH Session，`novaterm_list_commands` 返回 `executionEnabled=false`、空 commands，`novaterm_execute_command` 不产生 Transport 写入；SSH Session 的目录、票据、首次提交、同票据执行中查询、完成后重放仍只提交一次。

- [ ] **Step 2: 运行 MCP 测试并确认新断言失败**

  Run: `$env:Path='C:\Programs\Qt\6.8.3\msvc2022_64\bin;'+$env:Path; $env:QT_PLUGIN_PATH='C:\Programs\Qt\6.8.3\msvc2022_64\plugins'; ctest --test-dir build/Release -C Release -R '^novaterm_mcp_tests$' --output-on-failure`

  Expected: 非 SSH Facade 错误/零写入或 Session 级调用尚未满足断言。

- [ ] **Step 3: 移除 McpService 的 SSH 强耦合**

  `Execution` 保存 `QPointer<SessionCommandFacade>` 而不是 `QPointer<SshTransport>`；list/execute/cancel/stop/finish 全部通过 Facade。McpService 不包含 `SshTransport.h`，不调用 `qobject_cast<SshTransport*>`，也不直接调用 `executeBoundedCommand/cancelCommand`。请求 ID、票据、授权、TargetGuard 和 JSON 映射保持原逻辑。

- [ ] **Step 4: 让 SessionDirectory 从 Facade 获取目标身份**

  删除 `SessionDirectory.cpp` 中的 SSH cast 和目标散列实现，改用 `session->commandFacade()->targetFingerprint()`；Directory 仍只保存非秘密指纹并负责 epoch/attachmentId 变化检测。

- [ ] **Step 5: 运行 MCP 与 SSH 精确测试**

  Run: `cmake --build build/Release --target novaterm_mcp_tests novaterm_ssh_transport_check`

  Run: `$env:Path='C:\Programs\Qt\6.8.3\msvc2022_64\bin;'+$env:Path; $env:QT_PLUGIN_PATH='C:\Programs\Qt\6.8.3\msvc2022_64\plugins'; ctest --test-dir build/Release -C Release -R '^(novaterm_mcp_tests|novaterm_ssh_transport_check)$' --output-on-failure`

  Expected: 两个目标通过；MCP 输出 schema fixture 未变化。

### Task 4: 文档同步与阶段验证

**Files:**
- Modify: `docs/architecture/stages/P8_AI_MCP_Interface.md`
- Modify: `docs/MCP_Usage.md` only if user-visible behavior or wording changed
- Modify: `docs/superpowers/plans/2026-09-19-p8-session-command-facade.md`

**Interfaces:**
- Consumes: Tasks 1–3 的测试证据。
- Produces: §14 中准确的 P8.4a 实施记录；不得声称 LocalShell、Serial、Telnet 或 Custom 已支持命令执行。

- [ ] **Step 1: 更新 P8 §14 的当前代码事实**

  记录通用 DTO、SessionCommandFacade/Executor Router 和 SSH adapter 已实现；把“通用门面尚不存在”改成新的真实状态。保留 v0.5 其余待实施清单，并注明 LocalShell、InteractiveFramed、CommandLease 和 Custom Executor 仍未实现。

- [ ] **Step 2: 检查用户文档是否需要变化**

  由于本阶段不新增用户可见能力，`docs/MCP_Usage.md` 原则上保持“仅 SSH 固定诊断命令”。只有实现改变了用户可见错误或步骤时才做最小同步。

- [ ] **Step 3: 运行最终构建与相关测试**

  Run: `cmake --build build/Release --target NovaTerm novaterm-mcp novaterm_mcp_tests novaterm_ssh_transport_check`

  Run: `$env:Path='C:\Programs\Qt\6.8.3\msvc2022_64\bin;'+$env:Path; $env:QT_PLUGIN_PATH='C:\Programs\Qt\6.8.3\msvc2022_64\plugins'; ctest --test-dir build/Release -C Release -R '^(novaterm_mcp_tests|novaterm_ssh_transport_check)$' --output-on-failure`

  Expected: 构建成功，两个测试目标通过；未运行全套测试，因为改动集中于 MCP/Session/SSH 命令路径且未改变通用 Transport 接口。

- [ ] **Step 4: 审查工作树和文档准确性**

  Run: `git diff --check`

  Run: `git status --short`

  Run: `rg -n "qobject_cast<SshTransport|SshCommand(Result|Outcome|Limits)|SshCommandTypes" src/mcp src/session tests/mcp`

  Expected: 无空白错误；MCP/Session 不再依赖 SSH 专用命令类型或 cast；P8 §14 只宣称本阶段实际完成内容。
