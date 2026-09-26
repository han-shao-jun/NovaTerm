# NovaTerm 交互式 MCP 终端“手”实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 NovaTerm MCP 命令统一改为 Session 级交互事务，使命令与输出进入当前终端 UI，并增加风险确认与 SSH/LocalShell 脚本能力。

**Architecture:** `TerminalSession` 拥有输入仲裁、命令 Lease、提示符状态与交互执行协调器；出站统一经过 `SessionInputArbiter`，入站统一经过增强后的 `SessionInputPump` 和 `InteractiveStreamFramer`。MCP Bridge 负责 2025 elicitation 与 2026 MRTR，GUI 服务负责授权、风险、状态和执行记录，Transport 仍只处理字节与连接。

**Tech Stack:** C++17、Qt 6.8、Qt Test、QLocalSocket、libssh/SFTP、libvterm、CMake/CTest。

**Spec:** `docs/architecture/stages/P8_AI_MCP_Interface.md` §15

## Global Constraints

- Parser 单写，Renderer/Search 只读；MCP 不得建立第二条 Parser 或终端日志通路。
- `src/core/` 不新增 Qt 依赖；本计划新增类型位于 `src/session/`、`src/mcp/` 和 Bridge。
- Transport 只处理字节和连接状态；MCP JSON、风险与确认不能进入 `ITransport`。
- 所有跨线程队列必须有上限、统计和停止语义。
- 用户键盘/粘贴优先；抢占后不自动重放，不自动发送 Ctrl-C、Esc 或 Enter。
- SSH/LocalShell 仅在可信 shell integration 确认空闲提示符时注入；Serial/Telnet 仅在显式 Profile 就绪时注入。
- 内部 marker/framing 必须在进入 TerminalCore 前剥离。
- 客户端无 elicitation 时仅执行 `Allow`；`Confirm`、`Unknown` 与脚本必须拒绝。
- 脚本确认前不落盘；正文不进入终端 UI；调用命令和输出正常进入 UI。
- 保留现有五个 MCP 工具与 `schemaVersion=1`；新增工具不得改变旧工具必填字段。
- 新建线程入口第一行调用 `NovaTerm::setCurrentThreadName()`，名称 ASCII 且不超过 15 字节。
- 注释和 Doxygen 使用中文；成员使用 `_camelCase`；查询函数使用 `[[nodiscard]]`。
- 保留工作树现有的 `AGENTS.md`、P8 文档、`McpProtocol.cpp` 与 `McpTests.cpp` 未提交改动，修改重叠处逐块合并。
- 每次创建提交都将根 `CMakeLists.txt` 的 PATCH 版本递增 1，并在提交消息中列出实际测试。

## Review Focus

- Marker 被远端输出按任意边界拆分或伪造：Task 2 的 split/forged nonce 测试必须证明 UI 不泄漏且事务不误完成。
- 人类确认返回后 Session/prompt 已变化：Task 6 的 stale confirmation 测试必须证明零字节注入。
- 用户在 MCP 只发送了一部分命令时输入：Task 1/3 的抢占测试必须证明停止剩余字节并返回 unknown。
- 脚本目标已存在或确认后写入失败：Task 7 必须证明覆盖信息参与确认，写失败不提交调用命令。
- 无法识别的 Shell/Serial/Telnet 提示符：Task 4 必须证明能力不发布且不发送探测字符。

---

### Task 1: Session 输入仲裁与用户抢占

**Files:**
- Create: `src/session/SessionInputArbiter.h`
- Create: `src/session/SessionInputArbiter.cpp`
- Modify: `src/session/TerminalSession.h`
- Modify: `src/session/TerminalSession.cpp`
- Modify: `tests/session/SessionTests.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `ITransport::write(const QByteArray&)`、Session generation。
- Produces: `SessionInputArbiter::acquireMcpLease()`、`submitMcpInput()`、`submitUserInput()`、`releaseMcpLease()` 与 `mcpPreempted()`。

- [ ] **Step 1: 写输入来源和抢占失败测试**

在 `SessionTests.cpp` 增加用例，使用记录所有 `write()` 调用的 fake transport：

```cpp
void SessionTests::userInputPreemptsPartialMcpWrite()
{
    SessionInputArbiter arbiter;
    RecordingTransport transport;
    arbiter.bind(&transport, 7);
    QVERIFY(arbiter.acquireMcpLease(41, 7));
    QVERIFY(arbiter.submitMcpInput(41, QByteArrayLiteral("echo par")));

    QSignalSpy preempted(&arbiter, &SessionInputArbiter::mcpPreempted);
    arbiter.submitUserInput(QByteArrayLiteral("x"));

    QCOMPARE(preempted.count(), 1);
    QCOMPARE(transport.writes(), QList<QByteArray>{
        QByteArrayLiteral("echo par"), QByteArrayLiteral("x")});
    QVERIFY(!arbiter.submitMcpInput(41, QByteArrayLiteral("tial\r")));
}
```

同时覆盖：旧 generation 不能获取 Lease、同 Session 第二个 Lease 被拒绝、空输入不改变状态。

- [ ] **Step 2: 运行测试确认失败**

Run: `ctest --test-dir build -C Debug -R novaterm_session_tests --output-on-failure`  
Expected: 编译失败，缺少 `SessionInputArbiter`。

- [ ] **Step 3: 实现最小输入仲裁器**

```cpp
enum class SessionInputOrigin { User, Mcp };

class SessionInputArbiter final : public QObject
{
    Q_OBJECT
public:
    void bind(ITransport* transport, quint64 generation);
    void reset(quint64 generation);
    [[nodiscard]] bool acquireMcpLease(quint64 executionId, quint64 generation);
    [[nodiscard]] bool submitMcpInput(quint64 executionId, const QByteArray& data);
    void submitUserInput(const QByteArray& data);
    void releaseMcpLease(quint64 executionId);
    [[nodiscard]] bool hasMcpLease() const noexcept;
signals:
    void mcpPreempted(quint64 executionId, bool executionMayHaveStarted);
private:
    QPointer<ITransport> _transport;
    quint64 _generation{0};
    quint64 _executionId{0};
    bool _mcpBytesWritten{false};
};
```

`submitUserInput()` 必须先发出抢占并清除 Lease，再调用一次 `transport->write(data)`；`TerminalSession` 的 Core 输出连接和 `writeUserInput()` 都改走该接口。

- [ ] **Step 4: 运行 Session 测试**

Run: `ctest --test-dir build -C Debug -R novaterm_session_tests --output-on-failure`  
Expected: PASS。

- [ ] **Step 5: 提交**

把版本从 `0.2.22` 升到 `0.2.23`，只暂存本任务文件与版本行：

```powershell
git add src/session/SessionInputArbiter.* src/session/TerminalSession.* tests/session/SessionTests.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat: 增加会话输入仲裁与用户抢占

测试: novaterm_session_tests"
```

### Task 2: 入站 framing、提示符状态与隐藏标记

**Files:**
- Create: `src/session/InteractiveCommandProfile.h`
- Create: `src/session/InteractiveCommandProfile.cpp`
- Create: `src/session/InteractiveStreamFramer.h`
- Create: `src/session/InteractiveStreamFramer.cpp`
- Modify: `src/session/SessionInputPump.h`
- Modify: `src/session/SessionInputPump.cpp`
- Modify: `src/session/TerminalSession.h`
- Modify: `src/session/TerminalSession.cpp`
- Modify: `tests/session/SessionTests.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Transport 入站 `QByteArray`、Session generation。
- Produces: `InteractiveStreamFramer::consume()`、`InteractiveStreamEvent`、去 marker 的 visible bytes 与 `promptGeneration`。

- [ ] **Step 1: 写 framing 分包、伪造与 UI 隐藏测试**

```cpp
void SessionTests::interactiveMarkersNeverReachTerminalCore()
{
    InteractiveStreamFramer framer;
    framer.reset(9, QByteArrayLiteral("nonce-1"));
    const auto first = framer.consume(QByteArrayLiteral("out\x1b]633;NT;END;non"));
    const auto second = framer.consume(QByteArrayLiteral("ce-1;0\x07prompt$ "));
    QCOMPARE(first.visibleBytes, QByteArrayLiteral("out"));
    QCOMPARE(second.visibleBytes, QByteArrayLiteral("prompt$ "));
    QCOMPARE(second.events.size(), 1);
    QCOMPARE(second.events.front().kind, InteractiveStreamEventKind::CommandFinished);
}
```

再增加旧 nonce、超长未闭合 OSC、普通 OSC 633、marker 跨 1 字节分片和 reset 后残片清空测试。

- [ ] **Step 2: 运行测试确认失败**

Run: `ctest --test-dir build -C Debug -R novaterm_session_tests --output-on-failure`  
Expected: 编译失败，缺少 Profile/Framer。

- [ ] **Step 3: 实现 Profile DTO 与有界 Framer**

```cpp
enum class InteractiveStreamEventKind {
    PromptReady, CommandStarted, CommandFinished, ShellReset
};

struct InteractiveStreamEvent {
    InteractiveStreamEventKind kind;
    quint64 promptGeneration{0};
    std::optional<int> exitCode;
};

struct InteractiveFrameResult {
    QByteArray visibleBytes;
    QVector<InteractiveStreamEvent> events;
};

class InteractiveStreamFramer final {
public:
    void configure(InteractiveCommandProfile profile);
    void reset(quint64 generation, QByteArray executionNonce = {});
    [[nodiscard]] InteractiveFrameResult consume(const QByteArray& bytes);
};
```

Framer 内部未完成 marker 缓冲上限 4 KiB，超限时把数据按普通可见字节释放并发布 framing 失配事件，禁止无界等待。

- [ ] **Step 4: 将 Framer 接入唯一入站通路**

`SessionInputPump` 构造增加 `InteractiveStreamFramer*`，`acceptBytes()` 先调用 `consume()`，只把 `visibleBytes` 分块喂给 `TerminalCore::writeInput()`，并发出：

```cpp
signals:
    void interactiveEvent(const InteractiveStreamEvent& event);
    void interactiveBytes(const QByteArray& bytes);
```

`interactiveBytes` 仅用于当前有 Lease 的 64 KiB 有界捕获，不建立历史缓存。

- [ ] **Step 5: 运行 Session 与 TerminalSession 测试**

Run: `ctest --test-dir build -C Debug -R "novaterm_(session|terminal_session)_tests" --output-on-failure`  
Expected: 两项目标 PASS。

- [ ] **Step 6: 提交**

版本升到 `0.2.24`，提交信息：

```text
feat: 增加交互流 framing 与提示符事件

测试: novaterm_session_tests, novaterm_terminal_session_tests
```

### Task 3: SessionCommandCoordinator 与交互执行器

**Files:**
- Create: `src/session/SessionCommandCoordinator.h`
- Create: `src/session/SessionCommandCoordinator.cpp`
- Create: `src/session/InteractiveSessionCommandExecutor.h`
- Create: `src/session/InteractiveSessionCommandExecutor.cpp`
- Modify: `src/session/CommandExecutionTypes.h`
- Modify: `src/session/SessionCommandFacade.h`
- Modify: `src/session/SessionCommandFacade.cpp`
- Modify: `src/session/SessionDirectory.cpp`
- Modify: `src/session/TerminalSession.h`
- Modify: `src/session/TerminalSession.cpp`
- Modify: `tests/session/SessionTests.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 Arbiter、Task 2 Framer/Profile、`CommandExecutionRequest`。
- Produces: `SessionCommandCoordinator::submit/cancel` 和统一 `CommandExecutionResult`。

- [ ] **Step 1: 写状态机、未就绪零字节和抢占结果测试**

用例必须覆盖：`Unavailable` 返回 false 且 transport writes 为空；收到 `PromptReady` 后可提交；收到匹配 END 后 completed；用户抢占在已写入后返回 `Cancelled` 且 `terminationConfirmed=false`。

- [ ] **Step 2: 运行测试确认失败**

Run: `ctest --test-dir build -C Debug -R novaterm_session_tests --output-on-failure`  
Expected: 缺少 Coordinator/Executor。

- [ ] **Step 3: 扩展通用请求并实现 Coordinator**

```cpp
struct CommandExecutionRequest {
    quint64 requestId{0};
    QByteArray command;
    CommandExecutionLimits limits;
    QString commandId;
    QByteArray executionNonce;
    quint64 expectedPromptGeneration{0};
};

class SessionCommandCoordinator final : public QObject
{
    Q_OBJECT
public:
    [[nodiscard]] bool submit(const CommandExecutionRequest& request);
    void cancel(quint64 requestId);
    void reset(quint64 sessionGeneration);
    [[nodiscard]] bool isPromptReady() const noexcept;
    [[nodiscard]] quint64 promptGeneration() const noexcept;
signals:
    void finished(const CommandExecutionResult& result);
};
```

Coordinator 的单次捕获上限使用 `CommandExecutionLimits::maxOutputBytes`；达到上限后停止捕获并进入 OutputLimit，不截断 UI 流。

- [ ] **Step 4: 用 Interactive Executor 适配现有 Facade**

`InteractiveSessionCommandExecutor` 实现 `ISessionCommandExecutor`，能力为：

```cpp
return {CommandExecutionMode::InteractiveFramed,
        profile.providesExitCode(),
        profile.providesTerminationEvidence(),
        false};
```

`SessionDirectory::commandExecutor()` 改为接收 `TerminalSession*`，只在 Session 当前 Profile 可用时安装交互 Executor；保留 isolated 类但不再作为 v0.6 最终路由。

- [ ] **Step 5: 运行 Session/MCP/TerminalSession 测试**

Run: `ctest --test-dir build -C Debug -R "novaterm_(session|mcp|terminal_session)_tests" --output-on-failure`  
Expected: 三项目标 PASS。

- [ ] **Step 6: 提交**

版本升到 `0.2.25`，提交信息：

```text
feat: 增加 Session 级交互命令协调器

测试: novaterm_session_tests, novaterm_mcp_tests, novaterm_terminal_session_tests
```

### Task 4: Shell integration 与各 Transport Profile

**Files:**
- Create: `src/session/ShellIntegration.h`
- Create: `src/session/ShellIntegration.cpp`
- Modify: `src/session/CommandPlatformProfile.h`
- Modify: `src/session/CommandPlatformProfile.cpp`
- Modify: `src/session/LocalShellProfile.h`
- Modify: `src/session/LocalShellProfile.cpp`
- Modify: `src/session/SessionFactory.cpp`
- Modify: `src/profile/ProfileStore.h`
- Modify: `src/profile/ProfileStore.cpp`
- Modify: `src/session/SessionTypes.h`
- Modify: `src/transport/SshTransport.cpp`
- Modify: `src/transport/SerialTransport.cpp`
- Modify: `src/transport/TelnetTransport.cpp`
- Modify: `tests/session/SessionTests.cpp`
- Modify: `tests/transport/TelnetTransportTests.cpp`
- Modify: `tests/transport/SshTransportFailureCheck.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 2 `InteractiveCommandProfile`。
- Produces: POSIX/PowerShell/CMD shell integration，以及配置驱动的 Serial/Telnet prompt profile。

- [ ] **Step 1: 写 Profile 能力与零探测测试**

断言未知 Shell、缺少 prompt rule、密码提示和备用屏不发布 `run_command`；Serial/Telnet 未配置时 transport write 计数保持 0。

- [ ] **Step 2: 运行专项测试确认失败**

Run: `ctest --test-dir build -C Debug -R "novaterm_(session|ssh_transport_check|telnet_transport)_tests" --output-on-failure`  
Expected: 新断言失败。

- [ ] **Step 3: 实现显式 Profile 选择**

`RuntimeConfig.transport` 使用以下稳定键，不从屏幕猜测：

```cpp
struct InteractiveProfileSettings {
    QString shellKind;       // posix, powershell, cmd, serial-prompt, telnet-prompt
    QString promptPattern;   // 仅 Serial/Telnet
    QByteArray lineEnding;   // \r, \n 或 \r\n
    bool remoteEcho{true};
};
```

POSIX/PowerShell/CMD integration 生成 OSC 633 NovaTerm 私有子类型和随机 nonce；marker 文本由 Framer 消费。未知 shellKind 返回 unavailable。

- [ ] **Step 4: 接入启动配置而不发送探测字符**

LocalShell 在进程启动环境/参数中安装 integration；SSH 仅在用户 Profile 明确选择 shellKind 时安装，不能运行 `echo $SHELL` 探测。Serial/Telnet 只消费配置规则，不发送探测输入。

- [ ] **Step 5: 运行专项测试**

Run: `ctest --test-dir build -C Debug -R "novaterm_(session|ssh_transport_check|telnet_transport|terminal_session)_tests" --output-on-failure`  
Expected: 四项目标 PASS。

- [ ] **Step 6: 提交**

版本升到 `0.2.26`，提交信息：

```text
feat: 增加可信 Shell 与设备提示符 Profile

测试: novaterm_session_tests, novaterm_ssh_transport_check, novaterm_telnet_transport_tests, novaterm_terminal_session_tests
```

### Task 5: 风险策略、自由命令工具与固定诊断迁移

**Files:**
- Create: `src/mcp/CommandRiskPolicy.h`
- Create: `src/mcp/CommandRiskPolicy.cpp`
- Modify: `src/mcp/CommandPolicy.h`
- Modify: `src/mcp/CommandPolicy.cpp`
- Modify: `src/mcp/McpProtocol.h`
- Modify: `src/mcp/McpProtocol.cpp`
- Modify: `src/mcp/McpAccess.h`
- Modify: `src/mcp/McpAccess.cpp`
- Modify: `src/mcp/McpService.cpp`
- Modify: `src/session/CommandPlatformProfile.cpp`
- Modify: `tests/mcp/McpTests.cpp`
- Modify: `src/mcp/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 3 Coordinator/Facade、Task 4 Profile。
- Produces: `CommandRiskPolicy::classify()`、`novaterm_run_command`、交互版固定诊断。

- [ ] **Step 1: 写风险分类和协议 schema 失败测试**

```cpp
QCOMPARE(policy.classify("uname -srm").decision, RiskDecision::Allow);
QCOMPARE(policy.classify("rm -rf ./cache").decision, RiskDecision::Confirm);
QCOMPARE(policy.classify("sudo systemctl stop auditd").decision, RiskDecision::Deny);
QCOMPARE(policy.classify("python -c 'dynamic()'").decision, RiskDecision::Confirm);
```

协议测试覆盖：命令空串、超过 16 KiB、未知字段、换 Session 票据、无交互授权。

- [ ] **Step 2: 运行 MCP 测试确认失败**

Run: `ctest --test-dir build -C Debug -R novaterm_mcp_tests --output-on-failure`  
Expected: 新工具不存在或风险分类断言失败。

- [ ] **Step 3: 实现风险 DTO 与保守分类**

```cpp
enum class RiskDecision { Allow, Confirm, Deny, Unknown };
struct RiskAssessment {
    RiskDecision decision{RiskDecision::Unknown};
    QStringList reasons;
    QString policyVersion;
};
class CommandRiskPolicy final {
public:
    [[nodiscard]] RiskAssessment classify(QStringView command) const;
    [[nodiscard]] RiskAssessment classifyScript(
        QByteArrayView content, QStringView targetPath,
        QStringView workingDirectory, QStringView invocation) const;
};
```

只把明确的只读单命令归为 Allow；复合语法、重定向、解释器、文件/系统变化归 Confirm；已知禁止族归 Deny；解析失败归 Unknown。

- [ ] **Step 4: 新增 run_command 并迁移固定命令**

`McpProtocol::tools()/validateArguments()` 增加 `novaterm_run_command`；`McpService` 完成授权、epoch、风险与 prompt 检查后提交 Facade。固定 POSIX 模板改为：

```cpp
return {{"system.identity", "System identity", "uname -srm"},
        {"system.uptime", "Uptime and load", "uptime"},
        {"memory.summary", "Memory summary", "free -k"},
        {"filesystem.usage", "Filesystem capacity", "df -Pk"}};
```

策略版本升级，旧票据返回 `COMMAND_POLICY_CHANGED`。

- [ ] **Step 5: 运行 MCP 与 Session 测试**

Run: `ctest --test-dir build -C Debug -R "novaterm_(mcp|session)_tests" --output-on-failure`  
Expected: 两项目标 PASS。

- [ ] **Step 6: 提交**

版本升到 `0.2.27`，提交信息：

```text
feat: 增加受控自由命令与风险分类

测试: novaterm_mcp_tests, novaterm_session_tests
```

### Task 6: MCP 人类 elicitation 与确认状态

**Files:**
- Create: `tools/novaterm-mcp/ElicitationBroker.h`
- Create: `tools/novaterm-mcp/ElicitationBroker.cpp`
- Modify: `tools/novaterm-mcp/main.cpp`
- Modify: `tools/novaterm-mcp/StdioChannel.h`
- Modify: `tools/novaterm-mcp/StdioChannel.cpp`
- Modify: `tools/novaterm-mcp/CMakeLists.txt`
- Modify: `src/mcp/McpProtocol.cpp`
- Modify: `src/mcp/McpService.cpp`
- Modify: `tests/mcp/McpTests.cpp`
- Modify: `tests/mcp/interop_check.py`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 5 `RiskAssessment` 与待确认执行描述。
- Produces: 2025 `elicitation/create`、2026 MRTR、单次确认状态验证。

- [ ] **Step 1: 写 accept/decline/cancel/缺能力/陈旧确认测试**

测试 Bridge 真实 stdio：客户端声明 `elicitation.form` 时危险命令先收到反向请求；accept 后继续；decline/cancel 零字节；未声明能力返回 `CLIENT_CONFIRMATION_UNAVAILABLE`。确认后改变 epoch、promptGeneration 或命令正文返回 `COMMAND_CONFIRMATION_STALE`。

- [ ] **Step 2: 运行 MCP 测试确认失败**

Run: `ctest --test-dir build -C Debug -R novaterm_mcp_tests --output-on-failure`  
Expected: Bridge 不识别反向响应或缺少确认状态。

- [ ] **Step 3: 实现 2025 ElicitationBroker**

```cpp
struct PendingElicitation {
    QString id;
    QJsonValue toolCallId;
    QString executionId;
    QByteArray payloadHash;
    qint64 expiresAtMs{0};
};

class ElicitationBroker final : public QObject
{
    Q_OBJECT
public:
    [[nodiscard]] bool clientSupportsForm() const noexcept;
    void requestConfirmation(PendingElicitation pending,
                             QString message,
                             std::function<void(bool accepted)> completion);
    [[nodiscard]] bool handleResponse(const QJsonObject& response);
};
```

反向 JSON-RPC ID 使用 `nt-elicit-<uuid>`，与 Host 的普通请求 ID 分域；最多一个在途确认/连接，60 秒过期，断连全部取消。

- [ ] **Step 4: 实现 2026 MRTR**

Bridge 对现代请求返回 `resultType=input_required`、boolean elicitation 和 MAC 保护的 `requestState`；重提时验证 `inputResponses`、payload hash、client identity、session/epoch、prompt/profile/policy 版本和单次使用记录。

- [ ] **Step 5: 运行 MCP 与官方 SDK 互操作测试**

Run: `ctest --test-dir build -C Debug -R novaterm_mcp_tests --output-on-failure`  
Run: `python tests/mcp/interop_check.py --bridge build/bin/Debug/novaterm-mcp.exe`  
Expected: 自动化 PASS；旧版五工具与新确认流程均通过。

- [ ] **Step 6: 提交**

版本升到 `0.2.28`，提交信息：

```text
feat: 增加 MCP 人类确认协议

测试: novaterm_mcp_tests, tests/mcp/interop_check.py
```

### Task 7: SSH/LocalShell 脚本文件能力

**Files:**
- Create: `src/session/ISessionScriptProvider.h`
- Create: `src/session/LocalSessionScriptProvider.h`
- Create: `src/session/LocalSessionScriptProvider.cpp`
- Create: `src/session/SshSessionScriptProvider.h`
- Create: `src/session/SshSessionScriptProvider.cpp`
- Modify: `src/session/SftpSession.h`
- Modify: `src/session/SftpSession.cpp`
- Modify: `src/session/TerminalSession.h`
- Modify: `src/session/TerminalSession.cpp`
- Modify: `src/mcp/McpProtocol.cpp`
- Modify: `src/mcp/McpService.cpp`
- Modify: `tests/session/SessionTests.cpp`
- Modify: `tests/mcp/McpTests.cpp`
- Modify: `tests/mcp/ssh_loopback_check.py`
- Modify: `src/mcp/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 5 风险策略、Task 6 人类确认、Task 3 交互命令提交。
- Produces: `ISessionScriptProvider::writeScript()` 与 `novaterm_run_script`。

- [ ] **Step 1: 写确认前不落盘和正文不进 UI 测试**

本地临时目录测试依次断言：未确认文件不存在；decline 后不存在；accept 后内容完全一致；覆盖时确认消息包含现有目标；写入失败后 transport writes 为空；执行时 UI 字节只含调用命令和输出，不含脚本正文。

- [ ] **Step 2: 运行 Session/MCP 测试确认失败**

Run: `ctest --test-dir build -C Debug -R "novaterm_(session|mcp)_tests" --output-on-failure`  
Expected: 缺少 ScriptProvider/run_script。

- [ ] **Step 3: 实现通用 ScriptProvider 和 Local 写入**

```cpp
struct ScriptWriteRequest {
    quint64 requestId{0};
    QByteArray content;
    QString targetPath;
};
struct ScriptWriteResult {
    quint64 requestId{0};
    bool success{false};
    QString errorCode;
};
class ISessionScriptProvider : public QObject {
    Q_OBJECT
public:
    [[nodiscard]] virtual bool isAvailable() const = 0;
    virtual bool writeScript(const ScriptWriteRequest& request) = 0;
signals:
    void finished(const ScriptWriteResult& result);
};
```

Local provider 使用目标精确路径；不创建 NovaTerm 专用目录，不自动删除文件，不把正文写日志。

- [ ] **Step 4: 为 SFTP 增加有界 uploadBytes 并实现 SSH Provider**

`SftpSession` 增加：

```cpp
void uploadBytes(quint64 requestId, QByteArray content,
                 const QString& remotePath);
void cancelUpload(quint64 requestId);
```

队列持有内容的总预算不超过 2 MiB；连接身份必须与当前 Session 的主机密钥、端点和账号指纹一致。测试只使用回环 SSH，不读取用户历史凭据。

- [ ] **Step 5: 新增 run_script 编排**

`McpService` 固定顺序：授权 → 脚本风险 → elicitation → 再验 epoch/prompt → 写文件 → 再验 prompt → 通过 Coordinator 注入调用命令。任何一步失败都不得提交后续步骤。

- [ ] **Step 6: 运行脚本与回环测试**

Run: `ctest --test-dir build -C Debug -R "novaterm_(session|mcp|ssh_transport_check)_tests" --output-on-failure`  
Run: `python tests/mcp/ssh_loopback_check.py --bridge build/bin/Debug/novaterm-mcp.exe`  
Expected: 全部 PASS，且只连接回环 fixture。

- [ ] **Step 7: 提交**

版本升到 `0.2.29`，提交信息：

```text
feat: 增加目标主机脚本生成与交互执行

测试: novaterm_session_tests, novaterm_mcp_tests, novaterm_ssh_transport_check, ssh_loopback_check.py
```

### Task 8: 产品授权、文档同步与完整验收

**Files:**
- Modify: `src/mcp/McpAccess.h`
- Modify: `src/mcp/McpAccess.cpp`
- Modify: `src/ui/widgets/McpSettingsDialog.h`
- Modify: `src/ui/widgets/McpSettingsDialog.cpp`
- Modify: `tests/mcp/McpTests.cpp`
- Modify: `tests/ui/SystemInformationDialogLayoutTests.cpp`
- Modify: `docs/ARCHITECTURE.md`
- Modify: `docs/architecture/stages/P6_Session_and_Transport.md`
- Modify: `docs/architecture/stages/P8_AI_MCP_Interface.md`
- Modify: `docs/MCP_Usage.md`
- Modify: `AGENTS.md`
- Modify: `resources/translations/novaterm_zh_CN.ts`
- Modify: `resources/translations/novaterm_en.ts`
- Modify: `tests/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 1–7 的完整能力。
- Produces: 默认关闭的三项授权、可审计状态、同步文档和发布验收证据。

- [ ] **Step 1: 写授权隔离与 UI 测试**

测试读取授权不产生 `run_command`；交互命令、需确认命令、脚本权限独立；撤销任一权限使对应确认和在途工作失效；设置界面能分别保存和恢复三项授权。

- [ ] **Step 2: 运行 MCP/UI 测试确认失败**

Run: `ctest --test-dir build -C Debug -R "novaterm_(mcp|ui_dialog_layout)_tests" --output-on-failure`  
Expected: 新授权或控件断言失败。

- [ ] **Step 3: 实现授权和 UI**

在 `AccessStore` 中分别保存 `interactiveCommand`、`confirmedCommand`、`scriptTask`；关闭总开关或撤销授权时先清内存访问，再取消确认/Lease。UI 文案明确“命令显示在当前终端”“危险命令由 MCP 客户端确认”“风险检测不是沙箱”。

- [ ] **Step 4: 更新翻译与文档**

Run: `cmake --build build --target update_translations`  
回填所有新增翻译，确认无 `type="unfinished"`。同步唯一数据通路、线程/所有权、P6 禁止项、P8 §14 实施事实、MCP 使用方法和 AGENTS 踩坑记录；未完成的平台验收保持保守状态。

- [ ] **Step 5: 运行专项测试**

在 PowerShell 中设置 Qt 路径后运行：

```powershell
$env:PATH = "C:\Programs\Qt\6.8.3\msvc2022_64\bin;$env:PATH"
$env:QT_PLUGIN_PATH = "C:\Programs\Qt\6.8.3\msvc2022_64\plugins"
ctest --test-dir build -C Debug -R "novaterm_(core|session|mcp|renderer|renderer_p5|ssh_transport_check|telnet_transport|terminal_session|ui_dialog_layout)_tests" --output-on-failure
```

Expected: 除 AGENTS.md 已登记的环境/平台问题外无新增失败。

- [ ] **Step 6: 运行完整测试和回环验收**

Run: `ctest --test-dir build -C Debug --output-on-failure`  
Run: `python tests/mcp/interop_check.py --bridge build/bin/Debug/novaterm-mcp.exe`  
Run: `python tests/mcp/ssh_loopback_check.py --bridge build/bin/Debug/novaterm-mcp.exe`  
Expected: 全套无新增失败；回环不访问用户服务器；已知 ConPTY 偶发/句柄泄漏按 AGENTS.md 如实记录。

- [ ] **Step 7: 人工 UI 与性能验收**

在 Windows 验证 PowerShell/CMD，在 Linux/macOS 验证支持的 PTY Shell：

1. 低风险命令无需确认且命令/输出显示；
2. 危险命令弹出 MCP 客户端确认，拒绝时终端零变化；
3. 脚本正文不显示，调用和输出显示；
4. 用户输入立即抢占；
5. TUI、密码提示和未知 prompt 拒绝；
6. 命令期间 Parser 吞吐、GUI frame P95 和队列水位不突破 §9 预算。

将日期、平台、构建类型和结果写入 P8 §14；未执行项明确标为未验收。

- [ ] **Step 8: 最终提交**

版本升到 `0.2.30`，提交信息：

```text
feat: 完成交互式 MCP 终端操作接入

测试: novaterm 相关专项与全套 ctest；MCP SDK 互操作；SSH 回环
验收: Windows PowerShell/CMD；Linux/macOS 与性能结果见 P8
```
