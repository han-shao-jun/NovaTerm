# 实施、审查与验收记录

整理日期：2026-10-18。当前契约：[02](02_Current_MCP_Contract.md)；结构/后续计划：[总体架构](00_Overall_Architecture.md)、[P0](P0_Runtime_Consolidation.md)、[P8验收](P8_Integration_and_Performance.md)；原设计：[历史档案](04_Historical_Design_Archive.md)。

## 1. 记录规则与最新边界

本文件整合原P8 §14和2026-10-12之后的全部实施/审查/修复/验证记录。保留原日期、平台、命令、耗时、失败、限制和阶段编号；同日追加按原顺序。
这是历史证据，不是本次重新执行结果，也不保证未明确覆盖的平台、SDK或设备已通过。旧记录里的“待做”可能已被后续W5g～W5l完成；必须沿时间读，不从前一条覆盖后一条。

| 时间/阶段 | 可引用的事实与限制 |
| --- | --- |
| 2026-09-17～19 | 原只读/固定诊断、五工具及stdio/IPC历史基线，不证明HTTP入口 |
| 2026-09-26～28 | v0.6当前终端命令/脚本、确认及读取可用性优化；有效响应率94.2%，99%目标未达，GUI P95口径未闭合 |
| 2026-10-12～14 | HTTP-only迁移、绑定应用退出、协议/编码修复与当时平台记录；不推广为当前全平台验证 |
| 2026-10-15～17 | Linux专项/客户端只读、串口接线、输入/观察闭环、提权UI与安全回归；九工具不等于可信提权已实现 |
| 2026-10-18 W1～W5l | contracts/授权/统一观察/任务/证据/typed用例迁移及删除记录；最新W5l为Linux MCP九项9/9、28.36秒，全套27/29、190.26秒 |
| 2026-10-18 串口空闲收尾 | 串口空提示符降级为静默完成（无退出码、完成未验证），显式提示符仍为验证路径；Session/MCP/面板回归见文件末尾对应小节 |
| 当前仍未验收 | Windows/macOS重构后范围、真实UART/SSH/Shell/TUI、可信认证适配器、Windows文件竞争；按每条记录精确界定 |

Linux全套两项openpty环境失败分别涉及novaterm_session_tests串口PTY与novaterm_pty_tests。不得把27/29改写为全绿，亦不得用当时Linux成绩代表Windows/macOS。
原“最终结构验收”是当时用例迁移结果；当前低耦合/纯应用依赖/任务所有权差距仍由P0/P1审计，文档归档不自动完成这些目标。

## 2. 与阶段计划的对应

| 原内容 | 现阶段归属 |
| --- | --- |
| 初期接入、HTTP-only、九工具、W1～W5 | P0内核基线，P1入口/依赖收敛 |
| v0.4性能优化路线、成功率/Busy/GUI口径 | P8性能与容量任务；旧路线原文在04 |
| 终端输入/提示符/用户抢占/脚本/确认/unknown | 02接口契约及P0/P1行为/结构闸门 |
| Capture/全文日志/诊断设想 | P2～P5规划，不能从历史目标推定实现 |
| 模型/Agent/UI | P6/P7规划，历史MCP服务并不等于内置Agent |

下方原文保留原编号；原§1～§13和§15移至04。相对链接已按新位置修正，正文/代码/结果不作语义改写。
本文件承接原第1460～1788行和第2089～3253行，与04分区覆盖原文件全部行元素，无丢失分区。

## 3. 原§14：初期实施与v0.6记录

### 14. 首期实施记录（2026-09-17～19）

本节保留 2026-09-17～19 五工具及 stdio/IPC 的历史代码事实；该传输入口已删除。
当前 HTTP-only 入口以 §4 与文末迁移记录为准。前文
v0.5 的全会话命令执行是下一阶段设计目标，在实现与测试完成前不得写成已支持。

**当前生产固定诊断覆盖 SSH Session 与 Windows/Linux LocalShell：SSH 使用独立
exec channel，本地会话使用固定 `novaterm-local-diag` helper 子进程。
macOS LocalShell、Serial、Telnet、Custom 的固定诊断 Executor 仍未实现。**

v0.3/v0.4 的协议/性能新增项也继续以代码和测试为准；设计文档中的候选优化不能倒写成
实施事实。

| 步骤 | 当前代码 | 实施内容 |
| --- | --- | --- |
| P8.1 | `TerminalCore::tryTerminalState/tryModelRevision`、`TerminalContextProvider::trySnapshot` | 模型锁忙时立即失败；基础摘要与客户端进度分离，保留 cacheFloor/resetRevision |
| P8.1 | `src/session/SessionDirectory.*` | 弱引用目录、连接 epoch、Transport 附加身份、授权过滤后的有界分页；View 仍拥有 Session |
| P8.2 | `src/mcp/McpProtocol.*`、`LocalMcpServer.*` | 五工具 schema、参数校验、MAC、当前用户 IPC、帧/队列限制和合并唤醒 |
| P8.2 | `tools/novaterm-mcp/` | console stdio 桥接、初始化、取消、stdout 隔离、慢写退出、明确实例绑定 |
| P8.3 | `src/mcp/McpService.*` | 会话列表、上下文、capture 搜索、预算裁切和每客户端 token/capture 隔离 |
| P8.4 | `CommandPolicy.*`、`CommandPlatformProfile.*`、`CommandExecutionTypes.h`、`SessionCommandFacade.*`、`ISessionCommandExecutor.h`、`SshSessionCommandExecutor.*`、`LocalSessionCommandExecutor.*`、`tools/novaterm-local-diag/`、`McpService.*` | 固定诊断模板、通用执行 DTO、Session 级 Executor 路由、SSH 与 Windows/Linux LocalShell Isolated 适配、执行票据/去重、持久目标保护标记及结构化完成证据 |
| P8.5 | `McpAccess.*`、`McpSettingsDialog.*`、Application/MainWindow/TerminalPage 接线 | 总开关、客户端令牌、读取与逐项诊断授权、复制配置、执行记录和人工解除保护 |

当前实现中 RequestBroker/SessionReadFacade 的职责主要由 `McpService::Impl` 与 Session
显式读取入口组合承担。命令侧已经具备通用 `SessionCommandFacade`、
`ISessionCommandExecutor` 和第一个 SSH Isolated Executor；Facade 用 Session generation
与绑定序号丢弃换绑后的迟到完成结果，并在换绑或销毁前取消已登记的在途请求；
MCP 的执行、取消和完成映射不再持有 SSH 类型。
Windows/Linux LocalShell 分别由 `windows-local-v1` / `linux-local-v1` Profile
注册同一组语义 commandId。Executor 只启动应用目录中的固定 helper，以独立
stdout/stderr、退出码和进程终止证据返回结果；不向当前 PTY/ConPTY 写入，
也不继承交互 shell 的 cwd/alias/history。
当前 SSH 辅助 exec 输出不回灌交互终端；Serial/Telnet InteractiveFramed 仍只是设计，
因此当前代码也尚未产生第二条共享交互命令写入路径。

实施细节与边界：

- `TerminalStateCache` 当前以 `std::deque<Entry>` 保存稳定节点，并在淘汰
  `front()` 前从借用 `string_view` 的去重索引中删除对应 key；对象不可复制。
  这比仅依赖“不可复制”更完整地满足当前 borrowed-view 生命周期约束。仍应保留
  容器增长、淘汰、重复插入的回归测试；快照、JSON 和索引元数据另占有界空间。
- 命令返回保留 stdout/stderr 合计最多 64 KiB 原始内容；在边界最多多读一个探测
  字节来判断是否超限。转换后的 UTF-8 文本另限 64 KiB，不会因替代字符膨胀而失控。
- SSH 增加 `executeBoundedCommand` / `boundedCommandFinished`，保留旧的通用命令
  API 供 P7 使用。对未知是否已开始/结束的情况保持保守，不把 close/TERM 请求当成退出证明。
- Windows/Linux LocalShell 的 `novaterm-local-diag` 只接受四个固定 commandId，
  分别使用 Qt/Windows API 与 Qt/Linux `/proc` 读取采集，拒绝额外参数与未知命令；
  `LocalSessionCommandExecutor` 清理环境、合计限制 64 KiB 输出并在超时、取消、
  换绑或销毁时终止 helper。macOS 尚不安装 Local Executor；任何平台都不能
  回退到 `writeUserInput()` 或 shell wrapper。
- 客户端取消、撤权或 Session epoch 失效时，服务端先给 Executor 250 ms 发布更强的
  完成证据；若仍无结果，则把 Execution 完成为保守的 cancelled/unknown，保留目标
  quarantine 但允许用户核对后解除，避免执行记录永久停在 running 并耗尽 128 条上限。
- Core 已加入按真实读取需求、最多 4 Hz 合并的不可变 `PublishedTerminalState` 候选路径；
  Parser 在稳定提交点发布 shared snapshot，Provider 保留真实 capturedAt 且不再重复消费
  旧 history line。**本段记录 2026-09-22 当时的候选状态**；该路径已于 2026-09-28
  完成闸门并转为默认取数路径，`NOVATERM_MCP_PUBLISHED_SNAPSHOT` 语义已反转为
  「未设置即启用、显式置 0 才退回 try-read」。现状与新数据见 §14.2「2026-09-28 收口」。

2026-09-22 候选路径过载 A/B（四客户端各 10 Hz、64 MiB、三轮）显示吞吐中位数
`27.93 → 27.99 MiB/s`（无下降），capture P95 为 0.807～0.848 ms；成功读取分别
75/78、75/76、78/79，Busy 为 3/1/1。该结果显著改善旧路径的 read starvation，
但过载成功率仍非每轮 ≥99%，且正常 4 Hz 与 GUI frame P95 尚未补齐，因此继续保持
测试开关，不写成默认性能验收通过。

2026-09-22 正常设计负载 A/B（四客户端各 1 Hz、256 MiB、三轮）在授权时预热首份
快照后达到 36/36、36/36、36/36 成功读取，Busy 为 0；成功 RPC P95 为
45.10～48.77 ms，capture P95 为 0.986～1.011 ms，吞吐中位数
`29.01 → 29.08 MiB/s`（无下降）。每轮仅 8 次 Core publish 服务 40 次请求，
coalescedReadCount=27；snapshotAge P95 为 1.78～2.08 s。正常负载功能与吞吐门槛
已通过。当时真实 GUI frame P95 仍无设备级证据，因此候选路径保持显式测试开关；该缺口已于 2026-09-28 以 CPU 帧时间口径补齐，见 §14.2。
- 基础摘要缓存与增量筛选不共享客户端进度。截断时 nextToken 为空；语义不是完整日志。
- 多实例的接入配置通过版本校验与文件监听同步；令牌存取仅在用户管理客户端时
  触碰凭据库，协议认证只核对摘要。总开关关闭时清除本次授权，即使保存失败也先撤销内存访问。
- Windows 加固状态目录时，属主校验先算本进程令牌的对象属主身份集合（TokenUser、
  TokenOwner 与带 `SE_GROUP_OWNER` 的组），再写入“仅当前用户完全访问”的受保护
  DACL。提权运行创建的对象属主是 Administrators，只比对 TokenUser 会让
  `AccessStore::save()` 恒失败、`McpSettingsDialog` 的总开关表现为点不动。
  回归：`novaterm_mcp_tests::stateDirectoryOwnedByTokenDefaultOwnerIsSecurable`。
- 2026-10-15 补齐提权/非提权切换：UAC 过滤令牌的 Administrators 组没有
  `SE_GROUP_OWNER`，但历史端点目录可能仍以该组为属主，导致 HTTP 服务启动前报
  `Cannot secure HTTP endpoint directory`，两种配置复制都返回空。现在仅对令牌中
  的 Administrators 增加历史属主迁移，成功时用 Windows 的 `WRITE_OWNER` /
  `WRITE_DAC` 检查把属主规范化为 TokenUser，并写入仅当前用户完全访问的受保护
  DACL；其他非属主组仍拒绝。回归覆盖目录/文件迁移及 DACL、拒绝 Everyone 属主，
  以及设置页按钮实际写入 CC Switch 单 server JSON。
- 单实例导出配置默认自动发现，多实例导出固定当前 instanceId。旧 IPC 的执行票据
  不能在新连接重放；客户端断开后旧执行记录由本机 GUI 核对。
- 工具支持的固定程序缺失时返回命令失败及退出码，不自动搜索 PATH 或回退解释器。
- stdio 断连时桥接进程可能只能返回 COMMAND_OUTCOME_UNKNOWN，没有可信的完整执行
  DTO 时不伪造 details；有结构化完成证据的服务端错误保留完整执行详情。

验证入口均已加入仓库：

- `novaterm_mcp_tests`：实际子进程 stdio/IPC、授权、跨客户端、UTF-8、截断、搜索、
  取消、票据去重、持久保护、缓存淘汰、界面授权，以及 Windows 本地 helper 的固定
  参数拒绝、独立输出、非零退出、输出上限、超时、取消和 PTY 零写入。
- `tests/mcp/interop_check.py`：官方 MCP Python SDK 1.30.0、全部输出 schema、两个
  独立客户端和执行重试。依赖仅在测试时装入 build/mcp-test-deps。
- `tests/mcp/ssh_loopback_check.py`：仅回环的 SSH 协议测试端返回固定数据，验证正常
  退出、非零退出、原始输出上限和超时；不执行真实 shell、不读取用户保存的凭据。
- `tests/mcp/performance_check.py`：64 MiB 唯一文本、100 万行历史配置、四客户端
  各 10 Hz、三组开/关对照；同时记录成功读取数和 Busy 数，不能把快速 Busy 当成功读取延迟。
  Windows 在 CPU 数足够时固定负载与客户端的不同 CPU 集，结果文件记录 affinity mask。

本机验收过程中发现既有 Ninja/MSVC 的中文 `/showIncludes` 前缀与诊断代码页不一致，
头文件依赖记录可能为 0。根 CMake 已对 Ninja/MSVC 加入原始字节解码与前缀兼容层：
由 PowerShell 优先按 UTF-8、失败时按系统代码页解码，统一输出 Ninja 可识别的
ASCII 依赖前缀。路径选项另规范化为正斜杠，保留宏定义转义。已在修正后完成
干净构建，并抽查 Core、主程序、MCP 与 SSH 测试对象均记录真实头文件依赖；
以下结果仅使用这轮二进制。

Windows 验证结果（2026-09-18，MSVC + Qt 6.8.3，Release）：

| 验证 | 结果 |
| --- | --- |
| 主程序、stdio 桥接及全部测试目标构建 | 通过；翻译资源已刷新 |
| MCP 自动化 | 18 通过、0 失败；含真实 stdio/IPC、授权隔离、票据去重、输入上限、多实例与断连不自动改投 |
| 全套 CTest | 11/12 目标通过，154.19 秒；唯一失败目标为既有 ConPTY 用例，详见下述说明 |
| 官方 MCP Python SDK 1.30.0 | 五个工具的 schema、双客户端、读取/搜索、执行及重放检查通过 |
| 临时回环 SSH | 正常退出、退出码 42、输出超限、超时四项通过；没有连接用户保存的服务器或执行真实 shell |
| 文档与本地化 | Python 语法、翻译 XML、JSON 示例及 P8 相对链接通过 |

回环 SSH 的 libssh 在本机沙箱内初始化连接选项失败，因此经授权在沙箱外验证；
测试仍只监听 `127.0.0.1`，关闭用户/系统 SSH 配置加载，使用临时主机密钥和
known_hosts。此结果只证明真实 SSH 协议与通道处理，不代表真实服务器部署验收。

ConPTY 单独输出断言日志后为 8 通过、4 失败，均对应 `AGENTS.md` 的既有问题：
`duplexLoadAndBackpressure`、`latestResizeWins` 的偶发超时/旧尺寸，以及
`injectedStartupStagesRollBack`、`repeatedLifecycleReturnsResourcesToBaseline`
的系统句柄泄漏。没有将这四项标成 P8 通过，也没有扩大本次修改去修复 ConPTY。

高输出对照**未达到性能验收门槛**：每轮 64 MiB 唯一文本、百万行容量配置、
四个客户端各 10 Hz，三组 A/B，负载/客户端 CPU mask 分别为 15/1048560。

| 项目 | 第 1 组 | 第 2 组 | 第 3 组 |
| --- | --- | --- | --- |
| MCP 关闭吞吐（MiB/s） | 12.96 | 16.35 | 16.07 |
| MCP 开启吞吐（MiB/s） | 14.98 | 14.25 | 15.33 |
| 成功读取 / 总读取 | 7/168 | 6/175 | 4/165 |
| Busy 次数 | 161 | 169 | 161 |
| 成功捕获 P95（ms） | 2.2705 | 3.3639 | 0.2503 |
| 成功 RPC P95（ms） | 81.82 | 80.06 | 72.44 |

吞吐中位数为 **16.07 → 14.98 MiB/s（下降 6.79%）**；基线本身低于 20 MiB/s，
轮次波动较大，且成功样本很少。这些数据既不能证明吞吐损失在门槛内，也不能
用大量 Busy 的低耗时替代成功读取延迟。当前现象应按“持续写锁竞争导致 read
starvation”处理，而不是简单调大 try-lock 等待时间。下一轮优先实现/验证 §3.3 的
按需不可变快照复用，并按 §9.1 分离正常负载与过载测试；同时补齐 GPU frame P95
与真实终端场景。保留 try-lock 失败即返回 Busy 的安全边界，不为提高成功率阻塞 GUI
或暂停 Parser；只有 A/B 数据证明 PublishedContextSnapshot 同时提高有效响应率且不突破
吞吐/frame 门槛时，才将其设为默认读取路径。

尚需单独注明的验收范围：Linux/macOS 实机、具体桌面 MCP Host、真实 Linux 服务端
部署及 GPU frame P95 尚未覆盖；持续满负载下 try-read 可能频繁返回 Busy，短时
夹具不能替代完整性能验收。ConPTY 的既有系统句柄泄漏不归入 P8 功能修复。

v0.3 下一轮完成判据：

- MCP 2025-11-25 现有互操作不回归，新增 2026-07-28 `server/discover`/per-request metadata 路径通过至少一个 Tier 1 SDK；
- 正常设计负载 `read_context` 有效响应率 ≥99%，且终端吞吐、GUI frame P95 满足 §9.1；
- 至少输出 §9.3 的 coreCaptureCount、snapshotPublish/Reuse、coalescedRead、copiedBytes、snapshotAge 和 per-client starvation 指标，证明优化不是靠返回长期旧快照或隐藏 Busy；
- 4 客户端共享同一 session 时，同 revision 的基础文本不得按客户端数线性复制；capture/search 应复用 immutable snapshot/blocks；
- 过载测试不再只看成功请求 P95，必须报告各客户端 goodput、Busy 与最长无成功响应时间；
- search excerpt 不越过 capture 边界，CommandPlatformProfile 不通过探测或 PATH 放宽命令安全策略；
- `TerminalStateCache` 的借用字符串索引生命周期 invariant 有代码级证明和回归测试。

v0.4/v0.5 性能实施建议顺序（与全会话命令执行可并行推进）：

1. `PublishedContextSnapshot` 发布与跨客户端共享；
2. capture 仅持 shared snapshot/projection 元数据，去除同 revision 文本重复副本；
3. per-session capture in-flight coalescing；
4. recentOutput 分块/结构共享，减少完整 256 KiB 重建；
5. 增加 §9.3 copiedBytes/allocation/reuse/freshness 观测；
6. 再根据 profile 决定 UTF-8/JSON/私有 IPC 编码是否值得继续优化。

上述条目在代码和 A/B 验证完成前均属于设计目标，不写入“已实现”列表。

#### 14.1 v0.5 全会话命令执行的剩余增量

> 本节保留 v0.5 实施基线。v0.6 已选择统一的 Session 级交互命令协调器，后续
> 实施顺序和完成定义改以 §15.10 为准；不得继续把 v0.5 的 isolated-only 目标
> 当成最终架构。
>
> 2026-09-28 校对：下条第 2、3 条已被 v0.6 覆盖，不再是缺口 —— Interactive
> framing/ready-state 由 `InteractiveCommandProfile` + `SessionCommandCoordinator`
> 承担（不再是 `CommandPlatformProfile` 的职责，见 §5.4.4），`SessionCommandLease`
> 已由 `SessionInputArbiter` 的单 Lease + 用户抢占实现。Serial/Telnet 确实有
> InteractiveFramed Profile，但 commandId 集合为空、fixture 与 contract tests
> 仍缺。SSH 的 Profile 现为 `linux-diagnostics-v2`。

通用 `CommandExecutionRequest/Result/Outcome`、`SessionCommandFacade`、Executor 接口、
SSH bounded exec 适配，以及 `McpService` 去除 SSH 业务层强耦合已经完成并由
`novaterm_mcp_tests` 与 `novaterm_ssh_transport_check` 覆盖。以下条目仍不属于当前
“已实现”列表：

1. 补齐 macOS LocalShell 独立进程 Executor；Linux LocalShell 固定诊断已通过本机自动化测试，桌面人工验收仍待完成；
2. ~~扩展 `CommandPlatformProfile` 的 Interactive framing/ready-state 定义~~ — **已由 v0.6 覆盖**，改为 `InteractiveCommandProfile` + `SessionCommandCoordinator`（§5.4.4）；
3. 实现 `SessionCommandLease` 和 Serial/Telnet InteractiveFramed fixture — **Lease 部分已由 v0.6 覆盖**；Serial/Telnet 的 InteractiveFramed **fixture 与 contract tests 仍缺**，其 Profile 的 commandId 集合当前为空；
4. Custom 只通过显式 Executor 注册加入能力，不设置隐式默认行为；
5. 补齐 Serial/Telnet/Custom 的命令目录、错误码和 UI 风险提示；保持现有五工具和 schemaVersion=1；
6. 跑 SSH 回归 + Linux/macOS Local + Serial/Telnet contract tests 后，才能把 §14 状态改成全会话已支持。

实现过程中若发现某类 Session 无法可靠建立命令边界或终止语义，应保守地让该 Profile
`executionEnabled=false`，而不是为了覆盖率退化成任意 `writeUserInput()`。

用户接入步骤见 [MCP 使用说明](../../../MCP_Usage.md)。

#### 14.2 v0.6 实现与历史验收记录（2026-09-26，2026-09-28 增量）

本节的业务能力继续有效；stdio/IPC 配置、构建目标与测试成绩属于当时记录，
对应入口于 2026-10-12 删除。当前传输事实与 HTTP 验收状态见 §4 及文末迁移节。

2026-09-27 增量：Linux LocalShell 的固定诊断接入独立 helper 和
`linux-local-v1` Profile，复用现有有界 Executor；四个只读 commandId 通过
`novaterm_mcp_tests` 的 helper、授权、GUI 勾选和独立执行检查。
真实 Linux 桌面人工验收仍待补充。

2026-09-28 增量（三批收口，见下文「2026-09-28 收口」小节）：
`read_context` 默认取数路径改为 try-read 优先、发布物兜底；补齐 §9.1 的 CPU 帧
指标并把 Published 路径转为默认；「允许交互命令」产品授权真正接线且默认关闭；
`cd` 不再判为低风险。

以下内容是当前 `0.2.39` 工作树中已经进入代码的实现，不将尚未做的桌面人工验收、
跨平台验收或性能闸门宣称为完成。

| 能力 | 当前实现 |
| --- | --- |
| 用户抢占 | `SessionInputArbiter` 记录用户输入代际；用户输入会先通知 MCP、使已缓存的提示符 readiness 失效，再写入 Transport。脚本 SFTP 上传时用户开始键入会取消尚未完成的 provider 写入；注入后的交互命令沿现有 Coordinator 抢占路径结束。 |
| 交互输入/输出 | `InteractiveStreamFramer` 在进入 `TerminalCore` 前隐藏内部结束标记及普通 SSH/POSIX 命令附带的完成片段；`SessionCommandCoordinator` 用有界分块、单次 Lease 和输出预算收尾。配置了可信 Profile 的会话继续使用 ready prompt；未配置 Shell integration 的 SSH 会话可在没有提示符证据时直接写入当前终端，依靠命令末尾的 nonce/退出码标记证明完成。此模式无法可靠排除 TUI、密码提示或未提交用户输入，缺少完成证据时返回不确定结果。 |
| 人类确认 | MCP 2025-11-25 使用 `ElicitationBroker` 的反向 `elicitation/create`；MCP 2026-07-28 使用现代 `server/discover`、逐请求 `_meta`、`resultType=input_required` 与 MRTR `requestState/inputResponses`。缺少表单 Elicitation 时，Confirm/Unknown 命令和所有脚本均返回 `CLIENT_CONFIRMATION_UNAVAILABLE`，不降级到工具参数里的自报布尔值。 |
| 一次性状态 | MRTR 状态由当前本机 IPC 会话密钥 MAC 签名，绑定客户端身份、Session/epoch、目标指纹、Profile/风险策略、权限代际、命令或完整脚本请求摘要及过期时间；可信 Profile 绑定提示符代际，无提示符 SSH 命令额外绑定用户输入代际。服务端再次核验并消费一次性 nonce。权限撤销、重放、正文/路径篡改或状态变化均拒绝继续。 |
| 产品授权 | AccessStore/UI 有四项独立授权：读取共享、逐项固定诊断、脚本任务、**交互命令**。读取共享只允许读，**不蕴含向当前终端输入的权限**；`run_command` 需单独授予「交互命令」，默认关闭。未授权时 `list_sessions` 不发布 `run_command` capability，直接调用报 `COMMAND_PERMISSION_REQUIRED`（重提确认且已撤销时报 `COMMAND_CONFIRMATION_STALE`）。已授予交互命令后，普通低风险命令无需客户端确认，Confirm/Unknown 命令仍只通过 MCP 客户端人类确认继续。脚本仍需独立脚本授权，且每次确认。确认是提交授权，不能保证接受后的命令没有破坏性。 |
| 脚本目标文件 | LocalShell 用 `QSaveFile` 原子写入明确目标并设为仅属主可读写执行；SSH 用独立 SFTP 字节上传，单次与排队正文总量最多 2 MiB，并绑定活动 SSH endpoint/account/secret、known_hosts 路径及已验证主机密钥指纹。父目录必须存在；确认后可覆盖目标，脚本文件保留且不自动删除。 |
| 脚本调用 | 高危脚本/命令扫描到凭据读取、关闭安全机制、提权或格式化等已知行为时永久拒绝；其余脚本一律确认。写入成功后再次验证 Session、epoch、授权、Profile、提示符、目标与正文摘要，再通过协调器在当前终端显示/执行 invocation。正文不进入终端 UI；用户输入、写入失败或状态变化不会提交后续调用命令。 |

本次自动化与回环验收：

- `NovaTerm`、`novaterm_mcp_tests`、`novaterm_session_tests`、`novaterm-mcp` 与 `novaterm_ssh_transport_check` 构建成功；
- MCP/Session 专项 CTest 通过；覆盖 2025 accept/decline/cancel、2026 discover/MRTR、MAC 篡改/重放、权限撤销、脚本确认前后落盘、调用命令不含正文、写入失败零注入、无提示符 SSH 的本终端命令/危险命令确认及用户输入抢占；
- 官方 2025 MCP Python SDK 互操作和 2026 原始 stdio wire 检查通过；
- `ssh_loopback_check.py` 只连 `127.0.0.1` Paramiko 夹具，SSH 固定命令、无提示符交互 `pwd` 的命令/输出进入 TerminalCore 且内部片段隐藏，以及 SFTP 写入字节核对通过；夹具不运行真实 shell 命令；
- `novaterm_ssh_transport_check.exe` Transport/Profile/协议专项通过。

仍未完成的验收：真实 SSH Shell 的回显过滤、TUI/密码提示误输入风险及 Windows 桌面 PowerShell/Clink 人工场景；Linux/macOS PTY shell 人工场景；命令运行期间 GUI frame P95 和现有性能预算复测。完整 CTest 状态应以本轮最终验证记录为准。

##### 2026-09-28 收口

三批改动的代码事实：

1. **`read_context` 默认取数路径**。`TerminalContextProvider::trySnapshot()` 改为
   **try-read 优先、发布物兜底**：try-read 是非阻塞 try-lock，模型空闲时总能取到最新
   数据，因此是主路径；只在持续输出把模型锁占满、try-read 失败时才用 Parser 发布的
   不可变快照，不再直接返回 Busy。发布物带 2 s 硬性年龄上限，防止解析器停摆后把旧
   快照当成当前数据（年龄判据双侧拒绝，时钟回拨产生的负值同样按不可用处理），并只
   接受 revision 前进的发布物。发布请求带调用方的历史行高水位
   （`requestPublishedTerminalState(sinceLineId)`），使兜底路径的截断/增量语义与直接
   捕获一致 —— 固定传 0 会让发布物恒带 `truncated`，消费方每次都被迫全量重发。
   `NOVATERM_MCP_PUBLISHED_SNAPSHOT` 语义反转为「未设置即启用，显式置 0 才退回
   try-read」，并集中到 `publishedContextSnapshotEnabled()`：变量名只出现一次，避免
   拼错导致逃生开关永久失效。
2. **产品授权接线**。`Grant::interactiveCommand` 与 `AccessStore::canRunCommand`
   此前一直存在但从未被 `McpService` 调用，UI 恒传 `false`，`run_command` 全链路只看
   `canRead` —— 没有任何途径拒绝 AI 向当前终端输入。现已在 `runCommand()` 入口、
   `list_sessions` 的 capability 门与 2025 elicitation 恢复路径三处接线。
   该闸门**只约束 `novaterm_run_command`**：`run_script` 有自己的独立授权位与逐次确认，
   两项能力在 `list_sessions` 里也各自判定。若一并要求交互命令授权，「只授脚本任务」
   的合法配置会在脚本正文已落盘之后才失败，并报出错误的错误码。
3. **`cd` 不再判为低风险**。`run_command` 的执行目录是当前交互 shell 的动态工作目录，
   免确认的 `cd` 会静默改变此后所有相对路径命令的语义，且它在终端里只是一行普通回显、
   不进执行记录。现落到 `Unknown`，服务层按既有设计把 `Unknown` 与 `Confirm` 同样处理
   为需客户端确认。`RiskPolicyVersion` 升到 `interactive-risk-v4`，使旧的 Allow 分类
   无法被重放。

性能闸门（本机 Linux，Release，Xorg + kwin + NVIDIA RTX 4070，xcb + OpenGL，负载与
SDK 客户端分配到不同 CPU 集 0-3 / 4-27）：

| 项目 | 门槛（§9.1） | 正常负载实测 | 过载实测 | 判定 |
| --- | --- | --- | --- | --- |
| 有效响应率 | ≥99% | 94.2%（162/172，Busy 3~4） | 98.8%（324/328，Busy 1~2） | **未达标** |
| 成功 RPC P95 | ≤100 ms | 45.9~67.1 ms | 45.1~68.9 ms | 通过 |
| GUI 捕获 P95 | ≤2 ms | 0.888~1.284 ms | 0.495~0.992 ms | 通过 |
| 终端吞吐下降 | ≤5% | 无下降（15.97 → 17.10 MiB/s） | 无下降（17.14 → 19.09 MiB/s） | 通过 |
| GUI frame P95 增量 | ≤2 ms | **无法判定**（见下） | **无法判定**（见下） | **未闭合** |

对照旧默认路径（纯 try-read）同机实测：正常负载有效读取率只有 20~26%（Busy 39~52），
过载只有约 14%（Busy 91~111）。发布物兜底把 Busy 降到 3~4 与 1~2。

**必须同时记录的口径与偏差**：

- **帧指标无法判定门槛，且不得写成通过。** 三条独立原因叠加：
  1. 口径只是 `render()` 入口到出口的 **CPU 时间**（`cpuFrameP95Nanoseconds`），
     不含 present/fence、帧间隔与合成器；`QRhiWidget` 没有 `frameSwapped` 信号，
     端到端 GUI 帧延迟当前测不到。
  2. P95 取自最近 2048 帧的**滚动窗口**，而本轮每轮只有 120~965 帧，窗口从未填满，
     P95 实质是「第 9（或第 1）大的那一帧」，抽样误差极大。
  3. 更关键的是**离散度远大于待测差值**：过载轮 baseline 臂三次为
     9.185 / **13.059** / 9.060 ms（臂内极差 **3.999 ms**），mcp 臂 9.229 / 9.244 /
     9.413（极差 0.184 ms）。此前报告的 delta（+0.117 / +0.088 ms）比 baseline 臂
     自身极差小一个数量级，不具备可辨识性。
  因此 `performance_check.py` 现在**拒绝输出 delta**：每轮帧数低于
  `NOVATERM_MCP_PERF_MIN_FRAMES`（默认 1000）时，把 `cpuFrameP95DeltaMs` 置为 null、
  `cpuFrameP95Reliable=false`，只如实报告两臂原值与极差。**§9.1 的 frame 一项因此仍未
  闭合，不得表述为「帧门槛已过」。** 要真正闭合需要：把负载拉长到每轮 ≥2048 帧以填满
  窗口、在 GUI 线程上另量一段含 `readContext` 投影/编码的窗口时间，并增加轮数以压低
  离散度。
- 本机 offscreen 平台插件拿不到 QRhi（`QRhiWidget: QRhi is not supported on this
  platform`），必须用 xcb + `QT_WIDGETS_RHI=1` + `NOVATERM_RHI_API=opengl`。因此
  `performance_check.py` 会为夹具子进程设置这三项；常规 ctest 仍保持
  `QT_QPA_PLATFORM=offscreen`，因为渲染器只在 `--perf-*` 路径实例化。夹具在拿不到设备
  时以退出码 3、负载期间零帧时以退出码 4 失败，不把「零帧」当成 0 ms 的好成绩。
- **有效响应率 92.0% 未达 §9.1 的 ≥99% 目标，是本次未闭合项**，不得写成性能验收通过。
  剩余 Busy 集中在首批读取（此时尚无发布物可兜底）。

本轮验证：`novaterm_mcp_tests` 48/48；本机全套 ctest 11/11 通过（27.96 s）。未跑
`novaterm_terminal_session_tests`（Windows-only，本机构建无此目标）。

##### 剩余缺口（2026-09-28）

1. **`run_command` 没有 `commandTicket` 幂等保护**。`execute_command` 有票据 + 至多提交
   一次 + `idempotentHint=true`；`run_command`（≤16 KiB，可含复合 shell、重定向、解释器）
   完全没有，客户端超时重试就是第二次真实执行。本次不修。
2. **有效响应率未达 ≥99%**（正常负载 94.2%、过载 98.8%），见上表。
3. **§9.1 的 GUI frame P95 一项未闭合**，原因与所需补测见上节口径说明。此前写入本节的
   「帧门槛已过」结论已撤回。
4. `Grant::confirmedCommand` / `canRunConfirmedCommand` 仍全项目零调用。危险命令的确认
   完全由 MCP 客户端 elicitation 承担，该字段暂为预留，删除它需改动 12 处测试调用点。
5. 真实桌面 Shell/TUI 验收、跨平台验收与端到端 GUI 帧延迟测量（见上节与 AGENTS.md）。


## 4. HTTP-only之后的完整实施序列

### 2026-10-12：MCP 桥接随绑定应用退出（历史，入口随后删除）

`novaterm-mcp` 由客户端启动，不属于 NovaTerm 持有的 QProcess。旧
Bridge 在 IPC 关闭后只返回请求错误，stdin 尚未 EOF 时仍保持进程。
增加退出后又复现 Windows stdio 收尾卡住：`CancelSynchronousIo` 返回
`ERROR_NOT_FOUND`（1168），读线程仍未结束。当前收尾同时按线程和
stdin/stdout 句柄取消未完成 I/O，并等待实际完成后 join；按句柄取消的
语义参见 [Microsoft CancelIoEx 文档](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex)。

当前约定：初始化时尝试认证绑定唯一可用实例；无应用、缺令牌或多实例
未选定仍支持离线能力发现。未绑定时监视实例目录；目录不存在时先
监视最近现存父目录，创建后收窄；应用后启动时也会绑定，不要求客户端
先调用工具。认证成功后移除目录监视。绑定后 IPC 关闭，拒绝新请求、不自动改投，
先排空在途请求的错误响应，最多等待 2 秒，随后停止 stdio 并正常退出。
已发出的执行无法证明完成时保留 `COMMAND_OUTCOME_UNKNOWN`，不据桥接
退出推断远端进程已终止；客户端需要明确新建桥接才能重新连接。

回归先失败再修复：`boundBridgeExitsWhenApplicationStops`、
`initializedIdleBridgeFollowsApplicationLifetime`、
`bridgeFlushesPendingFailureBeforeExit`；另补
`offlineBridgeBindsWhenApplicationStartsThenExits`，先复现仅客户端
初始化后应用再启动仍未绑定，再以目录监视修复。多实例／断连回归改为验证旧
进程退出后由新桥接明确恢复；离线初始化仍通过。Windows 主程序、桥接
及 MCP 测试目标构建通过，完整 `novaterm_mcp_tests` CTest 1/1 通过，
11.48 秒。两轮只读线程复核未发现明确新增问题。本轮未重跑其他模块
或 Linux/macOS，不改变 P8 整体阶段状态。

Windows 独立进程手工检查不注册默认 CTest、不依赖 MCP SDK、不读取
真实接入令牌，运行方式：

```powershell
python tests/mcp/shutdown_check.py --fixture build/Release/bin/novaterm_mcp_tests.exe --qt-prefix C:/Programs/Qt/6.8.3/msvc2022_64
```

三个场景都保持客户端 stdin 打开：应用正常退出，桥接约 16 ms；应用
强制结束，约 15 ms；stdout 留有阻塞输出且客户端不读，约 2016 ms。
三项均正常退出、退出码 0。慢读检查只发三份工具定义，避免以长时间
逐条 ping 先触发原有 10 秒慢写保护而混淆退出期限。临时配置与进程
均由夹具创建并回收；未连接真实 SSH/串口或终止用户的主程序。
结果在 `build/mcp-shutdown-ctest-final.txt` 与
`build/mcp-shutdown-check-final.txt`，改动尚未提交。

### 2026-10-12：仅进程内 Streamable HTTP 迁移

当前入口仅为 NovaTerm 拥有的 Qt HTTP Server。旧 `tools/novaterm-mcp/`、
`LocalMcpServer`、对应 CMake 目标/安装 RPATH/测试依赖宏已删除；共享
`ElicitationBroker`、`WireSession` 和 `HttpMcpEndpoint` 位于 `src/mcp/`。
旧 stdio 桥接退出修复、
继承管道取消和子进程检查保留为历史记录，不构成 HTTP 入口的验收证据。

默认本机 URL `http://127.0.0.1:38973/mcp` 在重启后保持稳定，主应用启动前
`NOVATERM_MCP_HTTP_PORT` 可覆盖端口；端口冲突明确失败，不自动切换。
每请求 Bearer token，配置导出为 `{type:"http",url,headers.Authorization}`；
Codex 改用 `url`/`http_headers`，旧 `command`/`args`/`env` 配置必须迁移。
2025 为会话/elicitation，2026 为无会话 MRTR/断流取消，细节与许可见 §4。

新增 CTest 目标为 `novaterm_mcp_wire_tests`、`novaterm_mcp_http_tests` 与
`novaterm_mcp_endpoint_tests`。2026-10-13 Windows 注册 26 项，Release 全套
26/26 通过（233.97 秒）；Linux 按条件预计 24，未实跑核对。

本轮验收：

| 范围 | 结果与边界 |
| --- | --- |
| 主程序与全部默认目标 | Windows Release 构建通过；Qt HTTP Server/WebSockets DLL 已部署 |
| CTest | 26/26；四项 MCP 合计 40.76 秒，其余为默认回归 |
| 官方 Python MCP 1.30.0 SDK | 2025 HTTP 初始化、七工具/schema、有界上下文、搜索、固定诊断与票据重放检查通过 |
| 原始 2026 HTTP | discover、版本拒绝、catalog/read 通过；MRTR/脚本确认由 Qt 业务回归覆盖 |
| HTTP 生命周期 | 空闲关闭、未读待请求 SSE、应用崩溃三项通过；停止后监听不可达，未启动 MCP 子进程 |
| Codex CLI | URL/http_headers 配置解析为 streamable_http；仅配置解析，未验证桌面实接或修改用户配置 |
| 审查与资源 | 头/正文代际不一致在分流前 400 拒绝；拒绝/半包 socket 断连回收，计数回到基线 |
| 仍未重测 | Linux/macOS、真实桌面客户端接入、迁移后性能对照 |

HTTP 网络字节由独立 worker 处理，Qt 6.8 官方 parser/responder 负责最终解析
与响应。为防 Qt parser 消耗无界未完成请求，并拒绝它宽松接受的非法 chunk
边界，进入 parser 前做有界原始交换核验；正文 2 MiB、头 16 KiB、请求/队列
均有限额。一次 TCP 只承接一次交换。2025 HTTP 断开保留逻辑 wire，会话 DELETE
或超时回收；2026 断流只取消该 exchange，Bearer 的内部授权上下文保留短期
MAC 状态以支持重提，不产生 MCP session。

原始 HTTP 超限用例验证 413；Windows Qt 在完整上传中遇到提前关闭也可能呈现
传输错误，业务回归要求请求不进入工具调度，不能把该错误当成成功或自动重放。

日志为 `build/mcp-http-full-ctest.txt`、`build/mcp-http-interop-final.txt`、
`build/mcp-http-shutdown-final.txt`。测试只创建临时回环连接/进程，未连接真实
用户 SSH/串口。本轮修改随 0.2.68 提交。

### 2026-10-14：HTTP 审查修复与响应编码

连接按完整远端 IP/port 定位、删除核对实例；初始化/消息在上下文分配前
校验，无法交付初始响应时回收新 context。版本拒绝统一 supported/requested。
头验证后支持单次 100 Continue，replay 去 Expect 防 Qt 重复握手，未知 Expect
返回 417；解析后 404/405/503 持有 responder 排空。停机当前 socket 数归零。

最终 RPC 在 wire 编码为 QByteArray，另传错误码，Endpoint 不再重新编码或
解析；Service 不再编码内部 DTO，SSE 校验/组帧移入 I/O，额度涵盖多行 data
前缀，超限替代包仍有上限。2026 请求能力字段在共享 validator 前置核验。
2026 HTTP 不定义客户端核心通知，通知不分配 context，取消按请求流断连。
通知与 RequestMeta 不混用，依据见
[2026 Streamable HTTP](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http)。

Windows 主程序构建、MCP 四项 4/4（45.91 秒）、SDK 互操作及退出三矩阵通过；
独立复核未发现高置信残留。256 KiB wire 微基准约 1.98 → 1.28 ms/次（减少
约 35%），只代表固定 50 次/median 3 测量，不代表整机 GUI 帧率。

测试网络截止时间、传输错误检查及 reply 收尾已加强；QFile 的 QVERIFY
本已检查结果，回环 HTTP 不因 SSL 静态提示改 HTTPS。详细 RED/GREEN、计划及
日志入口见 `docs/superpowers/plans/2026-10-14-http-mcp-review-fixes.md`。
本轮未重跑非 MCP 全套、跨平台或整机 GPU 压力；改动随 0.2.68 提交。

### 2026-10-15：Linux MCP 专项与实际客户端只读用例

环境：Ubuntu 24.04、Linux 6.8.0-146-generic、Qt 6.8.3、GCC 13.3.0，
NovaTerm 0.2.69；使用 `build/` 的 Debug 配置，含本轮标签名称同步的未提交改动。
`ctest -N -L mcp` 实际注册四个目标。本节追加本轮结果，不改写此前 Windows
验收记录，也不代表非 MCP 全套、macOS 或性能验收通过。

#### 自动化执行与结果

在仓库根目录执行：

```bash
cmake --build build --target novaterm_mcp_tests novaterm_mcp_wire_tests novaterm_mcp_http_tests novaterm_mcp_endpoint_tests -j 4
ctest --test-dir build -N -L mcp
ctest --test-dir build -L '^mcp$' --output-on-failure --verbose \
  --output-log build/mcp-validation-2026-10-15.log \
  --output-junit build/mcp-validation-2026-10-15.xml
```

HTTP 测试需要允许本机回环监听，因此本轮在沙箱外运行。此前沙箱内的
`HTTP startup failed`/超时不计为通过；以下成绩来自本轮完整重跑。
GUI 用例由测试目标自带的 `QT_QPA_PLATFORM=offscreen` 运行。

| CTest 目标 | QTest 通过 / 失败 / 跳过 | CTest 耗时 | 结论 |
| --- | --- | --- | --- |
| `novaterm_mcp_wire_tests` | 10 / 0 / 0 | 0.40 s | 通过 |
| `novaterm_mcp_http_tests` | 50 / 0 / 0 | 1.79 s | 通过 |
| `novaterm_mcp_endpoint_tests` | 28 / 0 / 0 | 2.61 s | 通过 |
| `novaterm_mcp_tests` | 61 / 0 / 3 | 14.90 s | 通过，含平台跳过 |

CTest **4/4 通过，总墙钟时间 19.70 秒**。表中 QTest 数量含数据行及每个套件的
`initTestCase`/`cleanupTestCase`，不是测试函数数。三条跳过均为 Windows
所有权检查：`administratorOwnedObjectsMigrateToCurrentUser(endpoint-directory)`、
`administratorOwnedObjectsMigrateToCurrentUser(private-file)` 和
`unrelatedGroupOwnedDirectoryIsRejected`；不能据此声称 Linux 验证了 Windows ACL。
日志中的 offscreen `propagateSizeHints` 和夹具的状态迁移 QWARN 未导致断言失败。

以下按场景归类记录本轮已运行的关键 case；完整函数及数据行结果见上述日志。
名称不带套件前缀时属于 `McpTests`。

| Case | 操作与预期断言 | 本轮结果 |
| --- | --- | --- |
| `WireSessionTests::initializationAndCatalog`、`modernDiscoverDoesNotRequireInitialization` | 2025 初始化后提供七工具；2026 discover 无需初始化 | 通过 |
| `WireSessionTests::encodedResultPreservesUnicodeAndOutputLimit`、`cancelledCallNeverProducesNormalResult` | 中文/转义往返保真、响应超限有界；取消后的迟到结果不作为正常响应发布 | 通过 |
| `HttpMcpServerTests::rejectsMalformedHttp`、`expectContinueBeforeBody` | 非法 Host/Origin、重复长度、非法 chunk、超限及未知 Expect 被拒绝；合法 Expect 单次 100 Continue | 全部数据行通过 |
| `HttpMcpServerTests::requestFloodWhileGuiBlockedIsBounded`、`rejectedSocketsReturnResourcesToBaseline`、`stopResetsLiveSocketsAndPreservesPeak` | GUI 阻塞时请求受限；拒绝连接回收；停机连接归零并保留峰值统计 | 通过 |
| `HttpMcpEndpointTests::modernHeaderMismatchDoesNotDispatch`、`invalidInitializeDoesNotReserveClient`、`legacySessionCannotBeUsedByOtherClient` | 版本/头正文不匹配不分发，非法初始化不占容量，会话不能跨客户端复用 | 全部数据行通过 |
| `HttpMcpEndpointTests::modernDisconnectCancelsOnlyItsRequest`、`legacyDisconnectDoesNotCancelButDeleteDoes` | 2026 断流只取消对应请求；2025 普通断流不取消、DELETE 取消 | 通过 |
| `httpInitializedSessionReadsWithAuthorization`、`httpRejectsMissingTokenAndForeignOrigin`、`httpPendingCommandStopsWithApplication` | 真实回环 HTTP 读取、认证拒绝及应用停止时在途请求收尾 | 通过 |
| `readSearchAndClientIsolation`、`truncatedOutputNeverAdvancesCursor`、`revokeAndReconnectInvalidateAccess` | 有界读取/搜索隔离；截断不推进增量游标；撤权与重连使旧访问失效 | 通过 |
| `commandsRequireSeparateGrantAndAreDeduplicated`、`cancelledCommandNeverReplays` | 固定诊断独立授权、参数与票据校验、执行去重及取消不重放 | 通过 |
| `unknownExecutionProtectsTarget`、`privateMarkersSurviveServiceRestart`、`sessionInvalidationCompletesPendingExecution` | 未知结果隔离目标、保护标记持久化、会话失效收尾及人工确认后解除保护 | 通过；人工解除为服务接口测试，不代表手工点击确认框验收 |
| `humanDeclineLeavesTerminalUntouched`、`humanAcceptRunsOnlyAfterConfirmation`、`mrtrRequiresBoundOneShotConfirmation` | 拒绝不写终端；接受后才提交；MRTR 确认绑定请求且只能使用一次 | 通过，使用假 Transport |
| `scriptContentRequiresConfirmationAndNeverEntersTerminal`、`scriptMrtrConfirmationIsSignedAndOneShot`、`userTypingCancelsInFlightScriptUploadBeforeInvocation` | 脚本需确认、正文不注入终端；确认不可重放；用户输入取消尚未调用的上传 | 通过，使用夹具 |
| `settingsDialogSeparatesReadAndScriptPermission`、`settingsDialogLocalShellFixedDiagnosticsCanBeSelected`、`settingsDialogRefreshPreservesExpandedSessions` | 读取/诊断/脚本/交互命令授权独立，复选框整格点击有效，刷新保留展开状态 | 通过 |
| `settingsDialogUsesTabDisplayTitle` | 中文标签名显示；OSC 动态标题不覆盖；改名刷新且 id/epoch/attachmentId 不变；同名不刷新，长度仍受限 | 通过 |

#### 实际 MCP 客户端只读冒烟

使用当前客户端连接 NovaTerm 0.2.69，只操作显式共享的串口会话 `ttyUSB1`。
sessionId、epoch、captureId 均取自对应接口响应，不通过当前活动标签隐式选择。
搜索词从本次已返回的 viewport 中选取，未把终端正文、搜索词或接入令牌写入报告。

| 编号 | 接口 / Case | 参数与预期 | 实测结果 |
| --- | --- | --- | --- |
| LIVE-MCP-01 | `novaterm_list_sessions` | `limit=50`，列举授权会话及主界面标签名称 | 通过：返回 `ttyUSB1`、`serial`、`running`，`displayNameTruncated=false` |
| LIVE-MCP-02 | `novaterm_read_context` | `includeViewport=true,maxBytes=8192,maxLines=32`，返回有界快照并准确报告截断 | 通过：viewport 32 行；`outputTruncated=true,resetRequired=true,nextToken=null`，截断未推进游标 |
| LIVE-MCP-03 | `novaterm_search_context` | 使用上一快照的 captureId，`scope=viewport,caseSensitive=true,maxMatches=5` | 通过：命中 1 条；逐条核对 `lineIndex/startByte/endByte` 对应原快照的 UTF-8 字节 |
| LIVE-MCP-04 | `novaterm_list_commands` | 查询当前串口会话的固定诊断能力，不执行命令 | 通过：`commands=[]`、`executionEnabled=false`、`disabledReason=COMMAND_PROFILE_UNAVAILABLE` |
| LIVE-MCP-05 | `novaterm_read_context` 错误 epoch | 保持有效 sessionId，将 epoch 改为全零 UUID | 通过：`ok=false`、`STALE_SESSION_EPOCH`、`retryable=false` |

五项 **5/5 通过**。当前串口会话未提供可执行 Profile，未在真实串口上运行固定
诊断、交互命令或脚本，也未变更授权、断开连接或点击解除目标暂停；这些执行与
保护分支的本轮结果仅来自自动化夹具。官方 Python SDK、真实 SSH 执行、桌面
人工交互、Windows/macOS 及 GPU/高输出性能本轮未重跑。

原始自动化日志和 JUnit 为 `build/mcp-validation-2026-10-15.log`、
`build/mcp-validation-2026-10-15.xml`；只读冒烟的脱敏结果为
`build/mcp-live-validation-2026-10-15.json`。这些 `build/` 文件为本机产物；
本节保留可提交的 case、参数、结果及验收边界。

#### 2026-10-15 增量：仅通过当前 `novaterm` MCP 执行设计 case

本次通过当前环境真实的 `novaterm` MCP 工具发起请求，未运行 CTest 或测试
可执行文件。应用仍为 0.2.69，共享会话为 `ttyUSB1`（serial、running）。
先调用 `novaterm_list_sessions` 取得 sessionId/epoch，再据响应明确路由；
后续 captureId 与 sinceToken 均来自本次调用结果。仅调用会话列举、上下文读取、
上下文搜索和命令目录四个工具，没有向串口写入测试文本或执行命令。

§7.1–7.4 与此前 LIVE-MCP-01～05 的本次复测、扩展结果如下。下表数值为
当次快照，不要求设备后续输出逐字节保持一致。

| Case | 设计依据 / 请求 | 本次断言与结果 |
| --- | --- | --- |
| LIVE-MCP-01 | §7.1，`list_sessions(limit=50)` | PASS：返回 `ttyUSB1`、serial、running，与标签名称一致 |
| LIVE-MCP-02 | §7.2，`read_context(maxBytes=8192,maxLines=32,includeViewport=true)` | PASS：32 行、文本 2281 UTF-8 字节；输出截断且要求重置，nextToken=null |
| LIVE-MCP-03 | §7.3，搜索 LIVE-MCP-02 的原 capture，`scope=viewport,maxMatches=5` | PASS：命中 1 条；capture/revision 相同，字节区间逐条还原为查询词 |
| LIVE-MCP-04 | §7.4，`list_commands` | PASS：模板为空、executionEnabled=false、COMMAND_PROFILE_UNAVAILABLE |
| LIVE-MCP-05 | §7.2，实际 sessionId + 全零 epoch | PASS：STALE_SESSION_EPOCH，retryable=false |
| LIVE-MCP-06 | §7.1，`list_sessions(limit=1)` | PASS：仅返回同一授权会话、nextCursor=null；当前只有一项，不代表多页遍历验收 |
| LIVE-MCP-07 | §7.1，传入伪造目录 cursor | PASS：CURSOR_STALE |
| LIVE-MCP-08 | §7.2，`read_context(maxBytes=1,maxLines=1)` | PASS：文本仅 1 字节，outputTruncated/resetRequired=true，nextToken=null |
| LIVE-MCP-09 | §7.2，小预算后无 token 用 `maxBytes=262144,maxLines=1024` 重读 | PASS：返回 51776 文本字节、56 viewport 行和 735 recentOutput 行；源及输出均未截断，取得有效 nextToken |
| LIVE-MCP-10 | §7.2，用 LIVE-MCP-09 的 token 保持同一投影增量读取 | PASS：56 viewport 行、0 recentOutput 行、resetRequired=false，没有重复消费摘要 |
| LIVE-MCP-11 | §7.2，复用上述 token 但改 `includeViewport=false` | PASS：CURSOR_STALE，投影变化不能沿用旧游标 |
| LIVE-MCP-12 | §7.2，传入伪造 sinceToken | PASS：CURSOR_STALE |
| LIVE-MCP-13 | §7.3，授权会话与有效 epoch + 不存在的 captureId | PASS：CAPTURE_NOT_AVAILABLE |
| LIVE-MCP-14 | §7.3，在原 capture 的 both 范围查询特定未出现字面量 | PASS：matches=[]；仅证明该快照范围未命中 |
| LIVE-MCP-15 | §7.2，合法 UUID 格式但未共享的 sessionId | PASS：SESSION_NOT_AVAILABLE |
| LIVE-MCP-16 | §7.3，从原快照选择 ASCII 词并转大写，`caseSensitive=false,maxMatches=5` | PASS：返回 5 条；逐条核对原行字节区间，ASCII 折叠后与查询词一致 |
| LIVE-MCP-17 | §7.3，空 query | PASS：INVALID_ARGUMENT |
| LIVE-MCP-18 | §7.1，`limit=0` | PASS：INVALID_ARGUMENT |

总计 **18/18 case 通过，19 次 MCP 工具请求**。多出的一次是 LIVE-MCP-15
的参数探测：初次使用全零 sessionId，原先预期 SESSION_NOT_AVAILABLE，实际
返回 INVALID_ARGUMENT；全零 UUID 属于非法 sessionId，校验先于目录查询。
改用合法格式且未共享的 UUID 后得到 SESSION_NOT_AVAILABLE。保留该原始探测
及预期修正，不把参数校验当成会话可见性用例的通过证据。

当前能力为 read_context/search_context/list_commands/human_confirmation；
human_confirmation 仅表示客户端确认能力，不等于会话具有执行授权或可用后端。
本次未执行的设计 case：

| 范围 | 未执行原因 / 所需条件 |
| --- | --- |
| §7.5 固定诊断执行、票据重放与去重 | 当前 Profile 不可执行，目录不提供模板或 commandTicket；需有可执行 Profile 且独立授权的测试会话 |
| §15.4 命令/脚本，§15.5–15.9 确认接受/拒绝、取消、抢占、写入与执行 | 当前会话未发布 run_command/run_script；需专用测试 Session、对应独立授权及客户端确认通路 |
| 跨客户端 token/capture 隔离 | 当前仅一个接入身份，不能用伪造 sessionId 代替第二个真实客户端的隔离验收 |
| 重连、换绑、应用退出、HTTP 原始报文、GUI 布局与负载性能 | 当前七工具不提供相应控制/观测入口；需专门夹具或 UI 操作，本次未中断真实串口及应用 |

以上未执行项不计为 PASS，也不由此前 CTest 成绩替代本次实测。脱敏逐项结果
保存在 `build/mcp-live-design-cases-2026-10-15.json`，不含终端正文、查询词、
sinceToken、commandTicket 或接入令牌。该文件为本机产物，本节为可提交记录。

### 2026-10-15：串口命令配置接线与 MCP 代码审查

本轮补齐产品入口，保留默认禁用及分项授权：

- `SerialConfig` 新增提示符正则、命令行结束符 cr/lf/crlf、远端回显三项；
  端口表单显式配置，编辑时回填，SessionPanel 历史保存与重连及 SessionFactory
  创建均传递相同值。正则长度上限 256，非法正则和结束符拒绝；空规则保持禁用
  （2026-10-18 起空规则改为空闲收尾降级，见文件末尾对应小节）。
- `TerminalSession::attach(..., RuntimeConfig)` 在装配 Framer/Coordinator 和
  InputPump 前按值保存配置。TerminalView 和 TerminalPage 的串口、SSH、Telnet
  与本地 Shell 创建/编辑路径都传入本次独立配置，避免跨目标继承旧提示符或端点。
- 串口有效规则识别到当前行末提示符并通过 150 ms 静默窗口后，且客户端单独
  取得交互命令授权，才发布 `run_command`。没有提示符时拒绝执行，不注入探测命令。
  文件/字节仍走唯一 Session 输入通路；本轮未增加串口脚本写入或固定诊断模板。
- 新表单提供中英文翻译；已有串口历史缺省仍是空规则、CR、回显开启。
  SessionPanel 的测试历史路径改为显式注入临时目录，避免 Qt test mode 忽略
  XDG_DATA_HOME 后误用固定的测试用户目录。

新增回归：

| Case | 验证内容 | 本轮结果 |
| --- | --- | --- |
| `SessionTests::attachmentSnapshotConfiguresDeviceCommands` | 借用 Core 的真实 Session 在 attach 时取得独立配置；无提示符时不执行，提示符就绪后按 CRLF 输出；更换 Telnet 配置不继承串口规则 | 通过 |
| `SessionTests::serialCommandSettingsRejectInvalidInput` | 正则语法错误、超过 256 字符及非法结束符被拒绝 | 通过 |
| `SessionPanelTests::serialCommandSettingsRoundTripThroughFormAndHistory` | 表单回填/确认、临时历史保存、重建面板与双击重连保留三项值，不打开端口 | 通过 |
| `McpTests::configuredSerialSessionPublishesAndExecutesCommands` | 本机 HTTP + 假串口：未就绪拒绝、发布 run_command、不发布 run_script、独立授权、CRLF 发送与提示符收尾、撤权后零追加写入 | 通过 |

主程序及全部默认目标构建通过。共享 SerialConfig 数据布局与 Session 附着接口有
改动，因此按项目约定重建并运行了全套，而非只重用之前的专项成绩：

```bash
cmake --build build -j 4
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure --verbose \
  --output-log build/mcp-wiring-full-ctest-2026-10-15.log
```

运行环境为 Linux Debug，允许宿主回环网络、虚拟 PTY 及当前会话密钥环；全局
offscreen 仅用于本次 Linux 检查，不应照搬到 Windows D3D11 全套。
实际注册 **24 项，24/24 通过，总墙钟 170.32 秒**；MCP 四项合计 20.21 秒。
McpTests 为 62 通过/3 平台跳过，SessionTests 为 70 通过/2 跳过，SessionPanelTests
为 6 通过；均含初始化/清理。Session 跳过是 Windows Clink 与需显式开启的
虚拟串口 lrzsz 大文件矩阵，不视为真实设备验收。当前机器的 PTY 和 UI 布局项
本轮通过，不据此删除 AGENTS 中其他环境/历史的失败记录。

未重启或改动用户正在运行的 NovaTerm/ttyUSB1，未向真实串口发送命令。
真实设备仍需启动新版、编辑提示符规则并单独授权后验收；Windows/macOS 本轮
未运行。首次小范围验证遇到的测试问题（未设 offscreen、测试命令误用非免确认
模板、固定测试数据目录不可写）均已通过对应环境设置或夹具修正后复测。

按 qt-cpp-review 进行确定性扫描和六方向只读审查，去重后确认 **10 项剩余缺陷**
（P1 四项、P2 五项、P3 一项），尚未在本轮修复。高优先级为：解除暂停确认错绑
目标、SFTP 超时放弃线程后的 use-after-free、Zsh 进程替换绕过低风险确认、
脚本确认路径与实际落盘路径不一致。另有取消/上传事务竞争、LocalShell 能力漏报、
确认与执行期限不一致、首次共享预热死分支及脚本/HTTP 容量约束不一致。
以上为当时的审查发现；后续设计修复与回归统一记录在下一节，不单独维护审查报告。

两个本机探针仅对字符串进行风险分类、在自动回收的临时目录写入脚本内容，
证实了风险分类误放行与路径偏差；没有执行 Shell 或脚本。其他 SFTP 故障/长时
确认竞态当时主要为源码证据，不能把本轮 24/24 当成这些缺陷不存在；
下一节补充了对应修复和确定性回归。

### 2026-10-16：审查问题修复与安全回归

已完成前节记录的 10 项审查问题；本节统一保存设计变更和对应回归，
不单独维护审查报告或临时计划文档。初始化风格不改；其余机械检查按实际语义
处理，合法的 chrono duration::count() 类型误报不改成不存在的 size()。

关键行为：

- 手工解除暂停绑定目标指纹和执行 ID；列表刷新保留选择，目标/执行变化撤销
  确认，最终在锁内核对同一 ID，活动上传及执行不能被提前解除。
- SFTP worker 不捕获 QObject。队列、取消、连接状态、通知邮箱和 libssh 租约
  完整共享；有界等待后放弃工作线程时，其状态继续存活，旧代通知不触达析构对象。
- close/chmod 后重查取消；rename 成功不改报取消，失败使用
  `SCRIPT_PUBLICATION_OUTCOME_UNKNOWN`。Provider 身份变化或普通错误通知不能
  抹去已发布事实，是否调用由 Service 重新核对原请求身份。
- 确认通过后、写盘前持久预留目标，成功沿用同一预留进入 invocation。
  取消/撤权/断线后保留任务与锁直至 Provider 回报；确定发布但未调用记录
  `SCRIPT_PUBLISHED_NOT_INVOKED`，结果未知或 Provider 消失保留隔离。
  待回报上传最多 8 项；执行容量检查同时计入这些任务，不能通过反复取消绕过上限。
- 风险策略为 `interactive-risk-v5`，仅严格 ASCII 字面形式可免确认；Zsh 进程
  替换及其他扩展、Unicode 空白、引号和未知 ls 参数不误判 Allow。
  脚本路径不再隐式 trimmed()，风险、确认、写盘及校验使用同一原始路径，拒绝 NUL。
- LocalShell 的可信 coordinator 计入能力发布，命令/脚本独立授权保持不变；
  首次共享提前排一次快照发布，调整执行权限不重复预热。

#### 预算与协议

| 项目 | 当前值 |
| --- | --- |
| 脚本 UTF-8 原文 | 2 MiB |
| 编码后 HTTP JSON 请求 | 12 MiB + 256 KiB；脚本最坏六倍转义 + 有界信封 |
| 编码后响应 | 2 MiB，保持不变 |
| HTTP 共享输入额度 | 49.1875 MiB，涵盖 raw/Qt 解析事务及待投递信封 |
| HTTP 输出队列 | 单请求 4 MiB / 全局 8 MiB，保持不变 |
| 2025 命令 wire 总期限 | 确认 60 s + 请求执行时限（默认 5 s、最大 30 s）+ 5 s 余量 |
| 2025 脚本 wire 总期限 | 再加独立写入 120 s；不扣减批准后的执行预算 |
| 2026 MRTR | 确认通过新 HTTP 请求继续，wire 不重复计入确认等待预算 |

后端仍对确认、写入和执行分别计时；客户端网络截止时间应覆盖总流程。
预算扩展没有取消 socket/请求数、输出、捕获及执行记录的既有限额。

#### 用例与结果

新增 CTest 目标 `novaterm_sftp_session_safety_tests`、
`novaterm_command_safety_tests`，均标记 `mcp;p8;security`、超时 30 s。
Linux 实核默认注册 26 项；Windows 按条件预计 28，尚未运行核对。

| 用例组 | 新增或加强的断言 | 最终专项结果 |
| --- | --- | --- |
| MCP 业务/UI | 多目标确认绑定与替换竞争；首次共享预热；上传/调用互斥、同一预留移交；取消后成功/失败/未知/Provider 销毁；撤权及断线；LocalShell 独立授权能力 | 74 通过、3 条 Windows 平台跳过 |
| Wire | 注入单调时钟：第 55 秒批准后仍有完整执行预算；脚本额外写入预算；到期确实取消；最大转义请求进入后端而超限被拒绝 | 14 通过 |
| HTTP | 满额 content-length/chunked；2 MiB ASCII、六字节转义和 UTF-8 脚本；入队、回收及原有解析/认证拒绝 | 53 通过 |
| Endpoint | 两代协议、能力校验、交换/客户端隔离与断流语义保持原回归 | 28 通过 |
| SFTP 安全 | 暂停 worker 后销毁接收者再恢复；通知丢弃/代际；close/chmod/rename 取消边界；rename 失败结果未知 | 11 通过 |
| 命令/路径安全 | 替换语法与非字面参数；临时文件精确路径；SSH resolver；身份变化不掩盖发布结果；近 2 MiB 纯分类 | 61 通过 |

以上 QTest 计数含数据行及初始化/清理，不是函数数。六项 CTest 最终专项
**6/6，通过耗时 23.49 s**；全部默认目标构建后，全套 **26/26 通过，173.64 s**。

```bash
cmake --build build -j 4
ctest --test-dir build -L mcp --output-on-failure --verbose \
  --output-log build/mcp-fixes-targeted-final-2026-10-16.log
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure --verbose \
  --output-log build/mcp-fixes-full-ctest-2026-10-16.log \
  --output-junit build/mcp-fixes-full-ctest-2026-10-16.xml
```

运行环境为 Linux Debug、Qt 6.8.3、GCC 13.3.0，允许宿主回环/虚拟 PTY；
offscreen 仅用于本次 Linux 全套，不适用于 Windows 的 D3D11 联通测试。
SFTP 专项另以 ASan/UBSan 直接编译运行，**11/11 通过**，
`ASAN_OPTIONS=detect_leaks=0`，不宣称 LeakSanitizer 通过。

I-001 用近 2 MiB 重复前缀与空白进行纯分类测量，约 24～49 ms，未复现怀疑的
病态回溯；保留宽松看门狗及危险后缀检测，未作没有证据的性能重写。
独立复核 worker/发布、授权/事务、wire/HTTP/UI 接线后未发现新的高置信度问题。

边界：SFTP 故障测试为共享状态与可控回调注入，不是延迟真实服务器回包测试；
未执行真实 Shell/脚本、未连接用户 SSH 或串口，未重启现有应用。
Windows/macOS、真实 UART 交互、桌面客户端全流程和整机 GPU 帧性能仍需各自验收。

本批设计变更与实现归入 **0.2.70**。提交前递增版本后，主程序及 MCP 相关目标
重新构建通过，六项 MCP CTest **6/6 复验通过，23.87 s**；日志为
`build/mcp-0.2.70-precommit-2026-10-16.log`。独立审查报告与临时计划不纳入仓库，
设计变更、case 和验证边界统一保留在本文。

### 2026-10-17：终端观察/操作闭环续作

接手基线为 `f067c91`（暂存 MCP 修改），对应设计与实施计划分别在
`docs/superpowers/specs/2026-10-16-mcp-terminal-interaction-design.md`、
`docs/superpowers/plans/2026-10-16-mcp-terminal-loop.md`。本轮沿用当前工作区，
不创建分支、不自动提交；上次提交的测试 RED 过程没有执行账本，不能追认。

#### 已进入源码的接口与边界

- 九工具目录及新版 read_context 参数校验；默认旧读取不带新参数时继续使用原契约。
  显式 compact/full、waitMs/waitFor 或新版游标进入 TerminalInteraction。
  旧游标切换到新版前仍核验原连接签名、Session/epoch、授权及 viewport 绑定；
  新版游标可跨 compact/full 投影使用，不因省略 responseMode 误投旧后端。
- Session 按需拥有 TerminalController，Core 编码批次保留来源/操作 ID。
  用户、协议和 Agent 结果共用有序批次发布；用户抢占会阻止尚未提交的 Agent 结果，
  DSR/DA 协议回复保留 Lease。所有输入继续走唯一 Arbiter/Transport。
- `terminalControl` 和 `useElevation` 独立授权；旧 interactiveCommand 不自动
  升级。观察令牌绑定客户端、Session/epoch、权限、控制/用户/提示符代际和输入序号，
  已接纳操作按 operationId/动作摘要去重。`submitted` 只表示本机提交。
- 最多 32 个等待器、32 个全局 capture/每客户端 4 个，capture 60 秒过期。
  等待最长 30 秒，attention 普通滚屏到期才返回；change 用于显式交互反馈。
  compact 默认 4 KiB/24 行，最终包含 structuredContent/text 双份编码的结果
  限制为 8 KiB，源缺口与投影省略分别报告；补读固定 capture，不推进实时游标或续租。
- 界面增加完整输入、托管提权、凭据与控制状态列、本机接管动作；补齐新文本的中英文。
  sudo/su 凭据按目标身份和认证方法隔离，状态只读取非秘密引用，保存/删除走专属
  工作线程及独立系统凭据连接。修改凭据撤销托管提权，保存不授予执行权限。
  目前 `elevationMethods=[]`，提权请求在读取密码/注入命令前拒绝，无真实认证代填。

#### 发现并修复的回归

| 用例 | 原因与本轮修复 |
| --- | --- |
| `compactExpansionAdvancesForEscapedLongLine` | 单行转义后超过最终 8 KiB，原逻辑删掉整行并重签原游标；现按最终编码结果二分裁剪 UTF-8，断言分页前进、逐页预算和完整拼接 |
| `newInputInvalidatesLegacyCommandReadiness` | 新 Controller 只清自己的提示符，旧 Coordinator 仍认为空闲；非空输入实际提交后同步废弃旧证据 |
| `queuedUserInputPreemptsLaterAgentBatch` | 同批次 Agent 独立回调越过尚未发布的用户输入，实测写出 au；统一有序发布后只写用户 u |
| `maximumTerminalCommandIncludesConfiguredLineEnding` / Core 编码用例 | 协议允许 16 KiB 文本，但设备行尾/字符按键被计入同一额度；文本/粘贴仍限 16 KiB，行尾和按键单独纳入 64 KiB 总量，Core 原有超长文本拒绝断言保持有效 |
| `emptyAgentBatchPreservesCommandReadiness` | 零字节操作清除提示符，远端不可能返回新证据；空输入/接纳失败恢复原提示符 |
| `compactExpansionRejectsTooSmallUtf8Budget` | maxBytes 小于下个字符会成功返回同一令牌；现明确返回 TEXT_BUDGET_TOO_SMALL，可增大预算重试 |
| `compactCursorSupportsFullProjection` | compact/full 路由及旧/新签名不一致；校验后迁移旧游标，并按新版游标类型保持正确路由 |
| `compactExpansionPreservesSourceGap` | 补读漏掉 capture 已有缺口；所有补读保留 sourceGap/sourceTruncated/resetRequired |

上述失败均在本轮观察到 RED 后修复再验证。首轮全套还抓到了将 Core 文本额度误放宽
至 64 KiB 的实现错误，已恢复文本 16 KiB + 总量 64 KiB 的双重约束并保留越界断言。
`settingsDialogRoundTripsTerminalPermissions` 另验证真实界面的新增复选框往返：
开启完整输入同时开启读取、托管提权独立授权、关闭输入撤销提权、关闭读取级联撤权。

#### 审查与未验收范围

确定性 Qt/C++ 扫描加六方向只读审查覆盖本次提交及续作差异，涵盖模型契约、
所有权、线程、API、输入校验和性能。确认问题经上述用例修复；初始化风格、
合法 chrono::count、静态正则等机械提示不当作运行缺陷处理。

仍保留调查项：高频 activityChanged 是否造成重复排队捕获；QHash 每客户端
capture 淘汰的任意顺序；凭据更新成功但共享 revision 保存失败时的跨实例撤权。
后者须在开放真实认证适配器前补两实例故障验收，当前没有可消费该秘密的 MCP 路径。

完整设计尚有未收口目标：严格的基础快照 4 Hz 合并及性能计数/事件延迟，独立的
最近终态保留期限、自由 text/key 触发命令记录，以及更细的序号错误/接纳动作数。
现有有界控制与保守拒绝不等于这些目标已完成。不得宣称真实 token 节省比例或
GUI 帧性能通过；Linux 自动化也不替代 Windows/macOS、真实 UART、桌面 MCP Host、
真实 SSH/TUI 和可信本地/SSH sudo/su 认证验收。核心去 Qt 化仍未收口，暂停的搜索
匹配器注入没有恢复。

本轮构建与 CTest 命令：

```bash
cmake --build build -j 4
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure \
  --output-log build/mcp-terminal-loop-full-final-2026-10-17.log \
  --output-junit build/mcp-terminal-loop-full-final-2026-10-17.xml
```

Linux Debug、Qt 6.8.3、GCC 13.3.0，需允许本机回环、虚拟 PTY 和当前会话总线。
未放开沙箱时 HTTP 监听失败，不能将这批结果算作代码回归或通过；所有最终成绩
以放开所需本机资源后的日志为准。当前 `ctest -N` 实核 27 项，Windows 按条件预计
29 项，尚未在本轮运行。未连接用户服务器/真实串口、未重启现有应用。

最终主程序及全部默认目标构建通过，中英文 qm 生成通过，`git diff --check` 通过。
Linux Debug 全套 **27/27 通过，178.00 秒**；其中 MCP 七项 **7/7，26.95 秒**。
业务测试 85 通过/3 条 Windows 平台跳过，Core 88 通过，Session 74 通过/2 跳过，
计数含初始化/清理与数据行。Session 跳过仍为 Windows Clink 和需显式开启的
虚拟串口 lrzsz 大矩阵，不视为真实 UART 验收。本次 PTY/UI 本机测试通过，不清除
AGENTS 中其他机器的历史失败记录。上述最终日志包含满额文本加 Ctrl+C、设备行尾
和界面授权往返用例，不能用更早的专项或首轮全套失败日志替代。

#### 同日追加：只读审查 D-001～D-003 修复

后续针对未提交差异的 qt-cpp-review 确认三项 P2，本轮均先增加失败断言，再修改实现：

| 问题 | 实现与回归 |
| --- | --- |
| D-001：空输入回调恢复过期提示符 | 保存提示符时绑定独立证据 revision；用户输入、设备输出、reset/帧错误、候选或新提示符推进证据代际。只恢复未被替换/失效的证据。`emptyAgentBatchKeepsLatestPromptEvidence` 五行覆盖 shell-reset、framing-error、device-output、新提示符替换与首次提示符，旧实现五行均失败 |
| D-002：full 省略显式请求的未变化屏幕 | full 默认提供屏幕，显式 includeViewport=true 不受 modelRevision 是否变化限制；compact 默认仍省略未变化屏幕。`compactCursorSupportsFullProjection` 现断言实际屏幕文字，覆盖 compact→full、默认 full、旧游标迁移和 compact 显式取屏 |
| D-003：full 截断后错误推进游标 | full 从有界源的头部裁剪，行数/字节预算截断设置 resetRequired=true 并返回 nextToken=null；源摘要截断同样不发游标。`fullProjectionTruncationRequiresReset` 两行分别验证行/字节预算，并对照 compact 仍保留正常游标和补读、不强制 reset |

定向 RED：Session 五行失败；MCP 屏幕内容一条、full 截断两条失败。
实现后上述用例及原空输入 readiness、非空输入失效旧 Coordinator、compact 补读/
转义长行等定向回归通过。该批改动限于 Session/MCP，按 AGENTS 运行 MCP 七项与
Session 八个相关 CTest，不因局部修复重复全套。之前的 27/27 为上一轮全套成绩。

```bash
cmake --build build --target NovaTerm novaterm_session_tests novaterm_mcp_tests \
  novaterm_mcp_wire_tests novaterm_mcp_http_tests novaterm_mcp_endpoint_tests \
  novaterm_mcp_access_interaction_tests novaterm_sftp_session_safety_tests \
  novaterm_command_safety_tests -j 4
QT_QPA_PLATFORM=offscreen ctest --test-dir build -L 'mcp|p6' --output-on-failure \
  --output-log build/mcp-review-fixes-targeted-2026-10-17.log \
  --output-junit build/mcp-review-fixes-targeted-2026-10-17.xml
```

最终主程序及以上相关目标构建通过，专项 **8/8 通过，41.08 秒**，其中 MCP
七项 27.16 秒、Session 13.91 秒。业务 QTest 87 通过/3 条 Windows 跳过，Session
79 通过/2 条既有平台或手工矩阵跳过，计数含初始化/清理和数据行。
`git diff --check` 通过；本轮未重跑全套，未连接用户真实设备，改动仍未提交。

#### 同日追加：用户 core 的 gdb 定位与权限拒绝崩溃修复

用户提供 `build/Debug/bin/core`，生成于 2026-10-17 12:21:26（Asia/Shanghai），
对应 12:20:10 构建的 `build/Debug/bin/novaterm`。gdb 确认 SIGSEGV 在主线程，
原索引为 row=0、column=7（允许托管提权），`PermissionCellDelegate::editorEvent`
原第 51 行调用 model->setData，Qt 内部访问了已失效的节点指针。

根因：串口/Telnet 不能托管提权。点击该列时旧 setGrant 成功，新 setTerminalGrant
拒绝，itemChanged 错误分支同步调用 refresh() → clear() 删除当前树节点；
而 QTreeWidgetItem::setData 在 dataChanged/itemChanged 发射返回后还会访问节点。
先前的权限往返只覆盖本地 Shell 授权成功，没有覆盖这条拒绝分支。

增加 `settingsDialogRejectsElevationWithoutResettingActiveModel` 的 serial/telnet
数据行，使用真实 PermissionCellDelegate/model 的鼠标释放入口及临时假传输。
gdb 先稳定复现与用户 core 相同的 Qt 崩溃地址偏移；再在错误分支断点确认
column=7、grantSaved=true，继续执行即 SIGSEGV，排除 Parser/串口 I/O 线程原因。

修复仅将 itemChanged 的失败刷新/错误展示排入带 QObject 上下文的 queued 回调，
不捕获 item/index；等当前模型写入返回后再重建，窗口销毁后回调自动失效。
权限拒绝仍即时生效。回归断言：delegate 返回前持久索引有效且无 modelReset，
排队刷新后原索引失效、失败勾选撤回，原终端输入授权保留且未取得提权能力。
修复后 gdb 下两行均通过，inferior 正常退出。

原始 core 保留；为后续取证，覆盖程序前另外保留匹配可执行文件为
`build/Debug/bin/novaterm.core-20261017`。分析该旧 core 时应使用备份：

```bash
gdb build/Debug/bin/novaterm.core-20261017 build/Debug/bin/core
```

只构建程序和运行隔离测试，不自动启动用户应用或连接真实串口/SSH。

最终 `build/bin/novaterm` 与用户实际使用的 `build/Debug/bin/novaterm` 均重建成功。
`ctest --test-dir build/Debug -L mcp` **7/7 通过，26.95 秒**；业务 QTest
89 通过/3 条 Windows 平台跳过，含新增拒绝路径两行。日志为
`build/mcp-ui-core-2026-10-17.log`，JUnit 为同名 `.xml`。gdb 定向两行 4/4
（含初始化/清理）、inferior 正常退出，`git diff --check` 通过。
本轮 UI 局部修复未重跑全套；core 的原始修改时间和大小保持不变，未自动提交。

同日表格宽度调整：会话列改为字体度量的紧凑初始宽度并允许拖动，状态/权限/凭据
列继续按内容适配，当前操作列单独拉伸填充剩余空间，避免会话列大面积留白。
`build` 与 `build/Debug` 主程序均重建；现有权限点击、授权往返、拒绝回退、展开
状态保持与本地诊断界面五组用例通过（含数据行及初始化/清理 8 通过），并检查
offscreen 界面预览。纯列宽调整未重复全套或新增常量型测试，原权限语义不变。

#### 同日追加：提权勾选回退与复选框颜色

用户确认“允许托管提权”勾选无效/自动回退。此前虽已避免拒绝路径崩溃，但界面
仍将不支持的条件画成可勾选：串口/Telnet，以及尚未授权终端输入的本地连接，
都先修改模型，再由后端拒绝/清除并重建。新增
`AccessStore::elevationGrantDisabledReason()` 是本机 UI 的非秘密条件查询，按
连接类型、运行代际、输入授权、凭据更新和已验证目标身份返回原因，不访问密码库。
它只表示能否新授予权限，不表示认证适配器已经可用。

UI 在第 7 列保存可编辑性并显示“不支持”“需执行权限”“未连接”“身份未验证”
或“凭据更新中”，tooltip 给出完整原因。委托按列忽略不允许的编辑事件，保留其余
读取/输入权限可编辑；已有有效提权授权始终允许取消。后端再次验证和 queued
错误回退仍保留，防止程序直接改模型或条件变化时失守。先前 core 的回归现直接写
model 绕过 UI 禁用，继续验证拒绝时不会删除 Qt 尚在访问的节点。

用户另要求复选框明确区分颜色。仅替换 MCP 表格权限列的 indicator 绘制，保留
Ela 行背景/文字/布局：可编辑未选为主题蓝/青色边框，已选/半选为强调色填充并绘制
勾/横线，不可编辑为灰底灰框；勾的前景按底色明暗选择黑/白。实时主题变更触发
viewport 重绘，不改全局 Ela 样式。说明文字不只依赖颜色传达禁用原因。

`settingsDialogElevationCheckboxRequiresSupportedContext` 四行涵盖 serial、telnet、
local-without-input、local-with-input：原实现前三行 RED（错误接受点击），后者通过；
修复后全部 GREEN，禁用点击不会撤销已有读取/输入或触发模型重建，本地取得输入
权限后可以正常勾选。深浅主题均通过隔离 offscreen 预览检查，未新增颜色常量断言。

`build` 与 `build/Debug` 主程序及中英文 qm 均重建成功；Debug MCP 专项 **7/7，
27.79 秒**，业务 QTest 93 通过/3 条 Windows 跳过，含原权限往返与崩溃拒绝回归。
日志为 `build/mcp-checkbox-availability-2026-10-17.log` 及同名 XML。
本批仅改 MCP/UI，按相关目标验证，未重跑全套或真实设备认证。真实 sudo/su 适配器
仍不可用，没有通过放宽该条件或向终端代填密码来“修好”勾选。改动未自动提交。

#### 所有终端基础闭环与剩余本地状态收口（2026-10-17）

用户明确本次 MCP 重构应适用于所有终端。诊断扩展文档
原诊断扩展 v1.1 草案（现整合于 `docs/architecture/stages/P8_AI_MCP_Interface/00_Overall_Architecture.md`）：九工具基线、连接/目标环境/
执行能力分离、共享终端与独立受控执行双路由、离线 Capture 授权、未开启录制时的
任务证据、原始字节/搜索投影映射，以及任务/磁盘/全局资源边界。它是后续扩展草案，
不是已实现的 History/Diagnostic Pack，也不替代已有完整输入授权与同终端语义。

本批进入源码：

- LocalShell/SSH/Serial/Telnet/Custom 使用同一 text/key/paste/line_ending 闭环，
  Custom 不再因类型被排除。显式 Shell kind/提示符 Profile 跨传输解析，不发送探测。
  类型简写 attach 跨类型时清除旧 transport 配置；显式 RuntimeConfig 仍用新快照。
- 新观察后端每会话复用至少 250 ms 的 Provider 基础快照，高频事件唤醒采用一个
  single-shot timer 合并；控制/完成/撤权独立于屏幕采样节流。旧默认读取契约不变。
  capturedAt 如实反映屏幕时间，状态可能比复用屏幕新，不能假设一次完成反馈含全部输出。
- 每客户端四个、全局 32 个 capture 按单调接纳序号最旧优先淘汰，下一次捕获回收已
  过期条目；不再使用 QHash 遍历顺序。补读期限与授权绑定保持不变。
- 当前 command 与最近完成 lastCommand 分离，后者最多保留 10 分钟；新文本不复用
  旧 finished 状态，最近记录到期也不会清除另一条活动命令。自由输入仅预留关联，
  有匹配集成事件后才建立记录，不靠输入换行/静默推断成功。
- operation 公开协议动作接纳数；command 内部回车不额外计数，release 返回自身 ID/
  一项接纳且不发字节。有效上下文中的旧/超前序号及令牌到期有独立拒绝码，拒绝不重放。
- 提权格按授权、连接、凭据更新和可信目标身份判定，不再写“串口永远不支持”；
  当前串口/Telnet/Custom 没有可信身份适配器，仍拒绝凭据绑定和提权授权。

独立单席只读复核确认两处输入顺序缺陷，本批均新增 RED 再修复：

| 回归 | 修复 |
| --- | --- |
| Enter 后有文字、空行后有文字、命令后有尾部、多条已提交行（四行） | 按动作顺序记录有界待返回提示符与未提交尾部；首个提示符不能证明整批就绪，不给残留文字追加 command；多行不虚构单命令结果 |
| 编码期间到达集成提示符、设备就绪或设备候选（三行） | 非空字节真正交给 Arbiter 前使旧 Controller/Coordinator 提示符失效；空输入和失败路径仍按证据 revision 保留更新的提示符 |

全量 Session 还捕获了类型简写 attach 跨类型残留旧规则的问题，现从配置来源修正，
没有退回按类型屏蔽 Profile。五类基础输入、显式 POSIX 收尾、FIFO 补读、4 Hz 采样、
终态回收、序号/release、提权不可用原因及原 core/UI 回归均进入自动化测试。

验证：`cmake --build build/Debug -j 4` 成功，实际主程序为
`build/Debug/bin/novaterm`。最终全套：

```bash
QT_QPA_PLATFORM=offscreen ctest --test-dir build/Debug --output-on-failure \
  --output-log /home/super/Documents/qt_app/NovaTerm/build/mcp-final-continuation-full-2026-10-17.log \
  --output-junit /home/super/Documents/qt_app/NovaTerm/build/mcp-final-continuation-full-2026-10-17.xml
```

Linux Debug **27/27，181.21 秒**；MCP **7/7，29.05 秒**，Session 15.31 秒。
此前专项 MCP7/7 但 Session 换绑失败的运行不当成绿灯；最终全量已包含该修复。
这些是功能/结构回归，不是 CPU、吞吐、token 节省比例或 GUI 帧性能验收。

仍未验收/未实现：真实本地/远端可信 sudo/su 适配器及配对部署、跨实例凭据更新失败
后的撤权故障注入、真实 UART/SSH/TUI 与 Windows/macOS、性能和关键事件延迟基线。
无法确定复杂行编辑/多行执行边界时保守保持未知，可继续自由交互，不强行宣称就绪。
诊断 M1–M6、历史落盘与 Provider 包是独立后续工作；暂停的核心去 Qt/搜索重构未恢复。
原 core 与匹配旧可执行文件保留，未启动真实应用或连接用户设备，改动未自动提交。

#### 接入界面的授权选择与执行就绪分离（2026-10-17）

用户截图反馈串口无法勾选固定诊断/托管提权，并要求检查其他类型。定位到两处
职责混用：UI 只从当前 Profile 构造授权目录，后端又在执行器不可用时丢弃模板授权；
提权则用可信身份条件禁止保存授权。它们属于执行前置条件，不等于用户不能配置权限。

- 固定诊断始终展示四个已知授权模板；AccessStore 仅保留已知 ID，不因暂缺后端清空。
  执行仍复核 Profile、Executor、目标身份和一次性票据，缺后端不自动探测/发送命令。
- 已有终端输入授权后，所有类型可保存 useElevation；未连接、无输入权限或凭据更新
  才阻止新选择。身份未知/无适配器改为独立状态说明，勾选不取密、不启用适配器。
  凭据绑定仍要求可信身份，Serial/Telnet/Custom 不能按端口名绑定密码。
- MCP permissions.useElevation 是授权；availability.elevationAvailable 当前始终为
  false，elevationDisabledReason 区分身份未知/适配器不可用，elevationMethods 仍为空。
- 兼顾单项拒绝时的旧 core 防护：模型 setData 栈内不清树。独立复核另发现旧行仅存
  sessionId 的竞态；重连后取消提权会把旧行中勾选的输入权复制给新连接。五类终端均
  已复现 RED，现将根行绑定 clientId/attachment/epoch，在任何权限回写前拒绝过期行，
  排队刷新而不在信号栈内删除节点。

七组授权场景覆盖 LocalShell/SSH 的正常与缺后端、Serial/Telnet/Custom：选择可保留，
未就绪不发票据、不读/写凭据、不发送终端字节；读取撤销级联清除。五组旧行竞态覆盖
全部连接类型。深浅主题 offscreen 预览检查通过，实际 Debug 主程序已重建。

这次修复不新增诊断执行后端或真实提权适配器，也不等于 MCP 架构已收口。
当前 McpService 调度、UI 整组授权回写、新旧执行路径和状态/缓存职责仍较集中；
后续应先分离授权业务与会话权限模型，再拆观察/执行服务，保留九工具协议回归，
不把已有大函数仅套入新 Registry 后宣称完成职责重构。

#### 运行期内核重构 W1：契约与解码边界（2026-10-18）

按 `docs/architecture/stages/P8_AI_MCP_Interface/00_Overall_Architecture.md` 与
`docs/architecture/stages/P8_AI_MCP_Interface/P0_Runtime_Consolidation.md` 开始逐用例替换
遗留 stdio 业务结构。W1 只改 MCP 内部类型与边界，九工具名称、Schema、
错误码、HTTP/认证与 2025/2026 报文形状不变。

- 新增 `src/mcp/contracts/`：强类型 `ClientId`/`PeerId`/`RequestId`/`TaskId`/
  `SessionId`/`AttachmentId`/`Epoch`/`OperationId`/`CaptureId`/`DiagnosticId`/
  `ExecutionId` 与带标签计数器；`ErrorCode` 到对外文本的唯一映射、`Error`/
  `RetryHint`/`Result<T>`；`SessionReference`、`SessionBinding`、
  `AuthorizationContext`；`Budget` 预算表；九工具请求与六种终端动作
  variant（`ToolRequest`）。`McpProtocol.h` 的旧常量改为 `Budget` 别名。
- 新增 `src/mcp/protocol/ToolCodec`：九工具目录、逐字段校验从
  `McpProtocol.cpp` 原样迁入；`decode()` 在 wire 校验后产生强类型请求，
  `encodeFailure()` 复现原失败封装。`McpProtocol` 不再包含 Schema 或参数校验。
- `McpService::receive` 在排队前完成解码；作业保存 `ToolRequest`。
  `list_sessions`、`execute_command` 已改为消费强类型字段；会话身份、
  execute 路由、撤销/失效匹配统一经类型化请求读取。`run_command`/
  `run_script`/`read_context`/`search_context`/交互两类仍暂读原始
  `Job::arguments`（迁移期私有接缝，逐工具迁移时删除）。
- 目录兼容基线：`tests/mcp/fixtures/tool-catalog-2026-10-18.json` 由**重构前**
  `tools()` 直接导出（143 KiB）；新增 `novaterm_mcp_contracts_tests` 做对象级
  比较，并覆盖参数接受/拒绝文本、可选字段存在性、动作解码、错误编码与 ID 解析。

本轮 Linux Debug 构建通过，MCP 八项 8/8 通过（29.72 秒，含新增契约项）；
全套 28 项未重跑，不能写成 28/28。Windows/macOS 未运行。W2（授权事务与
单项授权修改）、W3（统一观察）、W4（任务/确认/执行与 Session 证据）、
W5（类型化 wire 边界与组合根收缩）仍待实施；结构验收尚未完成。

#### 运行期内核重构 W2a：单项授权事务（2026-10-18）

W1 建立的 `contracts`/`ToolCodec` 边界之上，授权写入从“整组位置布尔回写”改为
带连接绑定和预期行版本的单项事务：

- `src/mcp/contracts/Permissions.h`：`PermissionKind`（读取/诊断/风险命令/
  脚本/完整输入/提权）、`PermissionSelection`、`GrantSnapshot`、
  `PermissionChange` 与 `AuthorizationRejection`。`Result<T, E>` 泛化以容纳
  应用层拒绝类型，协议 `Error` 仍是默认实参。
- `src/mcp/application/GrantStore`：运行期授权行唯一存储，按
  (ClientId, SessionId) 索引，每行保存 `SessionBinding`、选择与行级
  `GrantRevision`。
- `src/mcp/application/AuthorizationService`：唯一 `change()`。固定顺序校验
  启用状态 → 客户端 → 会话绑定 → 预期行版本 → 权限不变量；级联规则集中：
  取消读取清除全部执行类授权，开启完整输入同时选择读取，取消完整输入撤销
  提权，提权要求完整输入且凭据变更空闲，未知诊断拒绝。拒绝返回当前
  `GrantSnapshot`，不覆盖旧整组状态。
- 重连语义：同一 attachment 的普通重连保留读取，快照隐藏并要求重新授权
  诊断/脚本/交互/完整输入/提权；旧 UI 行携带旧绑定，提交必被
  `StaleBinding` 拒绝，撤销意图不会落到新连接。显式读取修改会在新绑定上
  重绑定并清除执行类权限。
- `AccessStore` 删除 `setGrant`/`setTerminalGrant` 整组入口、公开 `Grant`
  结构与 `confirmedCommand` 死字段/`canRunConfirmedCommand`；读取谓词改走
  `GrantStore`，每次有效修改推进行版本与客户端版本并发 `revoked`/`changed`。
- `McpSettingsDialog` 每列/每个诊断只提交一项 `changePermission`，成功后
  同步回写行版本；根诊断复选框按单项事务逐条提交；服务端拒绝排队刷新，
  仍在 `itemChanged` 栈内不删除节点。
- 新增 `novaterm_mcp_authorization_tests`（纯应用层假端口）覆盖绑定/版本
  拒绝、读取撤权级联、完整输入隐含读取、提权前置、未知诊断、重连语义与
  版本推进。测试侧的整组搭建辅助在 `tests/mcp/TestGrantSupport.h`，不进入
  产品库。

验证：Linux Debug 构建通过，MCP 九项 9/9（28.71 秒）；全套 29 项未重跑，
Windows/macOS 未运行。仍未做：`ClientRegistry` 从 `AccessStore` 分出、
`CapabilityResolver`、`src/ui/models/SessionPermissionModel` 与对话框
view-model 化（W2b）；W3–W5 未开始。

#### 运行期内核重构 W2b：客户端登记、能力解析与界面模型（2026-10-18）

W2a 的单项授权事务之上，完成 W2 的结构收口：

- `src/mcp/security/ClientRegistry`：客户端身份、令牌摘要、客户端级版本与
  推进操作从 `AccessStore` 分出；`AccessStore` 保留组合、持久化与凭据
  边界，`clients()/addClient()/removeClient()/rotateToken()/version()` 转发。
  重载仍保持原有版本推进与失效语义。
- `src/mcp/application/CapabilityResolver`：授权快照 + `CapabilityFacts`
  → 能力数组、允许的读取/输入/提权投影、终端输入不可用原因、诊断就绪、
  提权授权是否可勾选与执行不可用原因。纯函数，不探测、不授予。
  `src/mcp/CapabilityAdapter` 是 Session 事实（Profile/执行器/仲裁器/控制器/
  目标身份）到该模型的唯一转换点。
- `listSessions` 的能力数组与 `TerminalInteraction::capabilities` 的
  permissions/availability/shellIntegration 都改为消费同一解析结果；
  对话框的后端就绪与提权原因也走同一入口，不再各自复制连接类型规则。
  `AccessStore::elevationGrantDisabledReason` /
  `elevationExecutionUnavailableReason` 改为委托解析器，保留既有调用点。
- `src/ui/models/SessionPermissionModel`：稳定 `SessionRowKey`（含
  attachment/epoch/generation/指纹）、只读行快照、诊断子行与具名列枚举
  （Session/State/ReadOutput/Diagnostics/Script/Interactive/TerminalInput/
  Elevation/Credentials/Operation）。`SettingsController` 装配快照并提交
  单项意图：先按 RowKey 重验当前绑定，过期行返回 `Stale`；持有 `Service`
  弱引用，不缓存 Session 指针。
- `McpSettingsDialog` 只连接控件、模型与控制器：授权事务、级联回写、
  行版本、能力/原因均来自控制器；不再直接取得可变 `AccessStore`，
  客户端/启停/凭据入口经控制器转发；委托与处理函数使用 `Column` 枚举，
  不再用 3/6/7 表示业务权限。测试侧整组搭建辅助
  `tests/mcp/TestGrantSupport.h` 不进入产品库。

验证：Linux Debug 构建通过，MCP 九项 **9/9，28.77 秒**；`ctest -N` 注册
29 项（全套未重跑）。新增能力解析单测覆盖能力随授权/事实变化、输入原因
优先级、提权授权与执行分离、诊断就绪条件。Windows/macOS 未运行；W3 统一
观察、W4 任务/确认/执行与 Session 证据、W5 类型化 wire 与组合根收缩未开始。

#### 运行期内核重构 W3a：统一观察存储（2026-10-18）

W2 的授权/能力/界面收敛之后，旧读取与 compact 交互观察不再各自维护采样缓存和
capture 表：

- `src/mcp/application/SnapshotStore.h`：每 Session 一个基础摘要缓存，注入单调
  时钟，250 ms 合并窗口；键含 epoch，换绑后不复用；加载失败不缓存。
- `src/mcp/application/CaptureStore`：连接作用域与客户端作用域共用物理存储、
  分别计额（每连接 4、每客户端 4、客户端作用域全局 32），FIFO 序号淘汰，
  支持按 session/connection/client 过滤删除与到期 sweep。记录保存绑定、
  grant 版本、revision、有界行投影、缺口与到期。
- `src/mcp/ObservationService`：唯一拥有采样与 capture 存储，由 `McpService`
  创建并传给 `TerminalInteraction`；旧 `read_context/search_context` 与
  compact/等待/展开共享同一数据源。
- 删除 `McpService` 的 `Capture` 结构与 `captures/lastCapture/lastRevision/
  lastBase`；删除 `TerminalInteraction` 的 `Capture/BaseSnapshot` 结构与
  `captures/baseSnapshots/captureSequence`。旧 search 改为与 compact 搜索相同
  的 source/lineIndex 语义，仍只搜索当次返回的投影。
- 观察预算集中到 `contracts/Budget`（compact 4 KiB/24 行、最终 8 KiB、
  full 默认 64 KiB/256 行），不再散落字面量。

验证：Linux Debug 构建通过，MCP 九项 **9/9，28.90 秒**；新增
`CaptureStore` 配额/作用域/到期与 `SnapshotStore` 合并/epoch/失败不缓存单测
（并入 `novaterm_mcp_authorization_tests`）。全套 29 项未重跑，Windows/macOS
未运行。W3b（ObservationService 承接读取/等待用例、`ProtocolProjectionSizer`、
typed Claim 转换）与 W4/W5 未开始。

#### 运行期内核重构 W3b：读取、搜索与投影收敛（2026-10-18）

在 W3a 的统一存储之上，兼容读取与搜索用例不再留在 `McpService` 的大方法里：

- `ObservationService::read`：合并窗、显式 `FreshnessPolicy`（严格
  `StrictRevision` 与发布快照的 `BoundedStaleness`）、UTF-8 预算裁剪、连接
  作用域 capture 记录与观察统计都在服务内；`McpService::readContext` 只剩
  cursor token 校验、`nextToken` 签发与 JSON 封装，原方法体约 130 行缩减到
  约 70 行。
- `application/CaptureSearch`：统一旧读取 worker 与 compact 搜索的匹配算法
  （UTF-8 字节偏移、按 source 独立行号、4 KiB 中断检查与 query 长度重叠）；
  取消与超时仍由调用方区分，旧读取保留 100 ms 预算。
- `protocol/ProtocolProjectionSizer`：以最终 `tool result` 的紧凑编码大小作为
  compact 投影反馈，TI 的 `fit()` 不再自己拼装序列化测量；预算常量继续来自
  `contracts/Budget`。
- 观察统计（复用/合并/发布/投影字节/capture 与 snapshot 年龄 P95）移入
  `ObservationService`，`Service::statistics()` 读取同一口径。

验证：Linux Debug 构建通过，MCP 九项 **9/9，28.44 秒**；新增统一搜索单测
（作用域、limit、大小写、4 KiB 边界、中断）并入
`novaterm_mcp_authorization_tests`。全套 29 项未重跑，Windows/macOS 未运行。

仍未做：等待/交互观察仍在 `TerminalInteraction`（随 W4 的 Session 证据与
Arbiter 迁移）；旧→新 cursor/capture 的 typed Claim 转换归入 W5；W4 任务/
确认/执行与 Session automation、W5 类型化 wire 与组合根收缩尚未开始。

#### 运行期内核重构 W4a：Session automation 目标与确认事务存储（2026-10-18）

- 新增 `novaterm_session_automation` 静态库：`SessionDirectory`、
  `ISessionCommandExecutor`、`CommandPlatformProfile`、
  `LocalSessionCommandExecutor`、`SessionCommandFacade`、
  `SshSessionCommandExecutor` 从 `novaterm_mcp_runtime` 迁出。runtime 只保留
  MCP 组合、适配与 HTTP 接线，PUBLIC 链接该目标；主程序 GLOB 排除和测试
  源清单无需新增重复编译。
- 新增 `src/mcp/tasks/ConfirmationRegistry`：2025 一次性 nonce 生命周期与
  待确认记录的唯一存储。记录包含连接/客户端/会话/epoch、命令或脚本描述、
  Profile/策略版本、payload 摘要、grant/提示符/用户代际与过期；不持
  Job/Session 指针，`McpService` 按 `jobKey` 解析当前 Job。
- `McpService` 删除 `PendingConfirmation`、`ConfirmationScope` 与
  `confirmations`、`activeConfirmations`、`consumedConfirmations`、
  `confirmationExpiries`、`activeConfirmationScopes` 五张平行表；注册/消费/
  断连/撤权/过期统一走注册表，`cleanup()` 只对返回的过期记录完成作业。
- 确认语义保持：2025 表单接受/拒绝/取消、2026 MRTR 重提与过期、连接绑定
  与摘要/版本复核、断开不取消远端任务（未知结果保护不变）。

验证：Linux Debug 构建通过，MCP 九项 **9/9，28.56 秒**；新增确认注册表
单测（active/consume/takeConnection/removeScope/过期 pending/empty）并入
`novaterm_mcp_authorization_tests`。全套 29 项未重跑，Windows/macOS 未运行。

仍未做：RequestScheduler、ExecutionService/TaskRegistry、move-only
TargetReservation、`src/session/automation/TerminalEvidenceModel` 与
Coordinator/Controller 提示符真源收敛（W4b）；W5 类型化 wire 与组合根收缩。

#### 运行期内核重构 W4b：请求调度独立（2026-10-18）

- 新增 `src/mcp/application/RequestScheduler`：每 peer 2 活动/4 排队，FIFO
  交付；取消的排队请求不再投递，已投递请求占用名额直到 `complete()`；
  `removePeer` 回收连接槽位。它只判断请求能否开始，不判断远端执行是否结束。
- `McpService` 删除 `Connection::active` 与 `waiting` 队列以及 `schedule()`
  里手写的出队/计数逻辑；接收、完成、取消、断开统一走
  `enqueue/takeReady/complete/cancel/removePeer`。`QTimer::singleShot(0)`
  仍负责跳出同步调用栈，`hasReady()` 只用于补齐空出的并发名额。
- 预算仍来自 `contracts/Budget`（`MaxActiveRequests`/`MaxQueuedRequests`），
  队列满的 `BUSY` 重试语义与每连接隔离不变。

验证：Linux Debug 构建通过，MCP 九项 **9/9，29.11 秒**；新增调度器单测
（配额、FIFO、活动名额、取消、跨 peer 隔离、removePeer）并入
`novaterm_mcp_authorization_tests`。全套 29 项未重跑，Windows/macOS 未运行。

仍未做：`ExecutionService`/`TaskRegistry`（任务唯一拥有与阶段）、move-only
`TargetReservation`、`src/session/automation/TerminalEvidenceModel` 与
Coordinator/Controller 提示符真源收敛（W4c）；W5 类型化 wire 与组合根收缩。

#### 运行期内核重构 W4c：执行记录索引（2026-10-18）

- 新增 `src/mcp/tasks/ExecutionRegistry`：执行记录按执行 ID 与 Executor
  请求 ID 索引；记录绑定、策略/目标、Executor 句柄、所属作业键与终态载荷，
  不复制 `Job` 的可变状态。完成记录的保留期淘汰集中在
  `expireCompleted(now, retention)`。
- `McpService` 删除私有 `Execution` 结构与 `QHash<QString, Execution>
  executions`：停止、取消、超时、脚本发布结果、命令任务创建/查询、
  `executionRecords()` 与目标确认都经注册表；`finishExecution` 通过
  `jobKey` 解析仍存活的 Job，而不是持有 `shared_ptr<Job>`。
- 语义保持：未知结果不清除 quarantine、已发布脚本记录保留、迟到的确定退出
  只解除保护不重复响应、完成记录 10 分钟淘汰、每连接 32 条与总量 128 上限。

验证：Linux Debug 构建通过，MCP 九项 **9/9，28.68 秒**；执行/取消/超时/
未知结果/脚本发布/目标保护的既有用例全部通过。全套 29 项未重跑，
Windows/macOS 未运行。

仍未做：`ExecutionService` 的显式阶段划分、move-only
`TargetReservation`、`src/session/automation/TerminalEvidenceModel` 与
Coordinator/Controller 提示符真源收敛；W5 类型化 wire 与组合根收缩。

#### 运行期内核重构 W4c 续：目标预留 owner（2026-10-18）

- 新增 `src/mcp/tasks/TargetReservation`：持久标记的 move-only 所有者。
  `acquire()` 先写标记再返回；析构**不释放**标记，未知结果保留 quarantine；
  只有确定未开始或已确认终止时显式 `release()`；移交给执行记录时放弃所有权，
  由执行终态按 target/executionId 解除。
- `McpService` 删除 `Job::scriptReservationId`/`scriptReservedTarget` 两个
  平行字符串字段与手写 `guards.reserve/release` 配对；脚本上传、命令预留、
  上传预留移交、取消/提供者消失/完成等路径统一经 owner。脚本发布结果记录的
  executionId 从 owner 读取，标记写入仍严格先于有副作用操作。

验证：Linux Debug 构建通过，MCP 九项 **9/9，28.51 秒**；脚本上传/发布/取消/
未知结果、命令预留/移交/quarantine、重启后标记与人工确认等既有用例全部通过。
全套 29 项未重跑，Windows/macOS 未运行。

仍未做：`ExecutionService` 的显式阶段划分；`src/session/automation/
TerminalEvidenceModel` 与 Coordinator/Controller 提示符真源收敛；W5 类型化
wire 与组合根收缩。

#### 运行期内核重构 W5a：类型化 backend 事件（2026-10-18）

- 新增 `src/mcp/protocol/BackendEvents.h`：`BackendCall`（id/tool/arguments/
  MRTR 元数据）、`BackendCancel`、`BackendElicitationResult` 与
  `BackendEvent = std::variant<...>`。工具参数已在 wire 侧通过 Schema 校验，
  事件只携带已接受的值。
- `WireSession` 删除内部 JSON `op` 总线，改发 `backendEvent`；
  `HttpMcpEndpoint::Backend::receive` 与 peer/route 绑定改为 typed。
  Endpoint 仍按 call/cancel 维护在途作业索引，业务 Service 不再读取
  `op/tool/arguments/mrtr` 字符串。
- `McpService::receive` 改为 variant 分派：确认结果、取消、工具调用各走
  独立分支；Schema 解码与业务拒绝仍在应用层复核。响应方向
  （`input_required`/`result`/`elicitation`）仍由 WireSession 的 JSON-RPC
  编码器处理，JSON 只出现在 wire codec 边界。
- 2025/2026 语义保持：MRTR 的 requestState/inputResponses 只在对应协议下
  传递；断流取消路径与独立 RPC ID 映射不变。

验证：Linux Debug 构建通过，MCP 九项 **9/9，28.55 秒**；wire 路由/取消/
elicitation、HTTP 断流取消、重复 RPC ID、2026 discover/MRTR、deadline 与
九工具调用等用例全部通过（测试断言同步迁移到 typed 事件，未删除用例）。
全套 29 项未重跑，Windows/macOS 未运行。

仍未做：`McpService` 组合根收缩与工具用例拆分、`Job::arguments` 过渡缝删除、
`ExecutionService` 显式阶段、`TerminalEvidenceModel` 与 Coordinator/Controller
提示符真源收敛、最终结构删除清单与全套回归报告。

#### 运行期内核重构 W5b：脚本任务与请求记录归位（2026-10-18）

- `Job` 移入 `src/mcp/tasks/Job.h`；新增 `ScriptTaskRegistry`，唯一持有脚本
  发布任务与脚本路径锁。`McpService` 删除 `QHash<quint64, ScriptWriteTask>
  scriptWrites` 与 `QHash<QString, QString> scriptPathLocks`，取消、Provider
  消失、用户输入取消、完成与路径锁解除都经注册表；路径锁仍按作业键释放。
- 任务索引继续保持不复制 `Job` 可变状态：`ExecutionRegistry` 只存 jobKey，
  `ScriptTaskRegistry` 保存仍存活的 `shared_ptr<Job>`（脚本任务生命周期确实
  需要 Job），两者分工明确。
- 创建/删除语义保持：待发布任务上限 `MaxClients * MaxActiveRequests`、执行+
  发布总容量 128、脚本正文 2 MiB、Provider 消失保留未知发布结果与保护标记。

#### 全套回归（2026-10-18，Linux Debug）

`ctest --test-dir build --output-on-failure`：**27/29 通过，190.17 秒**。

- 通过：MCP 九项（28.53 秒）、core 三项、renderer/p5、session 除串口 PTY 用例外
  的 89 项、filetransfer 八项、serial 门面/UI、telnet、ssh、ui 三项等。
- 失败：`novaterm_pty_tests`（6 个用例，`openpty` 无法启动子进程，既有环境问题）
  与 `novaterm_session_tests` 的 `realSerialSessionUsesExistingPumpForFileTransfer`
  两个数据行（同一 `openpty` 根因）。两项在本轮改动前的未修改工作树同样失败，
  不是重构回归；不得据此宣称 29/29。
- Windows/macOS 未运行。九工具目录 fixture、2025/2026 wire、授权/确认/执行/
  观察/搜索用例全部通过，未删除测试。

仍未做：`McpService` 组合根收缩与工具用例拆分、`Job::arguments` 过渡缝删除、
`ExecutionService` 显式阶段、`TerminalEvidenceModel` 与 Coordinator/Controller
提示符真源收敛、最终结构删除清单与兼容证据汇总。

#### 运行期内核重构：结构删除清单与验收状态（2026-10-18）

**已完成并删除的旧结构**

| 旧结构 | 去向 |
| --- | --- |
| `McpService::readContext` 内联采样/投影/capture | `ObservationService::read` + `ProtocolProjectionSizer` |
| `Capture` 结构、`captures/lastCapture/lastRevision/lastBase/publishedStats` | `ObservationService`/`CaptureStore`（含 typed `CaptureLine`） |
| `PendingConfirmation`、5 张确认表 | `tasks/ConfirmationRegistry` |
| `Execution` 结构、`QHash<QString, Execution>` | `tasks/ExecutionRegistry` |
| `Job::scriptReservationId/scriptReservedTarget`、`guards.reserve/release` 手写配对 | `tasks/TargetReservation`（move-only） |
| `scriptWrites`、`scriptPathLocks` 两张表 | `tasks/ScriptTaskRegistry` |
| `Connection::active/waiting`、`schedule()` 手写队列 | `application/RequestScheduler` |
| 内部 JSON `op` 总线（call/cancel/elicitation） | `protocol/BackendEvents` typed variant |
| `McpProtocol` 里的工具目录/校验/解码 | `protocol/ToolCodec` |
| `AccessStore::setGrant/setTerminalGrant/Grant/confirmedCommand/canRunConfirmedCommand` | `AuthorizationService` + `PermissionChange` |
| 对话框直接 `AccessStore` + 整组 grant 回写 | `SessionPermissionModel` + `SettingsController` 单项意图 |
| `TerminalInteraction` 自有 capture 容器与局部配额/sizer | `CaptureStore` + `ProtocolProjectionSizer` + `Budget` |
| Session 源文件由 `novaterm_mcp_runtime` 编译 | `novaterm_session_automation` 目标 |

**兼容证据**

- 九工具目录 fixture（`tests/mcp/fixtures/tool-catalog-2026-10-18.json`）在
  `novaterm_mcp_contracts_tests` 中按解析后的对象逐项比较，含 accepted/rejected
  参数、默认值、未知字段与错误映射。
- 2025 会话/elicitation 与 2026 无会话 MRTR 的 wire、路由、取消、重复 RPC ID、
  断流语义由 `novaterm_mcp_wire_tests`/`http`/`endpoint` 覆盖。
- 授权、确认、脚本发布、执行未知结果、观察/搜索/compact/full、目标保护与
  重启标记由 `novaterm_mcp_tests`、`access_interaction`、`sftp`、`command_safety`
  覆盖；没有删除测试，只把断言迁移到 typed API。
- MCP 九项 9/9（29 秒级）；Linux 全套 27/29，失败两项为 `openpty` 环境问题。

**仍未完成（不得宣称“彻底重构完成”）**

1. `McpService` 仍是组合根与大方法集合：`jobs`、`connections`、
   `lastCommand`、`observed*` 集合由 Service 持有；读取/搜索/动作/脚本/命令
   尚未拆成独立用例类。
2. `Job::arguments` 过渡 JSON 缝仍在（约 18 处读取），九工具虽已 decode 为
   `ToolRequest`，部分 handler 仍读原始参数。
3. `ExecutionService` 只有记录与终态收口，未实现验证/预留/提交/观察/结算的
   显式阶段机。
4. `TerminalEvidenceModel` 未创建；`SessionCommandCoordinator` 与
   `SessionTerminalController` 仍各自维护提示符候选/就绪状态（Controller 在
   部分路径读取 Coordinator，但不是单一写入模型）。
5. `TerminalInteraction` 的 wait/replay 与动作参数仍以 JSON 读取；完整输入
   交互用例尚未迁入观察/任务组件。
6. Windows/macOS、真实设备与托管提权适配器未验收。

#### W5c 补充：typed capture 行（2026-10-18）

`CaptureRecord::lines` 从 `QJsonArray` 改为 `QVector<CaptureLine>`（text/source/
row）。`CaptureSearch`、`ObservationService`、`TerminalInteraction` 的展开路径
与测试改为直接读写值类型；JSON 只在展开投影对 wire 输出时重建。MCP 九项
9/9（29.18 秒）。

#### W5d 补充：任务阶段字段（2026-10-18）

`ExecutionRecord` 增加 `TaskPhase{Preparing, Reserved, Submitted, Observing,
Final}`：脚本/命令记录在预留与提交完成后以 `Submitted` 创建，提交成功后转为
`Observing`，`finishExecution` 与脚本发布结果转为 `Final`。验证与预留仍在
创建记录前完成，尚未做成独立状态机；阶段字段只用于统一记录与后续审计，
不改变结果语义与容量。MCP 九项 9/9。

#### W5e 补充：命令去重组件（2026-10-18）

`McpService` 的 `QHash<QString, qint64> lastCommand` 移入
`application/CommandDedup`：同一目标指纹的短时重复命令门控与 5 秒过期
sweep 集中一处；Service 只调用 `blocked/remember/sweep/empty`。行为不变
（登记即阻断，过期由周期 sweep 决定），新增单元测试覆盖登记、未过期、
到期清理与 empty。

MCP 九项 **9/9（28.41 秒）**，`ctest -N` Linux 29 项。

#### W4c 续：Session 单一提示符证据模型（2026-10-18）

- 新增 `src/session/automation/TerminalEvidenceModel.h`（header-only）：唯一保存
  候选/就绪代际、用户代际与静默窗；`notePromptReady/noteCandidate/noteBytes/
  invalidate/restore` 是全部写入入口，静默窗到期才锁定候选并通知消费者。
- `SessionCommandCoordinator` 删除 `_promptSilence`、`_promptGeneration`、
  `_candidatePromptGeneration`、`_promptUserInputGeneration`、
  `_candidateUserInputGeneration`，只保留命令状态机并委托模型；
  `isPromptReady/submit` 读取模型代际与用户代际。
- `SessionTerminalController` 删除 `_prompt/_promptUser/_candidate/_candidateUser/
  _promptTimer`；`promptGeneration()`、`readyForCommand()`、observation 与编码
  回滚全部经 Coordinator 的同一模型 API。控制器不再能复活第二份提示符真源。
- 行为保持：候选静默窗 150 ms、非集成字节取消候选/清除就绪、reset/用户输入
  抢占、编码失败只在没有新事件时回滚、命令结束后同一事件内的提示符立即可用。

验证：MCP 九项 **9/9（28.92 秒）**；`novaterm_session_tests` **91 通过 / 2 失败**，
两项仍是既有 `openpty` 环境失败（`realSerialSessionUsesExistingPumpForFileTransfer`），
非回归。Linux 全套计数仍为 29 项，Windows/macOS 未运行。

#### W5f 补充：在途任务注册表（2026-10-18）

- 新增 `src/mcp/tasks/TaskRegistry`：在途 `Job` 的唯一所有者，提供按键查找、
  存在性、移除、容量、空判断与并发安全的快照列表；`McpService` 删除
  `QHash<QString, std::shared_ptr<Job>> jobs`。
- 会话失效、断开、撤权、过期扫描与调度投递都经注册表快照遍历；遍历期间任务
  可同步完成，回调仍按当前注册表状态复核。`peakRequests` 统计保持。
- `Job` 仍持有 wire 参数过渡缝，类型化请求解码不变；本步只收敛任务所有权。

P8 结构删除清单同步：`jobs` 并行表 → `TaskRegistry`。MCP 九项 **9/9
（28.62 秒）**。

#### W5g 补充：TerminalInteraction typed 化与 Job::arguments 删除（2026-10-18）

- `TerminalInteraction::call` 改为接收 typed `ToolRequest`；会话身份由
  `sessionReference()` 提取，工具名由 `ToolCodec::toolName()` 得出。读取、
  展开、搜索、交互、提权的等待条件/投影/动作全部从 typed 字段读取，
  `args.value(...)` 读取全部删除。
- 动作批次改为 typed `std::vector<TerminalAction>`；新增
  `ToolCodec::encodeActions()` 生成规范化 wire 形状，用于幂等操作摘要与
  Controller 提交。动作到 `TerminalInputAction` 的编码直接按 variant 分支。
- `Job::arguments` 删除。脚本确认摘要与脚本元数据摘要在 wire 接收时由原始
  参数计算后保存（保持既有摘要语义）；脚本发布后解析出的调用命令保存为
  `resolvedCommand`；命令/脚本超时从 typed 请求读取。
- `McpService` 与 `TerminalInteraction` 现在都不再持有或读取内部 JSON 业务
  参数；JSON 只出现在 wire codec、摘要计算与投影输出边界。

验证：MCP 九项 **9/9（28.54 秒）**，wire/HTTP/授权/确认/脚本/执行/观察/搜索/
交互全部通过。Linux 全套计数仍 29 项；Windows/macOS 未运行。

仍未做：`McpService` 大方法拆分（组合根收缩）、`ExecutionService` 完整阶段机、
`TerminalInteraction` wait/replay 状态迁移、Windows/macOS 与真实设备验收。

#### W5h 补充：只读用例拆分与任务阶段推进（2026-10-18）

- 新增 `src/mcp/tasks/ReadUseCases`：`list_sessions`、`read_context`、
  `list_commands` 的适配逻辑迁出 `McpService`，只依赖 AccessStore、
  SessionDirectory、ObservationService 与 TerminalInteraction 能力查询；
  Service 只负责完成作业。目录/上下文 cursor 与 grant 校验语义不变。
- `ExecutionRegistry::advancePhase()` 强制 `Preparing→Reserved→Submitted→
  Observing→Final` 单向推进（Final 可由任意非终态到达），命令/脚本提交与
  完成都经该入口，非法回退被拒绝。
- `McpService` 不再持有读取/列表用例实现；搜索 worker 与脚本/命令大方法仍在
  Service，属于下一步收缩范围。

验证：MCP 九项 **9/9**；Linux 全套 **27/29（190.36 秒）**，失败仍是
`novaterm_session_tests` 的串口 PTY 与 `novaterm_pty_tests` 的 `openpty`
环境问题，非本轮回归。Windows/macOS 未运行。

#### W5i 补充：固定诊断命令用例拆分（2026-10-18）

- 新增 `src/mcp/tasks/DiagnosticCommandUseCase`：`execute_command` 的票据校验、
  权限/隔离区/容量检查、预留、执行提交与 `finishExecution` 回调装配全部迁出
  `McpService`；Service 只传 AccessStore、ExecutionRegistry、ScriptTaskRegistry、
  TargetGuard、CommandDedup、观察集合与完成/变更回调。
- `McpService::command()` 缩减为依赖装配；执行记录仍由 `ExecutionRegistry`
  持有，阶段从 `Submitted` 开始，完成回调由 Service 注入。
- 语义保持：ticket scope/过期/策略/授权复核、quarantine、每连接 32/总量 128、
  重复目标 5 秒门控、facade 信号只接一次、失败释放预留并返回 BUSY。

验证：MCP 九项 **9/9**。命令/脚本/执行未知结果/目标保护等既有用例全部通过。

#### W5j 补充：脚本结果收口拆分（2026-10-18）

- 新增 `src/mcp/tasks/ScriptResultUseCase`：脚本发布结果的校验、执行记录
  （`SCRIPT_PUBLISHED_NOT_INVOKED`/`SCRIPT_WRITE_FAILED`/未知发布）、预留释放、
  失效判定与 `runCommand` 交接从 `McpService` 迁出。
- Service 只注入 AccessStore、ExecutionRegistry、ScriptTaskRegistry、
  TaskRegistry、SessionDirectory、完成/释放/执行回调；`finishScriptWrite` 与
  `recordScriptWriteOutcome` 缩为适配层。
- 语义保持：发布结果记录的 `published/publicationMayHaveOccurred` 细节、
  Provider/Profile/授权/代际校验、未知结果保留保护标记、发布成功后解析调用
  命令并转入自由命令执行。

验证：MCP 九项 **9/9**；脚本发布/取消/未知结果/迟到发布/目标保护用例全部通过。

#### W5k 补充：脚本/自由命令主流程拆分（2026-10-18）

- 新增 `src/mcp/tasks/ScriptCommandUseCase`：`runScript`/`runCommand` 的脚本风险
  确认、MRTR/表单确认、发布预留与路径锁、Provider 观察、facade/coordinator
  提交、确认 stale 复核与执行记录创建全部迁出 `McpService`。
- `McpService` 从 1897 行降到约 983 行；`runScript`/`runCommand` 缩为两行适配，
  仅保留完成/释放/记录/失效回调的装配。依赖以 `Ports` 显式注入，包含
  AccessStore、确认/执行/脚本/任务注册表、TargetGuard、CommandDedup、
  observed 集合、计数器、连接身份与发送/完成/释放/记录/终态/变更回调。
- 语义保持：脚本正文摘要、发布预留移交、未知结果保护、确认/取消/过期、
  每连接与总量容量、路径锁、用户输入取消、facade/coordinator 只接一次。

验证：MCP 九项 **9/9（28.52 秒）**；脚本发布/确认/取消/未知结果/执行/
目标保护/用户抢占用例全部通过。Linux 全套待重跑；Windows/macOS 未运行。

#### W5l 补充：执行终态收口与最终结构验收（2026-10-18）

- 新增 `src/mcp/tasks/ExecutionService`：命令结果投影、输出截断、未知结果
  判定、目标保护释放、作业完成与执行记录终态从 `McpService` 迁出。
- `TaskPhase` 与 `taskPhaseCanAdvance()` 移到 `contracts/TaskPhases.h`；
  `novaterm_mcp_authorization_tests::taskPhasesAdvanceOnlyForward` 覆盖合法
  递进、跳过、回退与 Final 终止，状态转换不再依赖 HTTP/事件循环。
- `McpService.cpp` 从重构前约 2106 行降至约 900 行；只保留 backend 事件接入、
  请求调度、完成/取消/过期、工具分派与组件装配。全部九个工具的业务处理已在
  `TerminalInteraction`、`ReadUseCases`、`DiagnosticCommandUseCase`、
  `ScriptResultUseCase`、`ScriptCommandUseCase`、`ExecutionService` 中。
- 最终删除/撤销清单：`Job::arguments`、工具处理大方法（读取/列表/命令/脚本/
  执行终态）、`TerminalInteraction` 内部 JSON 读取、等待/回放之外的旧业务映射、
  旧 capture/确认/执行/脚本私有集合。保留 JSON：wire codec、工具入口摘要、
  持久化、响应投影。

验证：MCP 九项 **9/9（28.36 秒）**；Linux 全套 **27/29（190.26 秒）**，失败
两项仍是 `openpty` 环境问题（`novaterm_session_tests` 串口 PTY、
`novaterm_pty_tests`），非本轮回归。Windows/macOS、真实设备、受管提权适配器
与 Windows 文件系统竞争用例未运行，不能据 Linux 结果宣称通过。

#### 串口空闲收尾降级（2026-10-18）

用户反馈 Netcatty 的 AI Agent 可在串口终端直接执行命令，而 NovaTerm 必须先
配置 MCP 提示符正则与命令行结束符。本轮为**串口**空提示符增加空闲收尾降级
（Telnet/Custom 不变），显式提示符仍是高可信路径：

- `InteractiveCommandProfile` 新增 `allowIdleCompletion`（默认 false）与
  `idleQuietMs`（默认 3000）；`ShellIntegration::profileFor` 在串口、空提示符、
  非 shell-integration 时返回降级 Profile：`requiresStartMarker=false`，
  不要求提示符就绪，命令与行结束符直发。
- `SessionCommandCoordinator` 新增两条计时器：命令活跃且已开始后，收到首批
  设备字节即启动 `_idleQuiet`（静默窗到达即完成，写
  `terminationConfirmed=true`）；从未收到任何设备输出时由
  `_noResponse = min(4×静默窗, 预算/2)` 兜底收口，避免无回显设备把已跑完的
  命令拖到整体超时（TimedOut 会隔离目标，代价更高）。结果 `exitCode=nullopt`，
  按"完成未验证"上报；`ExecutionService` 对交互执行仍映射为成功状态。
- `terminationConfirmed` 必须为真：`ExecutionService` 只在其为真（或未开始）
  时释放目标保护，否则每条串口命令都会进入 `COMMAND_EXECUTION_QUARANTINED`。
- 回显剥离只在 `allowIdleCompletion && remoteEcho` 时执行：首行 trim 首尾
  空白后等于命令或以命令结尾则丢弃首行，其余字节原样保留；屏幕仍显示回显，
  仅捕获结果剥离。远端未回显的设备不受影响。
- `InteractiveSessionCommandExecutor::isAvailable()` 放行空闲模式（确认后
  复核在 `ScriptCommandUseCase` 里依赖它）；`McpService` 确认后复核与
  `ScriptCommandUseCase` 的就绪闸门改为 `promptFree`（未验证 SSH 或空闲串口）。
  `CapabilityFacts` 新增 Serial 专属 `idleCompletion`，能力解析将其排除在
  `run_script` 之外。串口表单占位与提示文案同步修改，中英文已回填。
- 范围仅 `novaterm_run_command` 与能力发布：`interact_terminal` 的 `command`
  动作仍要求提示符就绪（`SessionTerminalController::readyForCommand`），空闲
  降级不改变它；串口本无脚本写入与固定诊断后端。
- 不改 wire schema/枚举与错误码；成功响应 `terminationConfirmed=true` 与
  目录 fixture 兼容基线一致。

新增回归：

| Case | 验证内容 | 本轮结果 |
| --- | --- | --- |
| `SessionTests::configuredDevicePromptDoesNotProbeTransport` | 空规则得到降级 Profile（`allowIdleCompletion`、默认窗 3000ms、CR 行结束）；配置正则后不再降级 | 通过 |
| `SessionTests::serialWithoutPromptCompletesByIdleQuiet` | 直发命令 + 回显上屏；静默窗完成、终止确认、无退出码、输出剥离回显 | 通过 |
| `SessionTests::serialIdleCompletionNoOutputFallsBackToDone` | 完全无输出时按无响应兜底（min(4×50, 2500)）收口 | 通过 |
| `SessionTests::serialIdleCompletionKeepsEchoWhenRemoteEchoDisabled` | 远端未回显时不误剥第一行输出 | 通过 |
| `SessionTests::serialIdleCompletionTimeoutWinsOverLongQuiet` | 整体预算先到 → TimedOut、`terminationConfirmed=false` | 通过 |
| `SessionTests::serialIdleCompletionPreemptedByUserInput` | 用户输入抢占 → Cancelled，输入照常送出 | 通过 |
| `McpTests::serialWithoutPromptCompletesByQuietWindow` | 空规则串口发布 `run_command` 且不发 `run_script`；直发命令、静默完成、stdout 不含回显 | 通过 |

本轮验证（Linux Debug）：全部默认目标构建通过；`novaterm_session_tests`
全项通过（15.97 秒，含同日 W 系列用例与本节新增六项）；MCP 九项 **9/9
（28.06 秒）**；`novaterm_session_panel_tests` 通过（0.11 秒）。单测通过缩短
`idleQuietMs` 覆盖窗口，默认 3000ms 由 Profile 用例断言。真实 UART、
Windows/macOS 未运行；空闲证据弱于提示符/结束标记，可能把"仍在运行但静默"
的命令判为完成，该局限保留在契约文档与表单提示中。
