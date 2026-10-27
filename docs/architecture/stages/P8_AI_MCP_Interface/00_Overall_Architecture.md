# SSH/UART MCP 终端工具总体架构

2026-10-27 B3b第二子批：新增独立授权的 `novaterm_open_target` / `novaterm_query_target`，共二十工具；打开/复用使用现有可见终端，配置/客户端身份和operationId绑定，不继承终端权限。关闭、单文件传输与B4仍待后续，见[本批记录](records/B3b_Target_Connections_2026-10-27.md)。

更新：2026-10-25。面向嵌入式 BSP 开发的外部 Agent 调试闭环。B0/B1 已实施，见 [B0/B1 实施与验收](records/B0_B1_Implementation_2026-10-25.md)；本文重写旧诊断/Agent 平台方案；源码现状与目标差距见 [03](03_Gaps_and_Evidence.md)。

## 1. 产品目标与责任

外部 Agent 在用户本地工程中编辑源码、编译、调用已有上传/烧录工具，随后通过 MCP 观察和操作已有 SSH/UART 会话。Agent 负责推理、规划和判断测试结果；NovaTerm 负责可靠终端连接、文本与屏幕、输入仲裁和可解释状态。

不内置聊天、Agent 宿主、模型 SDK、推理循环、自动压缩或诊断工作流。Netcatty 只可作为终端工具行为的参考，不再有模块迁移任务。

SSH 适合 Linux 运行期交互，UART 可跨 SSH 断连观察启动、U-Boot 和控制台。两类通道都复用同一终端应用能力；目标绑定、环境差异和完成证据不能被统一接口掩盖。

## 2. 数据与控制通路

~~~mermaid
flowchart TB
    Agent[本地外部 Agent] -->|MCP HTTP| Wire[协议与身份适配]
    Wire --> UseCases[观察 / 输入 / 清空应用用例]
    UseCases --> Session[TerminalSession 与授权仲裁]
    Session --> Transport[SSH / Serial Transport]
    Transport --> Device[目标设备]
    Transport --> Pump[唯一 InputPump]
    Pump --> Core[Parser / TerminalCore / Scrollback]
    Core --> Snapshot[有界文本与屏幕快照]
    Snapshot --> UseCases
    Human[本机用户] --> Session
~~~

B2 已将独立授权清空接入现有 Core/UI 同源的 Parser 队列，完成与有界幂等契约见 02。不得建立第二条 UART 读取线程、原始日志采集或旁路 Transport 写入来实现 MCP。现有会话的 View owner 不因 MCP 改为新的 SessionManager。

## 3. 分层与状态所有权

| 层/组件 | 唯一职责 | 禁止承担 |
| --- | --- | --- |
| WireSession / HTTP endpoint / ToolCodec | 协议生命周期、可信身份、Schema、类型化解码和结果编码 | 判断 BSP 测试成功、持有终端可变状态 |
| 应用读取/观察状态 | 授权、预算、等待、调用方进度和取消；复用 ReadUseCases/ObservationState | 自建全量日志数据库或直接写 Transport |
| CommandRuntime / 命令适配 | 已有命令与脚本任务、确认、发布、取消和未知结果 | 编排本地工程或模型回合 |
| AuthorizationService / 能力准入 | grant、版本、级联撤权、可用性与拒绝原因 | 根据模型参数创建可信身份 |
| Session / Controller / Arbiter / Evidence | 连接、环境和控制代际、人工优先、输入提交、完成事实 | 解析 MCP JSON 或根据输出猜测全部权限 |
| TerminalCore / Scrollback | Parser 单写、屏幕、缓存位置、淘汰与清空执行事实 | 为每个客户端复制历史、等待网络消费 |
| UI | 共享/撤回、人工操作、清空意图和状态显示 | 保存另一套授权/游标真源 |

现有 Capability 目录和类型化领域端口可复用，但不为这轮新增通用 Manager、全局事件总线或超级请求联合类型。协议组合根只装配和分派；每项可变状态明确一个 owner。核心用例通过窄端口隔离具体 Session、HTTP 和 QWidget。

QObject 使用一致的 parent 或智能指针所有权；跨异步边界只传有界值/不可变片段及标识，不传 UI/Session 裸指针到工作线程。回调必须核对取消、对象身份和连接代际，不能在旧任务完成后写新任务。

## 4. 观察模型

屏幕用于回答“终端现在是什么状态”；日志读取用于回答“从这个位置以后新增了什么文本”。现有 filtered/deduplicated recentOutput 仅是摘要，不用它冒充日志。

B1 已实现的日志语义复用 Core/Scrollback 中的解析后缓存，稳定位置和缓存代际取代“模型 revision 等于已读完”的假设。保留真实重复行，提供按实际交付位置续读、最早可读位置与明确缺口。屏幕/历史交界不靠内容去重。

具体分页、字段冻结和清空事务见 [04](04_Cursor_Observation_and_Clear.md)。不另建录制系统，不承诺无限日志、原始串口字节保真或覆盖前的每一帧。现有录制能力仍可独立使用。

## 5. 身份、权限与输入

- 客户端、请求、Session、连接 epoch、缓存代际、观察位置、operationId 和 executionId 分别表达不同事实；token 不代替授权。
- 所有工具显式指定目标；切标签、改标题或另一会话收到类似日志都不能重定向操作。
- 读取、普通命令、脚本、完整输入和清空权限须明确。只读 grant 不能清空或输入；B2 清空使用独立 ClearTerminal 授权，默认关闭，旧 grant 不升级。
- 人工键盘/粘贴优先；人工接管、撤权、Session 替换和连接变化使旧输入条件失效。
- 相同 operationId 的精确重试不重复产生输入或清空；改变参数拒绝，不能通过换 ID 绕过未知执行保护。
- 完整输入授权是广泛终端控制，不是每条命令的风险审查或 Shell 沙箱。模型不能从工具描述中得到错误保证。
- UART 链路未断不代表环境未变；U-Boot、Linux、REPL 和密码提示须通过屏幕/明确配置辨别，不能用普通日志直接授予执行就绪。
- 不向模型返回本机密码/私钥；托管提权没有可信适配器时保持不可用。

MCP 输出一旦交付，NovaTerm 无法控制外部 Agent 后续发给哪个模型。共享终端意味着向该获准客户端披露对应内容；撤权限制后续访问，不能追回已交付文本。不再规划应用内 AgentContextPolicy 或外部 thread 的宿主治理。

## 6. 完成、等待与恢复

submitted 仅证明本机提交。等待到期不是命令完成，命令完成也不是板上测试通过。UART 静默收尾是兼容降级；结果的证据类型、退出码及限制必须一起解读，不能只看 terminationConfirmed。

长任务以连续有界观察延续，不占一个无限等待 RPC。停止读取不发送 Ctrl+C；显式取消执行、释放控制和终止设备进程是不同动作。未知结果必须保留，不能自动重发烧录/重启。

SSH 断连和 UART 持续输出分别观察。当前 MCP 可操作已授权会话，B3b已提供已获准保存目标的显式打开/复用与连接查询，不开放任意地址建连或自动循环重连。SSH/SFTP 单文件交付也纳入 B3b，但烧录、JTAG、电源控制仍由明确范围的外部工具承担；U-Boot 短倒计时抢键必须实测后才可承诺。

## 7. 兼容与已有模块

冻结基线为九个终端工具与三个离线历史工具，精确说明见 [02](02_Current_MCP_Contract.md)。B1 保留已发布十二工具与既有权限；read_context 的显式 source=log 扩展以独立 fixture 冻结，旧观察参数/输出分支保持。未发布 P4 及仅供其消费的原型退役，不改变旧 sinceToken。

已有 recording/history-search/evidence 作为独立功能保留：录制开启不自动共享，离线 grant 不继承 Session grant，分页完整不证明源完整，Evidence 不代替授权。共享服务及来源适配不因 MCP 关闭就删除独立导入资源。实际结构与验证见 [资源边界记录](records/P3_Architecture_Fixes_2026-10-24.md)。

这些模块不再是在线终端闭环的前置。历史阶段的实现计划已删除，但不以此回滚已工作的功能。按用户追加授权，未验收 P4 诊断平台及无生产消费者的 P1 Recipe/EnvironmentTracker/本机 Agent 原型已完整退役。执行继续由 CommandRuntime/Session 控制器持有；旧固定诊断与离线历史保留。清单及证据见 [B0/B1 实施与验收](records/B0_B1_Implementation_2026-10-25.md)。

## 8. 预算与线程

现有上限以 Budget.h 为真源。读取和分页均有字节、行数、等待和并发上限；日志页的文本预算不代表 JSON/内存总开销。后台块未就绪明确返回，不在 GUI 线程阻塞冷块加载或遍历全历史。

MCP 慢消费者不能拖住 Parser/Renderer，不能为等待下一页无界固定历史块。源缺失与响应裁剪分别报告；屏幕时间戳如实反映快照年龄，不通过返回旧屏幕伪造高可用率。

历史性能成绩及串口驱动丢帧记录只作基线，新的闭环按 [01](01_Phases_and_Acceptance.md) 单独验收。

## 9. 对照补齐的应用边界

[06](06_Terminal_Workflow_Extensions.md) 规定文本条件等待、长任务、单文件传输和保存目标连接的语义。2026-10-27 B3a等待/任务已实现，精确字段与限制见02，验收范围见 [B3记录](records/B3_Implementation_2026-10-27.md)；B3b第一子批增加保存目标只读授权目录，见[目录记录](records/B3b_Target_Directory_2026-10-27.md)。目标打开/复用/查询已由第二子批实现，见[连接记录](records/B3b_Target_Connections_2026-10-27.md)；传输和关闭仍未实施。

- 文本等待复用同一日志/屏幕源、授权和等待调度；仅增加有界匹配状态，不建立第二采样源或脚本引擎。
- 长任务复用 CommandRuntime/Controller 的执行事实，只增加异步调用关联、查询和有界结果保留。SSH/UART 分别声明完成与停止能力。
- 文件传输复用 SSH/SFTP 和发布边界；独立本地资源与远端路径授权。传输任务不能反压终端 Parser，也不能将文件发布等同于烧录完成。
- 保存目标连接通过已有 Profile/Session 创建接线和本机组合根实现，保留 View-owned Session；显式目标授权不等于输入授权。
- 新的任务/传输/连接 ID 各自绑定客户端、目标、版本和幂等操作，不能以中心万能任务对象聚合所有可变状态。

精确公开字段、有限预算和错误码在相应 B0 子契约冻结；工具仅在实际实现、测试和权限配置到位后注册。现有十二工具契约不能因新设计被默默放宽或改变默认值。

## 10. 文档权威

本页定义当前架构方向；02记录已实现契约；03给出静态差距及分轮更新；04日志/清空和06等待/任务、目录/打开/查询已实施，06文件/关闭仍为目标；05是Agent使用流程；01规定实施和验收。records只保留实施/测试/故障证据，不携带继续实施旧平台方案的授权。全项目原则仍以 [统一架构](../../../ARCHITECTURE.md) 为准。
