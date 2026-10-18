# mcp/P8 集成、性能与跨平台验收实施计划

> For agentic workers：实施使用 superpowers:executing-plans；逐范围验证，未执行项目不得标通过。

**Goal:** 证明九工具兼容、新能力安全与资源有界，完成可追溯的平台/性能/端到端验收。
**Architecture:** 状态机/codec单测→模块集成→本机隔离协议/终端fixture→获准平台/真目标验收，各层分别记录。
**Tech Stack:** C++17、Qt Test/CTest、回环HTTP、fake模型/Agent、脱敏日志/隔离存储。
**Spec:** [总体架构](00_Overall_Architecture.md)、[统一完成定义](01_Phases_and_Acceptance.md)，贯穿P0～P7。

## Global Constraints

此次文档迁移不连接真实UART/SSH、不安装helper/hook、不自动登录/调用付费模型。fixture不代替真平台/GUI验收。默认按AGENTS.md“改哪测哪”，共享接口/依赖/多模块变更才跑全套。代码已实现、测试通过、性能达标分别报告。

所有模块必须遵守[总体架构](00_Overall_Architecture.md)§2.1～§2.4的低耦合、高内聚规则；下列专项结构验收是本阶段退出条件。

## Review Focus

- Linux成绩覆盖Windows/macOS：T3逐平台独立证据。
- openpty环境失败靠删测试掩盖：T1保留分类，不能宣称全绿。
- CPU提交耗时冒充端到端GUI延迟：T2先定义口径和重复对照。
- 迟到worker触碰析构UI：T1/T4故障注入/有界关闭。
- 只看注册数/编译：T1对象级Schema+行为矩阵。

## 文件、目标与证据

保留tests/mcp/*、tests/session/*、tests/CMakeLists.txt和九工具fixture。
新目标按实际阶段创建，当前尚未注册：
- novaterm_capabilities_tests；
- novaterm_history_store_tests、novaterm_history_search_tests；tests/evidence/ → novaterm_evidence_tests；
- novaterm_backend_diagnostics_tests、novaterm_embedded_diagnostics_tests；
- novaterm_agent_runtime_tests、novaterm_agent_ui_tests、novaterm_external_agent_tests；
- tests/diagnostics/DiagnosticIntegrationTests.cpp→novaterm_diagnostic_integration_tests；
- tests/benchmarks/mcp/（规划）录制开关、stream、退出和容量夹具。

结果归档为本目录后续dated验收记录，各阶段链接；不覆盖上级P8历史。
当前九项名称见阶段划分。注册数以ctest -N为准，不混淆QtTest内部case数与CTest目标数。

## T1：兼容与故障矩阵

**Consumes:** 旧fixture和新契约；**Produces:** 可重复行为/缺口报告。
- [ ] Schema逐对象比较，accepted/rejected/default/未知字段/UTF8/错误；2025/2026确认/重提/篡改/过期/断流/重复ID。
- [ ] 五类LocalShell/SSH/Serial/Telnet/Custom基础text/key/paste/观察；无Profile不禁自由输入，也不伪造exec/SFTP/提权。
- [ ] stale UI/版本/旧绑定撤销、用户抢占、operationId精确重试、提示符编码回滚、多行尾部、TUI未知。
- [ ] full/compact转换、projectionLimited/sourceGap、UTF8极小预算、到期/FIFO/补读进度、最终8KiB双payload。
- [ ] 发布中取消/撤权/迟到、Published-not-invoked/Unknown、预留移交、quarantine跨重启。
- [ ] Raw History重复/ANSI/CR/NULL/非UTF8、跨块搜索原始偏移、尾部gap、磁盘满/索引损坏/崩溃恢复/租约。
- [ ] Session关闭后的独立CaptureRead、缩范围/轮换失效、证据过期、未录制时内存任务证据。
- [ ] Unknown/U-Boot零探测、同连接环境切换、共享终端TUI/传输拒绝、诊断独立授权/符号不匹配。
- [ ] Agent分片前零执行、同chat互斥/同terminal仲裁、批准迟到、413后零重复、UI隐藏/恢复不重放。
- [ ] 凭据方法/目标隔离、状态查询零取密、保存失败/跨实例撤权；未验收适配器仍不可用。
- [ ] Session/UI/Runtime销毁+worker迟到，结果只完成一次，无UAF/无限GUI join。

## T2：性能与容量

**Consumes:** 同硬件/构建/负载录制开关对照；**Produces:** 吞吐/GUI P95/CPU/内存/磁盘/队列/失败率报告。
- [ ] 记录硬件、OS、Qt/编译器、生成器/配置、数据量/编码/模拟方式、采样定义、重复次数。
- [ ] off/on同负载对照丢帧/gap、吞吐、GUI帧时、CPU、峰值内存/队列/磁盘，不只平均。
- [ ] 初始目标：录制吞吐下降≤5%，GUI frame P95增长≤10%；MCP有效响应率99%仍是未完成闸门。
- [ ] 旧有效响应率94.2%不是新成绩；旧GUI口径未闭合，先定义端到端，不能仅统计CPU提交。
- [ ] 历史默认单响应≤64KiB、页扫描2秒/16MiB；任务/归档/缓存/HTTP队列测硬限和超额拒绝。
- [ ] 高速模型stream、隐藏侧栏、长消息/工具组、停止退出，文本批处理不能无界推迟批准/终态。
- [ ] 单调时间用于到期/授权，墙上仅显示；记录跨设备时钟限制。
- [ ] 不达标如实标记，不删缺口/改断言求通过。

## T3：构建与平台

**Consumes:** 生产依赖和测试清单；**Produces:** 各平台通过/失败/未执行。
- [ ] Linux GCC/Clang、Windows MSVC、macOS独立构建；CMake≥3.20、Qt≥6.8、C++17。
- [ ] Windows显式-C并遵守Qt Test/offscreen插件配置，沿用现有生成器/配置。
- [ ] SQLite/sidecar若新增，验证安装包依赖，不只开发机编译。
- [ ] 默认CI仅loopback/fake Agent/模拟SSH或串口/脱敏fixture；不得真实账号/设备/自动重连。
- [ ] 真实Shell/TUI、设备、部署和认证适配器、Windows文件竞争另有授权与实测，未跑明确列出。

历史P8 W5l：Linux MCP九项9/9（28.36秒），全套27/29（190.26秒），两项为openpty环境失败。不是本次重跑，不据此声称Windows/macOS通过。

## T4：端到端与退出

**Consumes:** 已交付阶段；**Produces:** 场景、结构删除、平台限制汇总。
- [ ] nginx启动失败，只诊断：Profile/授权→systemd+journal→证据/限制；SSH独立和串口Linux共享分别验收，未录制说明期限。
- [ ] UART最近Panic→前序PCIe→Build ID符号→原文证据/时间关系/gap；符号不匹配保持未知。
- [ ] U-Boot读/获准输入→Linux新环境/就绪后诊断→重启Bootloader旧Recipe失效；不新连接/输入旁路。
- [ ] Agent选区提问→模型→批准→结果→停止/接管；换标签不串目标，fork/重启不重放。
- [ ] shutdown停接纳→撤未提交→有界收口→释放已证实可释放资源；保留unknown保护，不把退出当远端停止。
- [ ] 汇总P0结构差距/删除清单、九工具fixture、新工具版本、测试/性能/未验收范围，满足统一定义才更新状态。

## 命令

~~~bash
cmake --build build --config Debug
ctest --test-dir build -C Debug -N
ctest --test-dir build -C Debug -L mcp --output-on-failure
ctest --test-dir build -C Debug -R "novaterm_(capabilities|history_.*|evidence|backend_diagnostics|embedded_diagnostics|diagnostic_integration|agent_.*|external_agent)_tests" --output-on-failure
~~~

新目标不存在不能假装已运行；仅综合验收或AGENTS.md指定场景跑全套：
~~~bash
ctest --test-dir build -C Debug --output-on-failure
~~~
最终报告分别列代码、行为、性能、平台，不能只说测试通过。

## 低耦合、高内聚验收

- 架构验收与功能/性能验收分列；仅编译成功、目录分层或文件缩短不能证明低耦合高内聚。
- [ ] 从公开头和CMake目标直接依赖建立有向图，检查环、反向依赖、实现库经PUBLIC泄露以及无消费者接口；保留图和拒绝原因作为验证证据。
- [ ] 按阶段边界独立链接核心测试，证明无需HTTP/UI/真实Session/模型进程即可验证各自不变量。
- [ ] 完成典型替换演练：模型后端、历史存储/reader、诊断Analyzer、UI观察者；记录修改模块，公共语义不变时不应波及其他业务实现。
- [ ] 逐一核对状态owner、巨大Ports/共享可变Context/全局服务定位器和隐式事件命令；拆散到多个类仍共同维护一个状态视为未达标。

## 原P8性能路线与度量补充

原文保留在[04历史档案](04_Historical_Design_Archive.md)§9；下面是本阶段继续验证的目标，非本次实测。不同对照口径分开，不能择优汇总：
| 场景 | 负载/目标 |
| --- | --- |
| MCP正常设计负载 | 同会话新采样总需求≤4Hz、最多4客户端共享；有效响应率≥99%，成功RPC P95≤100ms，GUI捕获/快照取得P95≤2ms |
| MCP关闭/开启对照 | 相同负载终端吞吐下降≤5%，GUI frame P95增量≤2ms；与现有录制off/on目标不是同一实验 |
| 录制off/on对照 | P2新增录制吞吐下降≤5%、GUI frame P95增长≤10%，独立记录缺口与磁盘/队列 |
| 过载 | 例如4客户端各10Hz，允许Busy但队列/内存/CPU有界；逐客户端success/Busy与最长无成功间隔，不用Busy低延迟充当读取性能 |
| 探索目标 | 原≤2～3%吞吐下降、16～32KiB共享块等仅A/B候选，有profile后才调整，不改变5%退出门槛 |

- [ ] 分别记录coreCaptureCount（含try-lock失败）、snapshotPublishCount、snapshotReuseCount、coalescedReadCount、snapshotCopiedBytes、projectionCopiedBytes、jsonEncodedBytes、allocationCount、snapshotAgeP50/P95、perClientLongestNoSuccessMs，声明实际已有/需补采样的指标。
- [ ] 优化次序：同session/epoch/revision基础快照复用→250ms窗口合并→capture/search共享不可变投影→预算裁剪/UTF-8少遍扫描→同DTO输出两兼容副本；每步独立A/B，不提高锁等待或建立无界队列。
- [ ] 一个客户端的projection/token进度不复用为另一个客户端进度；所有共享读取分别校验权限/epoch/预算。快照年龄如实显示，不能长期返回旧数据换成功率。
- [ ] 不在当前HTTP-only实现恢复旧IPC/CBOR优化；线程池、lock-free、SIMD、全文索引、共享内存等无profile证据不进入主线。
- [ ] Parser、Renderer和UI终端流不因MCP输出限额截断或新增背压；复制字节/临时JSON/DTO/worker开销单独计，不用文本上限冒充总内存。

## 补充验收矩阵：跨阶段契约

下表是2026-10-18设计补齐后的未来验收要求，不是新增通过记录。对应测试随所属阶段实现；只读交付按阶段划分§6.1验证其子集，不提前运行尚不存在的测试目标。

| 契约 | 所属阶段/检查 | 判定依据 |
| --- | --- | --- |
| 模型发送 | P6；P7外部resume | fake网络/进程捕获实际请求体；本机读取不默认授予发送，撤权/接收方变化后正文及派生摘要不再发送；不跨origin转发凭据 |
| 环境失效 | P1、P4/P5 | 普通输出不能建立就绪；过期、人工接管、旧适配器序号拒绝；无可信适配器的一次确认不能被二次提交复用 |
| 持久完整性 | P2、P3/P5消费 | 数据/manifest/index/seal各点故障注入；未正常封口返回unknown_tail和可空缺失计数，磁盘满无法写gap仍不谎报完整 |
| 搜索连续性 | P3 | 在UTF-8/ANSI/匹配中间耗尽预算；跨页结果一致、重复页不前进、query变化拒绝、过期不自动重扫；超长行显式不完整 |
| 类型化扩展 | P1 | 用读取和fake日志分析两类端口证明共享准入/取消，新增领域不改中心variant或Dispatcher业务分支 |
| 增量交付 | P0/P1/P6 | A/B/C/D分别列公开能力和未完任务；早期只读不暴露执行，批准执行不依赖未实现的历史Pack |

- [ ] 预算验收补齐SearchSession状态总量、1MiB单状态、上下文来源集合1024项上限；达到上限明确拒绝/收窄，不静默丢失授权来源或扫描状态。
- [ ] 文档验收检查本目录及上级入口相对链接；当前物理路径为P8_AI_MCP_Interface/，mcp/Pn仅为逻辑阶段编号。
