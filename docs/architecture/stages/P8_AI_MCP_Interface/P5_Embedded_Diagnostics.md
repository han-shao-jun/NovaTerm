# mcp/P5 Embedded Linux 诊断实施计划

> For agentic workers：实施使用 superpowers:executing-plans；离线与在线任务分别验收。

**Goal:** 分析Boot/Kernel/DDR日志，并在已识别Linux目标上采集驱动、设备树和PCIe只读证据。
**Architecture:** 输入资源/原文范围→有界流式Analyzer→Finding/Evidence；在线Collector复用P4 Recipe与P1路由，不把解析器写进McpService。
**Tech Stack:** C++17、Qt Core、P3资源/证据、Qt Test；可选显式符号资源。
**Spec:** [总体架构](00_Overall_Architecture.md)；离线依赖 [P3](P3_History_Search_and_Evidence.md)，在线依赖 [P4](P4_Backend_Diagnostics.md)。

## Global Constraints

先离线再在线，不要求目标有systemd/lspci/debugfs/perf或完整BusyBox。Bootloader不是POSIX，Unknown/U-Boot零自动Linux探测。不读任意BAR/MMIO，不默认JTAG/TRACE32，不执行AI生成的调试器命令；读取寄存器也可能read-to-clear。符号不匹配不输出虚假源码行。

所有模块必须遵守[总体架构](00_Overall_Architecture.md)§2.1～§2.4的低耦合、高内聚规则；下列专项结构验收是本阶段退出条件。

## Review Focus

- 跨boot/gap/乱码导致虚假因果：T1保留边界/不确定性。
- vmlinux/ko仅uname相同但Build ID不匹配：T2拒绝精确定位。
- 大日志一次复制到QString：T1流式/分段/取消与内存上限。
- 私有DDR格式污染通用解析：T4独立Parser，未识别平台报unknown。
- UART从Linux回Bootloader后旧Recipe仍可执行：T3环境revision失效、零POSIX输入。

## 文件与接口

新增 src/diagnostics/embedded/BootLogAnalyzer.*、KernelCrashAnalyzer.*、KernelSymbolResolver.*、DriverInspector.*、DeviceTreeInspector.*、PcieInspector.*、DdrLogAnalyzer.*、IDdrLogParser.h、CMakeLists.txt。
公共LogInput/AnalyzerOptions/Finding放 src/diagnostics/common/；修改Registry、CommandPlatformProfile和MCP adapter。
测试 tests/diagnostics/embedded/EmbeddedDiagnosticsTests.cpp、fixtures/（脱敏），目标 novaterm_embedded_diagnostics_tests。

公共语义：
- LogInput：已授权Evidence资源的有界分段读取器、encoding/boot/gap/时间元数据；不持任意文件路径。
- EvidenceRange：source、resourceId、原始record/byte区间和contentHash。
- Finding：code、severity、summary、evidence、limitations；不将模型解释当证实根因。
- ILogAnalyzer::analyze(LogInput, AnalyzerOptions)→有界Findings，支持流式/取消；具体同步/异步适配在worker边界。
- KernelSymbolResolver::resolve(AddressQuery, SymbolResourceSet)→Match/Unknown，返回匹配依据。
- IDdrLogParser：独立平台格式识别与解析契约；未知格式不强行套用其他平台。

| Capability | 输入/输出 |
| --- | --- |
| boot.analyze | UART/Boot原文→阶段、切换点、失败/缺失证据 |
| kernel.crash.analyze | Oops/Panic日志+可选符号→异常/调用链/限制 |
| driver.inspect | 已识别Linux→绑定、probe和defer |
| devicetree.inspect | 运行时DT+可选显式DTS/DTB→节点/资源/差异 |
| pcie.inspect | sysfs/lspci/日志→BDF/Link/BAR/MSI/AER观测 |
| ddr.training.analyze | 平台训练/恢复日志→阶段、频点、Byte/Rank/Channel |

拟新增统一 novaterm_analyze_log，P4 list_diagnostics/run_diagnostic复用；不按SoC增MCP工具。分析器只需要对应资源读取授权，不需要终端执行权限。

## T1：Boot与Kernel离线分析

**Consumes:** P3授权原文/导入；**Produces:** Finding+原始EvidenceRange。
- [ ] fixture覆盖ARM64 Oops/Panic、CallTrace、RCU stall、hung task、watchdog、probe defer、乱码、重复异常、跨boot和缺口。
- [ ] 写 segmentedAnalysisMatchesWholeFixture、cancelStopsBoundedWorker、gapDoesNotCreateFalseTimeline，再验证失败。
- [ ] 实现分段状态保留和事件定位，限制单批输入/Findings/上下文，不整段加载几GB。
- [ ] 单调时间只描述本机采集顺序；跨设备不把接收时间当设备绝对事件时间。

## T2：可选符号定位

**Consumes:** 用户明确共享vmlinux/System.map/ko资源；**Produces:** 地址/符号匹配证据。
- [ ] 测 mismatchedBuildIdNeverReportsLine、missingModuleKeepsRawAddress、unameReleaseAloneNotTrusted。
- [ ] 核对Build ID/模块身份，不只比uname；不匹配保留原PC/LR/异常地址与Unknown。
- [ ] 若使用外部解析器，进程参数结构化、超时/输出有界，禁止从日志/AI拼GDB/TRACE32命令。
- [ ] 解析器只能读取已共享资源，不能顺路径读取任意本机文件。

## T3：在线只读采集

**Consumes:** P4受限Recipe、P1环境/路由；**Produces:** 采集结果与共享/独立来源证据。
- [ ] 测 busyboxMissingLspciIsUnavailable、bootloaderHasNoLinuxRecipe、environmentResetInvalidatesPendingInspection。
- [ ] 先Driver/sysfs→运行时DT→PCIe/sysfs/lspci/interrupts/log，缺字段标未知，不虚构速率/中断方式。
- [ ] DTS/DTB比对使用独立导入资源，在线和离线授权不能相互替代。
- [ ] 共享终端执行继续遵守占用同意、提示符、用户接管和未知结果，无输入旁路/自动重试。

## T4：DDR扩展与统一入口

**Consumes:** 独立平台日志Parser；**Produces:** 平台Finding和MCP统一analyze入口。
- [ ] 测 unknownDdrFormatStaysUnknown、platformParserIsolation、partialTrainingReportsLimitations。
- [ ] 首期仅日志，Synopsys PHY/SoC厂商等规则独立注册；不把私有字段写进Service或通用Kernel分析。
- [ ] 注册已实现analysis ID，查询目录按资源/目标权限显示可用性。
- [ ] 端到端夹具：UART历史找Panic→查前序PCIe→符号匹配→输出证据、未采集区间和时间关系限制。

~~~bash
cmake --build build --config Debug --target novaterm_embedded_diagnostics_tests
ctest --test-dir build -C Debug -R novaterm_embedded_diagnostics_tests --output-on-failure
ctest --test-dir build -C Debug -L mcp --output-on-failure
~~~
退出：离线可独立交付；在线范围按实际Profile/传输验证。真实板卡、helper安装、JTAG/MMIO另立强授权阶段，不在此计划自动开展。

## 低耦合、高内聚验收

- Analyzer只解析有界LogInput并输出Finding；Collector只采集目标；符号解析和DDR平台解析按真实独立规则划界，不拼成单个EmbeddedManager。
- 离线Analyzer不依赖Session、Backend Pack、HTTP、Widgets或History磁盘格式；输入读取/符号资源均经窄端口提供。
- [ ] 离线fixture测试可在不链接Session/Backend Provider的目标中运行；切换History reader或新增SoC Parser不改MCP工具和聊天UI。
- [ ] 解析状态、取消、分段上限由Analyzer/任务owner收口，不能让Collector和UI同时维护相同解析进度真源。

## 补充实施约束（设计补齐，尚未验收）

- 离线LogInput携带sourceCompleteness与projection_gap；unknown_tail不等于日志已完整结束，禁止据此断言“后续无异常”。T1增加unsealedCaptureCannotProveNoLaterCrash。
- T3在线Recipe消费[P1环境事实与失效契约](P1_Capability_Runtime.md#补充契约环境事实与失效)，不能仅凭曾见Linux日志认定当前环境；没有新鲜就绪证据时零输入。
- 离线子集依赖P3已实现的导入/授权/Evidence来源端口；不依赖P4全部Provider，也不要求先完成历史搜索UI。符号资源与报告发送给模型仍需独立发送许可。
