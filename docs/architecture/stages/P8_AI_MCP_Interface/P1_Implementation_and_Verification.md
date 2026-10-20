# mcp/P1 实施与验收记录

实施日期：2026-10-18～19。计划：[P1](P1_Capability_Runtime.md)；约束：[总体架构](00_Overall_Architecture.md)。

## 交付范围

已建立 `src/capabilities/` 的目录、可信入口、资源授权、准入租约、领域端口、环境路由和诊断任务所有者。
当前只发布原九工具，目录版本为 **1**。内部能力 ID 与 MCP 工具名、RPC request ID 分开。
`ToolCodec` 继续独占 JSON Schema/解码/编码；目录绑定来自 Registry，原 fixture 按对象比较保持不变。
按目标的可用性仍由既有 CapabilityResolver 和新 Recipe Router 计算，不改变静态工具目录。

本机 `LocalAgentAdapter` 是供应用组合根调用的类型化入口，开关、principal 和授权 lookup 独立于外部 MCP。
它与 MCP 适配器消费相同领域端口；P6 的模型 Driver、聊天 UI、批准卡片不在本次交付内。
当前 Service 装配的是外部 MCP 身份与既有 AccessStore；未来本机组合根须提供自己的可信身份/资源授权，
不能将模型提供的 clientId 转为 principal，不能把外部 MCP 开关当作本机 Agent 总开关。

Recipe 路由与真实 Session 适配已实现，但本次没有注册 P4/P5 Recipe 工具，也没有新增诊断包、
真实设备探测、自动连接或部署。Recipe 授权端口与共享终端占用同意端口均须由组合根明确注入，
缺失时拒绝；不借用既有 FullTerminalInput 或旧固定诊断选择。

## 直接依赖与状态所有者

| 组件/目标 | 直接依赖和消费者 | 唯一 owner / 边界 |
| --- | --- | --- |
| `novaterm_capabilities` | Qt Core；由协议、领域端口和 Session 适配消费 | Registry 只管理已实现元数据；Authorization 拥有入口 seal；Dispatcher 仅拥有有界操作/取消索引 |
| `TypedCapabilityPort<Request, Response>` | 轻量契约与 Dispatcher | 必须先准入，副作用前/结果交付前再验；不建立全系统请求 variant 或 JSON 总线 |
| `novaterm_terminal_capabilities` | capabilities、mcp application/contracts、Qt Core | Read/Search/Observation/Input/Command 各用自身领域值；不链接 HTTP、Widgets、具体 Session |
| `ReadCapabilities` / `SearchCapabilities` | ReadSessionPort、授权 capture lookup、有界 runner | 复用 ReadUseCases / CaptureSearch；capture 的 scope/TTL 由来源适配器验证 |
| `ObservationCapabilities` / `InputCapabilities` | ObservationState、TerminalInputPort | 等待/重放仍由唯一 ObservationState 拥有；端口只保留租约与交付，不复制 replay/wait 规则 |
| `CommandCapabilities` | 唯一 CommandRuntime | 保留票据、elicitation、MRTR、发布/调用、unknown 与 target protection；延迟提交保留租约复核函数 |
| `novaterm_session_environment` | capabilities、Qt Core；由 Session 实现直接链接 | EnvironmentTracker 位于 Session 集成层，SessionDirectory 更新完整绑定；novaterm_core 不依赖它 |
| `SessionRecipeAdapter`（集成目标） | SessionDirectory、Facade/Coordinator/Arbiter、TargetGuard、窄授权/同意查询 | 独立执行走现有 Facade；共享执行走 Coordinator；不直接写 Transport |
| `DiagnosticTaskManager` | RecipeSessionPort、InvocationLease、单调时钟 | 应用级唯一任务 owner，32 全局/4 每 Principal、最多 64 步；执行结果与取消/交付状态分开 |

所有新应用组件在其所属应用线程调用；异步搜索只向 worker 传不可变 capture/请求和取消标记，
通过有界线程池与受保护邮箱回到应用线程。诊断任务内部的短期 shared ownership 只用于同步回调栈保活，
不向其他模块共享可变 Job；关闭立即撤销交付、取消在途操作，并保留未知目标保护。

新增库的源码已从主程序的递归 glob 排除，避免同一生产实现重复编译。
`novaterm_capabilities_tests` 的最终链接仅含 capabilities、Qt Core/Test；
`novaterm_capability_entry_tests` 仅含纯应用库和 Qt Core/Test。
新增 fake 日志分析端口无需修改 Dispatcher、终端领域请求或既有 UI 分支。

## 安全与兼容语义

- `InvocationContext` 的身份带入口私有 seal；改写 principal、使用其他 authority、关闭入口、授权版本/资源绑定变化均拒绝。
  MCP 在入队前保存当前绑定/授权版本，出队后重新验证。操作索引上限 128，租约 move-only，完成/取消幂等。
- 命令确认后、脚本发布前/发布后调用前均复核租约；停用本机入口不能借尚未返回的批准或上传完成继续提交。
  结果交付重验撤权，不返回已撤销正文。现有完整输入授权仍免逐条确认。
- 输入仍走唯一 Controller/Arbiter；本机与 MCP 共享动作验证、64 动作/16 KiB 原文预算和 replay 身份。
  取得控制权后再次校验授权。基础 text/key/paste 不要求 Shell；command 快捷动作仍检查就绪。
- TransportKind 不推导 TargetEnvironment。新连接/重连、人工接管、reset、身份变化、登录/REPL/TUI/文件传输边界使环境证据失效。
  普通输出只撤销矛盾证据，不建立 Linux 就绪、不续租、不发送探测。
- 环境/就绪证据最多 5 秒。无配对适配器时，本机确认绑定 principal、Recipe、计划摘要及当前连接，最多消费一次。
  配对适配器报告也必须有独立诊断授权和可见的共享终端占用同意；旧消息序号/旧连接拒绝。
- Recipe 明确选择独立或共享路线，失败不换路重试。共享请求设置唯一 nonce，并遵守 Coordinator 的 prompt-free/prompt-generation 契约；
  本机固定 helper 保留 commandId。unknown/取消不声称远端已停止，不清除隔离标记。

**限制**：同一连接上没有可观测信号的远端环境切换不能可靠检测；5 秒证据不是绝对保证。
P4/P5/P6 的产品展示、实际配对适配器和本机确认 UI 必须保留此限制。P1 不把现行 Shell 标记升级为可信认证身份。

## 验收映射

| 计划范围 | 实际测试证据 |
| --- | --- |
| T1 目录、重复/未实现拒绝、fixture 兼容 | CapabilityRuntimeTests：catalogHasStableIds、unimplementedToolsNotRegistered；ContractsTests：catalogMatchesCompatibilityFixture、catalogExposesNineToolsInOrder、参数接受/拒绝；wire |
| T2 可信身份、排队/交付撤权、绑定分离、有界准入 | trustedAdmissionAndRevocation、cancellationAndResourceSeparation、revocationDeadlineAndBoundedAdmission、twoUnrelatedTypedPortsShareAdmission |
| T2 两入口投影/搜索/观察/输入/执行一致 | EntryParityTests 全部；CommandRuntimeTests：typedExecutionPortsShareRuntimeAndRejectRevocation、disablingLocalEntryBlocksDeferredCommandSubmission（confirmation / publication）、commandPortDestructionCancelsDeferredSubmission |
| T3 Unknown/环境变化、期限、单次确认、独立权限/占用同意 | EnvironmentRoutingTests 全部；包含五种 Transport fake 矩阵、旧适配器序号拒绝与 pairedReadinessStillRequiresVisibleOccupancyConsent |
| T3 任务预算、取消、单次完成、失败不换路、析构 | DiagnosticTaskTests 全部；包含同步完成/取消回调销毁 owner、迟到回调不交付 |
| T3 真实 Session 接线 | McpTests：recipeAdapterUsesExistingSessionAndInvalidatesOnInput、recipeAdapterSubmitsSharedAndIsolatedRequests；既有五类连接输入矩阵 |
| T4 纯边界/扩展与既有行为 | 新四目标独立链接；MCP 标签全部目标；根完整构建和全套回归 |

复用已覆盖的测试名称，不为了与计划中的示意名称一致再写同义测试。

## 本轮验证

平台：Linux x86_64，Qt 6.8.3，GCC 13，Debug；构建目录 `build`。
本次新增四个 CTest 目标，当前注册 **36** 项、MCP 标签 **16** 项。

- 完整构建：`cmake --build build -j 4` 通过。
- 全套回归：**36/36** 通过，184.45 秒。
- 全套之后仅补命令端口析构收口：完整重建通过，受影响的 MCP 标签 **16/16** 再验通过，30.23 秒；
  未重复运行其余 20 项（源码未变）。
- ASan/UBSan：五个纯应用目标 **5/5** 通过（capabilities、routes、diagnostic tasks、entry、command runtime）。
  使用 `ASAN_OPTIONS=detect_leaks=0`，不声称 LeakSanitizer 通过。
- `git diff --check` 通过。独立链接命令已核对无 HttpServer/Widgets/具体 Session；核心无反向依赖。

测试中的本机 HTTP/PTY 在沙箱内受限；本轮完整回归使用沙箱外隔离测试环境。
不连接用户服务器、不连接真实 UART。Windows、macOS、真实串口和 P4/P6 产品端到端均未验收。

独立审查指出的延迟提交授权缺口、Recipe 请求映射缺口、同步回调 owner 析构风险均补了回归；作者结构复核另补命令端口析构后不得继续提交的回归。
ASan 另外检出跨 chunk 环境候选识别清理自身缓冲后继续读取 view 的问题，已修复并复验。
审查会话受额度限制未完成最终复审；后续修复由针对性测试、消毒器与作者结构复核验证。

复测命令：

```bash
cmake --build build -j 4
ctest --test-dir build -L mcp --output-on-failure
ctest --test-dir build --output-on-failure
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build/p1-asan \
  -R 'novaterm_(capabilities|capability_routes|diagnostic_tasks|capability_entry|mcp_command_runtime)_tests' \
  --output-on-failure
```

提交策略遵循阶段文档：修改保留在当前工作树，不自动提交、推送或创建额外分支/worktree。

## Release 构建补验（2026-10-19）

此前本轮完整构建记录只覆盖 Debug。GCC 13.3.0 在 Release 的 `-O3 -Werror` 下，
对 EnvironmentTracker::invalidate 中“复制 SessionBinding → 清空快照 → 移回绑定”的临时对象
报 `optional<QString>` 内部指针 `maybe-uninitialized`，已在 `build/Release` 稳定复现。
改为值初始化新的 EnvironmentSnapshot，先复制绑定并推进 revision，再整体替换旧快照；
保持全部过期就绪/确认状态清空，未修改任何警告或优化选项。

新增 `invalidationPreservesConnectionBinding` 两行数据（有/无目标指纹），验证完整连接绑定保留、
revision 递增、环境退回 Unknown、就绪/同意/期限全部清空以及重复失效。

本次局部修复的实际验证：

- `cmake --build build/Release --config Release --target all -- -j4`：通过。
- `ctest --test-dir build/Release -C Release -L capabilities --output-on-failure`：**4/4** 通过。
- Debug 环境路由专项：**1/1** 通过。
- ASan/UBSan 环境路由专项：**1/1** 通过，`detect_leaks=0`。

本次未重跑 Release 全套或其他平台测试，不把前述 Debug 全套成绩计作 Release 全套成绩。
本机日志位于 `build/p1-verification/release-all-build.log`、`release-capabilities-ctest.log`；
失败复现日志为同目录的 `release-invalidate-red.log`、`release-routes-red.log`。

## Qt C++ 审查修复（2026-10-20，0.2.72）

提交前按 qt-cpp-review 完成确定性 lint 和六领域只读分析，确认四项行为缺陷。
本轮均先复现失败，再做局部修复；不将机械风格提示批量改写为代码。

| 审查项 | 修复与回归 |
| --- | --- |
| D-001 取消命令后准入名额泄漏 | CommandCapabilities 在取消、断连、停止时先移除本地 active/完成租约，再取消 Runtime；适配器同步清理调用上下文。`cancelledCapabilityCommandsReturnAdmissionBudget` 覆盖 129 次取消、按 peer 断连和停止；远端未知记录/隔离标记不随本地名额回收而清除 |
| D-002 多段分片漏检环境边界 | 短 chunk 保留累计流尾部，长 chunk 保留其最后 128 字节；失效后不保留旧 overlap。`fragmentedRevocationMarkers` 逐字节覆盖 U-Boot、login、Password、REPL 与 alternate-screen 标记 |
| D-003 未知/取消/超时终态可交付已撤权正文 | 新增独立于交付生命周期的 `InvocationLease::checkAuthorization()`；诊断所有终态再验通用与 Recipe 权限，拒绝时清空正文并保留执行/终止事实。`revokedOutputSuppressedOnEveryTerminalPath` 覆盖 Recipe 撤权下未知完成/取消，以及通用撤权下 dispatcher 取消/到期 |
| D-004 执行超时覆盖领域超时结果 | 命令租约保留 5 秒 WireDeadlineMargin，Service 的执行期限仍由 Runtime 管理。真实适配器回归 `commandTimeoutKeepsExecutionFacts` 保留 COMMAND_TIMEOUT、executionId、executionMayHaveStarted、terminationConfirmed 与 quarantine |
| I-001 sweep 重入（复现后修复） | 授权回调取消当前任务或销毁 facade 时，原迭代器路径被 ASan 证实 UAF。改为持有 key/共享任务快照，并在授权与时钟回调后复核任务身份；`sweepToleratesAuthorizationReentry` 两行数据均通过 |

原审查涉及上述问题的三个领域已对修复做只读复核，未发现这些修复中的剩余高置信度缺陷。
I-002（Recipe steps 与 planDigest 的规范生成责任）仍按 P4 Provider 接入契约核验；当前调用由可信 host 构造，
没有公开 Recipe 工具，本轮不将该待核查项宣称为已证实或已修复的产品漏洞。

本次修复仅调整能力运行期/环境识别与 MCP 接线，未修改终端核心接口、Qt/第三方版本或编译选项。
按 AGENTS.md 跑相关专项，不重复跑此前已通过的无关全套；前文 36/36 仍是对应版本的历史成绩。
版本按提交约定由 0.2.71 递增为 0.2.72。

0.2.72 最终验证：

- Debug 完整构建通过，MCP 标签 **16/16**（30.74 秒）。
- Release 完整构建通过，MCP 标签 **16/16**（31.99 秒）。
- ASan/UBSan 纯应用五目标 **5/5**（0.13 秒），`ASAN_OPTIONS=detect_leaks=0`，不包含泄漏检测。
- `git diff --check` 通过。用户要求的 Git 提交包含 P1 实施和上述修复；独立的 `commit_check.md` 用户改动不纳入。

本机最终日志为 `build/p1-verification/review-fixes-0.2.72-{debug,release}-build.log`、
`review-fixes-0.2.72-{debug,release}-mcp.log` 和 `review-fixes-0.2.72-asan.log`。
Windows/macOS、真实 UART、P4/P5/P6 产品集成及 Release 全套仍不在本轮验证范围。
