# 阶段划分、依赖与统一验收

日期：2026-10-18。总体约束见 [总体架构](00_Overall_Architecture.md)，文档入口见 [README](README.md)。

## 1. 总分结构与实施边界

总体架构规定统一能力、安全、数据、线程与兼容边界；阶段计划分别规定文件职责、接口、任务、测试及退出门槛。本目录 mcp/P0～P8 不替代项目上级 P0～P9 编号。

此次只迁移文档。设计纳入路线不等于授权真实设备访问或实施未来功能；每阶段进入开发时按对应计划逐任务执行，保持现有未提交工作，不重置、不自动提交、不创建额外分支/worktree。

| 阶段 | 目标与独立交付 | 依赖 | 当前状态 |
| --- | --- | --- | --- |
| [P0](P0_Runtime_Consolidation.md) | 九工具运行内核、结构收口、基线 | 现有代码 | 实现与结构收口完成；Linux专项通过，其他平台保留补验范围 |
| [P1](P1_Capability_Runtime.md) | 通用能力目录、本机应用入口、目标环境与执行路由 | P0 对应兼容/安全闸门 | 规划 |
| [P2](P2_History_Recording.md) | 用户主动开启的保真录制、分块持久化、恢复和管理 UI | P1 资源/授权契约 | 规划 |
| [P3](P3_History_Search_and_Evidence.md) | 原文分页、跨块搜索、证据、离线资源授权、三历史工具 | P2；任务证据契约可提前定义 | 规划 |
| [P4](P4_Backend_Diagnostics.md) | Linux 可信 Recipe、独立/共享路由、Backend Pack | P1、P3 证据契约 | 规划 |
| [P5](P5_Embedded_Diagnostics.md) | Boot/Crash/DDR 离线解析，驱动/DT/PCIe 在线采集 | 离线依赖 P3；在线依赖 P4 路由 | 规划 |
| [P6](P6_Agent_Runtime_and_UI.md) | 内置模型回合、统一事件、上下文、聊天与批准 UI | 基础依赖 P1；历史/诊断集成依赖 P3～P5 对应能力 | 规划 |
| [P7](P7_External_Agents_and_Product_Tools.md) | 外部 Driver、续接、原生交互、产品工具、Skills | P1、P6 对应契约 | 规划 |
| [P8](P8_Integration_and_Performance.md) | 行为、资源回收、性能、跨平台与端到端综合退出 | 贯穿各阶段；最终汇总 P0～P7 | 规划/持续 |

~~~mermaid
flowchart LR
    P0 --> P1
    P1 --> P2 --> P3
    P1 --> P4
    P3 --> P4
    P3 --> P5
    P4 --> Online[P5 在线采集]
    P1 --> P6
    P3 -.历史证据.-> P6
    P4 -.诊断工具.-> P6
    P5 -.日志分析.-> P6
    P6 --> P7
    P0 --> P8
    P3 --> P8
    P5 --> P8
    P7 --> P8
~~~

可分流：P1 后开始 P6 只读聊天，不等待历史/诊断全部落地；P3 后 Embedded 离线分析不被 systemd/容器开发阻塞。仅在接口稳定且文件不冲突时安排并行开发，不多个任务同时重写 McpService.cpp。

## 2. 原阶段映射

| 原编号/内容 | 新归属 |
| --- | --- |
| 内核 W1 contracts/ToolCodec | P0 既有基线与兼容审计 |
| W2 授权/ClientRegistry/CapabilityResolver/UI model | P0，P1 复用同一真源 |
| W3 统一观察/搜索/投影 | P0，P2/P3 不用其摘要替代 Raw History |
| W4 任务/确认/执行/Session 证据 | P0，P1 复用并抽取入口 |
| W5 typed backend、用例拆分与删除清单 | P0 已落地清单+剩余结构核验 |
| 诊断 M0 九工具闭环和基线 | P0+P8 |
| 诊断 M1 能力模型/路由 | P1 |
| 诊断 M2 录制存储 | P2 |
| 诊断 M3 搜索/证据 | P3 |
| 诊断 M4 Backend Pack | P4 |
| 诊断 M5 Embedded Pack | P5 |
| 诊断 M6 集成/性能/平台 | P8 |
| Netcatty 回合、事件、模型与聊天交互 | P6 |
| Netcatty 外部 Agent/Skills/CLI/产品工具 | P7 |

## 3. 状态冲突的处理

历史W1～W5的迁移记录保留作追溯；随后执行包装和三个可变注册表已按职责合并，P0又完成纯执行/观察边界、类型化wire入口与停机收口。当前文件、owner和退出证据以[P0§2及验收索引](P0_Runtime_Consolidation.md)为准，不重建已经删除的旧类。

当前P0实现及Linux专项已验收；不同日期的历史测试数字不作新成绩。现有MCP为12个CTest目标，Linux总注册32项；未运行的Windows/macOS和真实设备范围继续单列，不能用Linux或注册数冒充通过。

## 4. 每阶段实施模板

各计划包含 Goal / Architecture / Tech Stack / Spec、全局约束、Review Focus、文件、输入输出接口、复选任务和命令。
每个未实施任务按：先写有意义失败用例→验证失败原因→最小实现→相关专项验证→记录变更。代码提交由实施会话按用户授权决定，本次不提交。
已完成迁移不重写同义测试；先核对现有证据，仅为真实差距新增测试。
每个阶段独立可交付，不建立只有接口没有消费者的空框架。
低耦合、高内聚按总体架构§2.1～§2.4验收：每个任务先明确职责、最小端口、状态owner、允许/禁止依赖和独立验证，不将拆文件或扩大共享Ports视为解耦。

## 5. 统一完成定义

- [ ] 模块围绕共同不变量/生命周期内聚；依赖单向且无环，公开契约不泄露实现，跨模块状态只有一个写入owner。
- [ ] 独立应用测试仅链接所需契约与假端口；增加Provider/Analyzer/Driver不修改无关领域或UI业务分支。
- [ ] 组合根仅装配，Dispatcher仅分派；无全局服务定位器、共享可变Context、巨大Ports或无意义接口转发层。
- [ ] 文件和目标有真实消费者；无重复生产编译、依赖反向或永久 legacy fallback。
- [ ] 九工具 fixture 对象级兼容、accepted/rejected 参数和错误语义均通过。
- [ ] 授权/可用性/目标身份分离；入队、批准后、提交和交付前复核；取消与未知结果正确。
- [ ] 资源/队列/磁盘/worker 有上限；退出不阻塞 GUI；迟到回调零 UAF。
- [ ] 本阶段任务测试与边界矩阵通过，未运行的平台/设备明确列出。
- [ ] 性能报告写明硬件、构建、负载、次数及统计口径；目标不冒充实测。
- [ ] 文档、Schema、目录、配置和删除清单同步；历史恢复不重发操作。
- [ ] 真目标连接、hook/helper/适配器部署另有明确授权，不用模拟成绩替代。

当前十二目标：novaterm_mcp_tests、novaterm_mcp_wire_tests、novaterm_mcp_http_tests、novaterm_mcp_endpoint_tests、novaterm_mcp_contracts_tests、novaterm_mcp_authorization_tests、novaterm_mcp_read_use_cases_tests、novaterm_mcp_command_runtime_tests、novaterm_mcp_observation_state_tests、novaterm_mcp_access_interaction_tests、novaterm_sftp_session_safety_tests、novaterm_command_safety_tests。

## 6. 推荐首个可见交付

P0/P1 安全入口 → P6 只读侧栏 → P6 受控诊断与批准闭环 → P6 长输出/上下文。
P2～P5 按独立交付推进，再接入同一 Agent 工具目录；P7 外部后端最后逐一接通。
首场景：选中错误日志→助手读取有界上下文→提出诊断→批准→显示结果/证据→用户停止与接管。
完整后端/嵌入式/环境切换端到端场景见 P8。

### 6.1 增量交付闸门

下列闸门是可交付子集，不等于P0/P1整阶段完成。未迁移执行路径继续服务现行MCP九工具，本机入口只注册已迁移能力；同一能力只有一个业务执行真源，不为早交付复制授权/观察/执行实现。

| 交付 | 必须先完成的范围 | 可后置范围 | 退出证据 |
| --- | --- | --- | --- |
| A：共享只读入口 | P0 T1兼容、T2读取授权、T3读取/搜索及其T5纯边界；P1 T1/T2只读端口、可信Principal及对应T4独立链接 | 执行任务ownership迁移、Recipe路由、P2～P5 | 九工具基线无回归；本机/MCP同绑定读取一致；撤权后零正文；只读用例不链接HTTP/Widgets |
| B：只读侧栏 | A；P6 T1/T2文本与只读循环、T3 Observer、T4最小有界上下文及发送授权、T5文本/选区/停止 | 批准执行、持久聊天恢复、自动摘要、历史/诊断和外部后端 | 模型请求体来源/接收方核验；切标签不串目标；停止后无后续请求；内存和stream有界；不展示未实现执行/恢复功能 |
| C：批准执行闭环 | B；P0 T4及执行相关T5；P1执行端口/取消/确认和适用路由；P6 T3及工具结果配对、unknown处理 | 新Backend Pack、Raw History、Embedded、外部Agent | 先用现行固定诊断/受控命令；批准拒绝零输入、重连批准失效、取消不谎报远端结束、重试零重复副作用 |
| D：长对话与恢复 | C；P6 T4/T5完整预算、压缩、保留策略、来源依赖和恢复校验 | P2～P5、P7仍可独立推进 | 过期输出不重执行、摘要不绕撤权、恢复不重发工具；按平台记录结果 |

C使用现有诊断时不声称已交付P4/P3持久证据。新增共享Recipe须先满足P1环境事实契约。P4可在P3来源无关Evidence接口与内存结果实现完成后启动，不必等待历史搜索UI；P5离线可消费已共享导入资源，不必等待在线Backend Pack。相应子集独立测试通过后才能供下游消费，不用空接口算交付。

- [ ] 每次子集交付记录涉及的任务、未完成项、入口公开能力及测试范围；不得把未涉及的整阶段复选框勾选。
- [ ] 新增跨阶段契约的验收用例纳入P8补充矩阵；未提供证据时状态仍为规划。

## 7. 各阶段模块边界验收

| 阶段 | 高内聚边界 | 低耦合验收证据 |
| --- | --- | --- |
| P0 | 授权、观察、确认、执行任务和Session证据各有owner | 巨大Ports/共享Job/JSON接缝清单逐项闭环，核心用例不依赖HTTP/UI |
| P1 | 目录、分派、资源授权和执行路由分工明确 | 新handler只在组合根注册，Registry不include具体Provider；静态目标依赖无环 |
| P2 | 录制接纳、分块提交、恢复和配额归History | 禁用录制不影响普通终端；存储测试不链接Agent/诊断/Widgets |
| P3 | 原文搜索归History；来源无关证据/导入独立模块 | 替换History reader不改Analyzer/Evidence核心，三来源经适配器注册 |
| P4 | 每个Provider拥有自己的Recipe约束与结果解释 | Fake执行/证据端口可测，不链接具体Transport、HTTP或HistoryChunkStore |
| P5 | Analyzer只解析资源，Collector只采集目标 | 离线测试不需要Session/Backend Pack；新SoC解析不改McpService/UI |
| P6 | Runtime回合、上下文、存储、Driver和UI各守职责 | Runtime无Widget；Driver不见Manager；工具仅调用P1端口，不include业务实现 |
| P7 | 后端进程/协议适配与产品工具分开 | 新后端不改已有聊天业务；SFTP工具不穿透到Provider私有会话 |
| P8 | 验证组件边界和典型扩展成本 | 记录直接依赖、单独链接及变更范围，不以库数量/行数代替 |

阶段依赖图描述交付先后，不授权跨模块include或链接具体实现。每阶段计划的“低耦合、高内聚验收”是强制退出条件。

## 8. 原项目P8内容整合

原P8的对外细节在[02当前契约](02_Current_MCP_Contract.md)用源码/fixture核对；全部实施/审查/测量记录在[03](03_Implementation_and_Verification.md)，旧设计及优化路线在[04](04_Historical_Design_Archive.md)。03/04分区覆盖原始全部内容，历史“当前/最终”表述不得覆盖当前契约和本阶段结构差距。
原P8.1～P8.6的已落地基础并入mcp/P0；原v0.4性能路线映射mcp/P8；未实现历史/诊断/Agent分别按P2～P7，不能混为已实现。当前契约和历史证据分开维护，保留低耦合、高内聚原则。
