# MCP / P3 应用边界收敛（2026-10-24）

> 历史实施与验证证据；保留原日期、命令、失败和限制。不是当前实施计划，旧 P 编号仅用于追溯。当前路线见 [目录入口](../README.md)。

基于 `0eec9e9` 及同日之前未提交的五项修复，落实架构评审 A-001～A-003。保留十二工具及其 Schema、预算、凭据/授权文件格式和已有本地 P3。用户随后要求创建 Git 提交，版本递增至 **0.2.82**；不推送。

## 1. 当前边界与所有权

| 组件 | 职责与 owner | 依赖 |
| --- | --- | --- |
| `ResourceSharingService` | 显式共享事务、授权真源、查询与证据发布流程；读取流/搜索状态仍由各自 History 服务拥有 | Qt Core、History search、Evidence；最小 IdentityLookup / GrantPersistence / HistoryReadPort |
| `HistoryCapabilities` | list/read/search 三个类型化领域入口；必经共享 Dispatcher，提交与交付前复核准入 | 上述应用服务、capabilities；无 HTTP/JSON/Session |
| `HistoryCapabilityAdapter` | MCP wire DTO / 签名游标 / JSON 投影及 concrete recording 适配；兼容 facade 只转发 | 注入的共享应用实例、协议和领域契约 |
| `ResourceComposition` | 默认 MCP principal epoch 与私有 grant 文件适配；安全文件函数仍复用原实现 | MCP AccessStore / 私有文件边界，仅集成层使用 |
| `Application` | 显式 shared 持有应用服务，UI 共享意图直接调用该实例；MCP 可注入同一实例 | 具体组合根，允许知道具体实现 |

`novaterm_resource_sharing` 不依赖 MCP/HTTP/Widgets 或具体 Session/SQL 存储。新测试程序的 DLL 依赖只有 Qt6Core/Qt6Test 和系统/CRT；证据见 `build/architecture-fix-application-dependencies.txt`。

这不是新增全系统 Manager：请求/RPC 仍归 Service；命令阶段归 CommandRuntime；读/搜索流归 History；Evidence 库不反向依赖 History。应用服务仅组合显式资源共享和查询发布流程，不持终端执行任务。

## 2. 三项修复

### A-001：取消回收到读取 owner

HistoryReadRequest 增加取消状态与 owner-thread 准入谓词。HistoryReadService 在接纳与迟到完成提交分页状态之前检查；取消/准入失效删除 stream，不发布无人接收的 nextCursor。实际在途 I/O 的 active 额度仍待其回报后减少，不提前宣称底层读取已停止。

HistoryCapabilities 将 Dispatcher 的取消与租约 validation 绑定到领域请求，MCP cancel/disconnect/deadline 继续使用相同操作上下文。完成回调即使最后被协议层丢弃，也不再遗留未交付游标。身份/授权交付复核仍保留。

回归：70 次晚到取消后其他 principal 可读；准入前取消零存储 I/O；70 次在途调用后关闭可信入口，也不会耗尽读流名额。存储适配器负责在应用 owner 线程交付回调，领域谓词不在 CPU/SQL worker 执行。

### A-002：资源应用生命周期不再绑定录制来源

历史授权、私有持久化事务、读取/搜索、导入和证据发布逻辑从 MCP adapter 移到协议无关应用服务。身份事实与持久化通过窄端口注入；应用服务不读取外部 MCP enabled。默认外部身份桥接仍验证 AccessStore 当前 epoch；本机入口可使用自己的可信授权 lookup，不能由模型参数构造 principal。

setHistorySource 只替换历史查询适配和 terminal_capture reader，清理该来源的查询缓存；不清空 imported_file reader、导入授权或已生成的导入证据。Application 显式持有共享实例，UI 不再经 `Service::history()` 实现共享事务。MCP Service 构造函数也支持注入已有应用服务。

授权文件仍是原私有 `history-grants.json` / lock，格式 version 1。锁不可用时保留原先明确的拒绝；持久授权恢复仍核对当前 principal epoch，不恢复临时授权。

回归：无录制来源也可显式导入；增加/移除录制来源后导入 EvidenceRef 仍可解析；撤回后解析拒绝；两个可信入口调用同一应用服务，停用外部入口不影响本机入口；存储 generation=3 正确传到授权绑定、共享状态和 EvidenceRef，不写死为 1。

### A-003：冻结旧终端联合类型

`ToolRequest` 保留原九工具；History wire DTO 移到 `protocol/HistoryToolRequest.h`。`ToolCall` 仅是协议组合 envelope，不进入 terminal application/tasks；WireSession 分别产生 BackendCall / BackendHistoryCall，HTTP endpoint 分别登记回包关联。

MCP Service 仅做协议组合和请求生命周期映射，History 通过领域类型化端口准入，不再通过 `LegacyTerminalMapping`。旧终端应用与 Dispatcher 不包含 HistoryToolRequest / ToolCall。新增领域仍需在协议组合层接入 codec/路由，不宣称已有动态插件或 P4/P6 产品集成。

回归：编译期冻结 ToolRequest variant_size=9；原九工具和三历史工具 fixture 保持；History 使用独立 backend event；完整 MRTR 元数据下，没有 requestState 的孤立 inputResponses 保持原来的忽略行为，不变成一次确认。

## 3. 独立复核与收尾修正

两名只读复核者分别审查资源/准入和协议/路由。资源复核发现搜索缓存重放、异步证据发布后还需检查取消和 admitted；补回归先 **2 failed**，修复后定向 **4/4**。失效时取消查询 cursor、删除新 EvidenceRef、不写缓存。复核后未发现新的明确阻断项。

全量回归发现一个实现回归：已取消的非法 UTF-8 查询被提前判断为 CANCELLED。保留原断言，将完整 UTF-8 入参校验移到取消检查前，最终历史目标 **34/34**。

新增 MRTR 兼容夹具起初缺少协议要求的 clientCapabilities 元数据，失败在未路由到 backend；修正夹具后通过。该夹具失败不记作产品缺陷。自动审批拒绝了为验证红灯而临时恢复旧 confirmation 判断的操作，理由是会重新引入确认状态混淆；没有执行该回退，保留修复并完成替代验证。

## 4. 实测记录（Windows Release）

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| 读取取消 RED | 2 条新增断言失败 | `build/architecture-fix-cancel-red.txt` |
| 读取取消 GREEN | 定向 4/4，含 init/cleanup | `build/architecture-fix-cancel-green.txt` |
| 无录制来源导入 RED | 明确提示不能导入 | `build/architecture-fix-source-red.txt` |
| 纯应用资源服务 | 最终 QTest 8/8 | `build/Release/tests/resources/ResourceSharingTests.ctest.txt` |
| 当前注册数 | `ctest -N` 实核 49 项 | tests/CMakeLists 与新增 resources 子目录 |
| 全量初测 | **47/49，329.00 秒**；ConPTY 与搜索入参校验顺序失败 | `build/architecture-fix-full-ctest.txt`、`build/architecture-fix-history-full-initial.txt` |
| 修正后相关 22 目标 | 首轮 **21/22，94.42 秒**；唯一失败为新 Wire 夹具缺元数据 | `build/architecture-fix-final-related.txt` |
| Wire 最终单项 | **1/1，2.88 秒**；两条新 wire 函数定向 QTest 4/4 | `build/architecture-fix-wire-ctest-final.txt`、`build/architecture-fix-wire-green.txt` |
| 分段最终覆盖 | 相关 **22 个目标均通过**；历史 QTest **34/34**，资源 **8/8**；剩余失败为已知 ConPTY | 上述各轮日志，不混记为一次全套 48/49 或一次专项 22/22 |
| ConPTY 文件输出完整复跑 | **10 passed / 2 failed，45.862 秒**；仍是两个已知句柄断言，线程/子进程检查通过 | `build/architecture-fix-conpty.txt` |
| 最终构建与依赖 | Release 默认目标构建/重链接通过，应用测试仅 Qt Core/Test | `build/architecture-fix-complete-build.txt`、`build/architecture-fix-application-dependencies.txt` |

ConPTY 此轮 repeated lifecycle 的 final/baseline 为 handles 218/221、threads 6/6、children 0/0；不把这次句柄下降说成复现历史 +200 增长率，不修改断言。

本轮新增/修改接口和构建边界，因此按 AGENTS.md 运行过全量；最后局部修正只复验相关目标。没有再重跑无关目标，也不借用 2026-10-23 的全套成绩。

## 5. 保留范围

- P4/P5 生产诊断结果来源、持久证据、报告消费者、P6 Agent/模型 UI 没有在本轮实现。两个可信入口测试是应用能力验证，不是 P6 产品验收。
- 真实 SSH/UART、其他平台、消毒器、断电和真实性能矩阵未执行；未启动可能恢复真实连接的主程序。
- 架构评审 I-001 的 GUI 停机等待延迟仍待测。本轮没有把同步 waitForDone 改成新的隐式 detached 线程，也不声明有硬退出时限。
- 原五项修复及用户 `.dide/` 保留；后续按用户要求合并创建 0.2.82 提交，不推送。提交前的构建/专项复验在下节单独记录，不覆盖前述各轮历史成绩。

## 6. 0.2.82 提交收口

版本按 AGENTS.md 从 0.2.81 递增至 0.2.82；代码范围为前述五项缺陷与三项架构修复。用户 `.dide/` 不纳入提交。最终版本构建与相关专项结果以本节末尾记录为准。

0.2.82 Release 默认目标构建/重链接通过；提交前新一轮相关 CTest **22/22**（91.85 秒），命令为 `ctest --test-dir build/Release -C Release -L 'mcp|history-search|evidence|sftp' --output-on-failure`。日志 `build/commit-0.2.82-build.txt`、`build/commit-0.2.82-ctest.txt`。这是版本递增后的新一轮结果，与前述分段结果分别记录；未重跑无关全套或 ConPTY，不覆盖其此前两条已知句柄断言。
