# P2 实施与验证记录

> 历史实施与验证证据；保留原日期、命令、失败和限制。不是当前实施计划，旧 P 编号仅用于追溯。当前路线见 [目录入口](../README.md)。

计划：`P2_History_Recording.md`。2026-10-22 Windows 实现与本机验证完成；第二轮审查的 9 项问题及 I-001 已修复，当前验证见文末增量。阶段发布闸门尚未全部关闭。

## 执行台账

- T1：实现并专项验证。实际填满 8 MiB Parser 队列，确认 pending 非零后排空且只观察一次；Framer remainder 不重录，分流载荷仅报告省略。首次启动/重连代际专项通过。
- T2：实现；存储最终 QTest 25/25，通过最大记录、提交交集、截断恢复、未知 gap、故障注入、权限、租约和删除用例。
- T3：实现；Service QTest 8/8（含启停竞争、第二 owner 拒绝、配额停止），UI QTest 4/4（含离线状态、选中项稳定）；offscreen 预览已检查。
- 最终：独立只读审查已完成，三项问题均 RED→GREEN。Release 完整构建成功。既有 39 项 38/39，新增录制 4/4，合计 42/43；唯一失败为既有 ConPTY 句柄断言。

Pre-flight：T1→T2 传递有序记录/gap，T2→T3 提供已提交前缀和完整性。Session 只提供原始接收/边界事件，不持有存储状态。既有 `novaterm_history` 必须保持无 Qt 链接；新录制目标与其分离。

Ruling: 用户明确要求执行现有设计/计划，沿用文档中的 T1–T3 和退出契约，不重复创建设计审批。当前沙箱拒绝 `.git` 分支写入，在指定工作目录实施并保留修改，不提交或合并。

Ruling: 录制目标使用 `Recording.cmake` 从根工程显式引入，旧 `src/history/CMakeLists.txt` 继续只声明无 Qt scrollback。成本：构建导航多一个文件；避免将 Qt Sql 传递进核心数据后端。

Ruling: 默认读取的 `maxBytes` 计原始 payload，元数据由 `maxRecords` 单独约束（默认 256，硬限 1024）。成本：调用方输出编码另行计额；保证默认预算可返回合法最大 64 KiB 原始记录。

## 实现边界

| 消费者 | 最小契约 / owner | 依赖方向 / 验证 |
| --- | --- | --- |
| SessionInputPump | 分流后、Framer 前 `receiveObserved`，不经重投/remainder | Session → 只读信号；Session 专项 |
| TerminalSession | 接收/省略/连接世代/环境与文件传输边界，无 History 类型 | 组合根按 ID 接入 History；代际专项 |
| HistoryRecorder | 每 capture 单调 ID，16 MiB/全局 64 MiB，固定尾 gap | History 自有队列；纯 Core/Test 测试 |
| HistoryChunkStore / Index | 私有 chunk + 校验链 + manifest；SQLite 仅派生索引 | Qt Core/Sql；不链接 Session/Widgets |
| HistoryService | 16 活动 capture、256 清单、64 请求；独立 worker 所有磁盘状态 | UI/Session 仅交付值；不保存 Session/GUI 指针 |
| HistoryCaptureModel / Panel | 本机意图、快照、错误和完整性 | UI → HistoryService；独立 offscreen 测试 |

记录按 64 KiB 分割，chunk 初值 4 MiB。GUI 不排队 payload 到 Qt 事件队列；接纳只占有有界内存。worker 轮转每 capture 的批次，合并尾 gap 在停止后也能提交。全局预算预留每活动 capture 一条最大记录的接纳空间，饱和会话不能吃光新人额度。

数据完整写入并同步后，临时 manifest 同步、原子替换后才发布提交；索引随后提交。恢复按 manifest 的字节高水位与逐记录 hash 取交集。损坏 manifest 只恢复可校验的 `recovered_unsealed` 前缀。无有效 seal、截断、同步/索引失败、配额停止都保持未知尾部，丢失数量不填零。恢复对象禁止追加。

Windows 使用私有 SID/SYSTEM DACL、拒绝 reparse point、多硬链接和目录重定向，并保持目录句柄防重命名；POSIX 实现使用 owner/0700/0600 和符号链接检查（尚未在本轮平台验证）。创建/恢复时设置权限；常规校验不反复改写 ACL。清理先关闭内部读取缓存，删除包含 WAL/SHM、保留标记和派生 evidence 的整个 capture。

根目录用 QLockFile 单 owner，另一个进程不能把正在录制的目录当停止 capture 清理。worker 在启动时恢复并缓存最多 256 个只读存储句柄，之后分页不再次扫描/rebuild 全 capture；缓存归 owner，清理前释放。页读取限制 payload 256 KiB / 1024 记录，读租约最多 32 个、绝对生命周期 30 秒；到期不能靠续租无限保活。第二轮审查后改为拥有并 join worker：先给 500 ms 排空窗口，再在 I/O 边界协作取消；全部 SQL 对象必须在 QApplication 之前销毁。不可中断系统调用可能延迟最终退出，不宣称严格 500 ms 上界；未封口时下次恢复报告 unknown_tail。

## 审查与修复

独立 reviewer 对整个未提交变更进行只读审查（未代跑测试）。三项正式发现：

- 离线页读取重复全 capture 恢复阻塞录制：`offlinePageDoesNotRescanOrRebuildCapture` 禁止再次扫描时旧实现失败，改用 owner 持有的已验证索引。
- 模型每次 reset 清空选中项：`liveUpdatesPreserveSelectedCapture` 旧实现行号 -1；同 ID 集合改为 dataChanged。
- 默认预算不能返回最大记录：`defaultReadAcceptsMaximumPayload` 旧实现返回空页；按 raw payload 预算收口。

审查保留判断：最终构建/回归以本记录最终结果为准；Linux/macOS、真实断电与真实终端性能继续未验收；串口 consumer 同步销毁泵的生产触发链未证实（实际断开异步），未当确认生产缺陷；可信认证隔离确实未实现且 UI 明示。

Final: fixed 离线全量恢复、刷新清空选择、最大记录默认读取三项问题 — 上述三个命名用例均已观察 RED，最终 recording CTest 4/4，Store 25/25、Service 8/8、UI 4/4。

最终补充：缺失目录的 recover 不创建新目录；unknown_gap 的未知数量不能封成零丢失；SQL 连接在创建它的 worker 上显式关闭，再报告退出，避免共享邮箱在 GUI 最后析构时跨线程清理 SQL。全局饱和仍为新人预留接纳空间。

Windows manifest 替换曾间歇出现 Win32 5（已取得系统错误码），不归因于未确认的特定进程。对 AccessDenied/SharingViolation 在 worker 上按 2/4/8/16/32/64/128/256 ms 有界重试并重新检查路径；永久失败仍未封口。原失败用例连续 20 次通过，未放宽任何完整性断言。

完整构建发现现有 `HistoryGuiScrollBenchmark.cpp` 的派生类隐式删除默认构造函数触发 MSVC `/Wall /WX` C4623；显式 `ProbeRenderer() = delete` 与原语义一致，解除全构建阻塞。

## 冻结的磁盘默认值与依据

Windows Release / Qt 6.8.3 / MSVC 19.51，独立假接收源，每 16 ms 提交 64 KiB，共 64 MiB；不启动主程序、Transport 或真实设备。CPU 使用 GetProcessTimes，内存使用 PeakWorkingSetSize。

| 指标 | 关闭录制 | 开启录制 |
| --- | ---: | ---: |
| 原始输入 / 接纳 | 64 MiB / 64 MiB | 64 MiB / 64 MiB |
| 丢失字节 | 0 | 0 |
| elapsed | 16.3836573 s | 16.3840093 s |
| 合成源吞吐 | 3.9063317 MiB/s | 3.9062478 MiB/s |
| QWidget 重绘事件循环间隔 P95 | 5.4272 ms | 5.5074 ms |
| 进程 CPU | 0.03125 s | 8.203125 s |
| 进程峰值内存 | 19,726,336 B | 21,737,472 B |
| chunk + 索引/WAL/SHM 等目录占用 | 0 | 71,206,829 B |

原始 JSON 及日志见 [Windows 录制基线目录](../../../../../tests/benchmarks/archive/mcp-history/history-recording-windows-2026-10-22/README.md)。目录占用含停止后内部只读索引仍打开时的 WAL/SHM，不能将差额全算作记录格式开销；后续 checkpoint/关闭可能减少占用。

基于这一 **64 MiB / 约 68 MiB 实际落盘** 基线，首版冻结：单 capture **512 MiB**、全局 **2 GiB**、默认期限 **7 天**。这是约 7 倍/30 倍本次目录样本的保守磁盘预算，不承诺在任意速率下录满一天；配额按实际目录占用计量，追加前保留记录/索引余量，达到边界先清无租约、未保留的停止 capture，仍不足则停止当前录制并保持 unknown_tail。保留期限独立可配（UI 为天），长期保留仍计入容量，不覆盖活动分块。

Ruling: 采用上述可配置默认容量与期限，以限制首版隐私暴露时间和磁盘占用；成本：持续大流量会提前停止，用户需按用途提高额度。该基线证明了落盘比例和零丢失样本，不证明性能发布目标。

**性能门槛未关闭**：合成源没有实际 Parser/QRhi 负载，事件循环间隔不是实际终端 GUI P95；开启录制的 CPU 开销明显，不能宣称 CPU/峰值内存 10% 或吞吐/GUI 5% 的真实终端目标通过。没有运行真实 UART、真实终端渲染对照或断电实验；后续发布前须补齐并按结果优化。

## 可复现验证

在 AGENTS.md 的 MSVC 环境中：

```powershell
cmake -S . -B build/Release
cmake --build build/Release -j 8
$env:PATH='C:\Programs\Qt\6.8.3\msvc2022_64\bin;'+$env:PATH
$env:QT_PLUGIN_PATH='C:\Programs\Qt\6.8.3\msvc2022_64\plugins'
ctest --test-dir build/Release -C Release -L recording --output-on-failure
# 独立确认实际背压/代际，Windows QTest 输出写文件
build/Release/bin/novaterm_session_tests.exe recordsOnceAcrossBackpressure recordingLifecycleFollowsConnectionGeneration -o build/p2-session.txt,txt
```

存储测试 dumpbin 依赖只含 Qt6Test/Qt6Sql/Qt6Core 与系统运行库，无 Session/Widgets；旧 `novaterm_history_no_qt_link_check` 在既有回归中通过。windeployqt 实际部署了 `bin/sqldrivers/qsqlite.dll`；未检查跨平台安装包，不据此宣称各平台部署验收通过。

既有 39 项回归（394.02 秒）包括 MCP、Core、Session、Renderer、TerminalSession、UI、文件传输两项与六项纯协议；38/39。最终录制 CTest 4/4（5.44 秒），实际背压/代际专项 4/4（含初始化/清理，707 ms），UI 文件输出 4/4（544 ms）。ConPTY 用文件输出单独核对两条失败：启动回滚句柄 229→266，重复生命周期 288→485；线程 7→6、子进程 0→0 检查通过。按 AGENTS.md 的已知失败规则保留，不修改生产关闭路径、不放宽断言。本轮串口传输两项通过；不把旧 Windows 取消发布失败直接套到本轮。

## 仍需后续验收

- Linux/macOS 构建、权限/恢复、QSQLITE 部署和安装包内容。
- 硬断电、文件系统/设备持久化保证；Windows 使用文件 flush 与 MoveFileEx WRITE_THROUGH，不宣称物理设备断电通过。
- 真实 Parser/QRhi、GUI P95、吞吐、CPU/内存与长稳；本次合成结果不能替代。
- P3/P5 后续消费者必须保留完整性状态、独立授权和证据过期语义；当前未注册历史 MCP 工具。
- 可信认证隔离/privacy gap 尚未实现；UI 明示远端输出可能含秘密，当前不承诺自动脱敏。

## 第二轮审查修复（2026-10-22）

本轮按 `effective-cpp` 修复用户确认的审查问题，保留现有初始化写法；Qt review 技能中的 VAR-3 规则已独立移除。`chrono.count()`、QVERIFY 包裹的 open 检查、需要修改元素的 std::map 遍历属于已核实的静态匹配误报，不做错误替换。有效的 min/max 宏保护、JSON 类型/语法检查也已补齐。

| 问题 | 修复与行为证据 |
| --- | --- |
| D-001 旧 Session ID | 接线集中到 `service/HistoryRecordingBinding`，同一生产函数用于 TerminalPage 和专项；在接收/边界/停止回调执行时取当前 Session ID。绑定后 close/resetForReuse 再接收的用例由 RED 转 GREEN，不再只测静态假面板。 |
| D-002 保留保护丢失 | 保留标记独立于索引恢复加载，恢复失败也保留 kept。低于已有保留数据大小的总配额及故障注入用例证明不会误删。 |
| D-003 Qt SQL 超出 QApplication 生命周期 | 拥有 std::thread，500 ms 排空后设置协作取消并 join，所有 SQL 仍在 worker 上销毁。900 ms 故障探针证明析构不会在 SQL owner 尚未退出时返回。 |
| D-004 索引漏行 | 对请求起点、相邻记录和尾部覆盖逐项核验，兼容合并 gap；首/中/尾索引行缺失均报告 missing/unknown，不再假完整。 |
| D-005 旧连接代际 | 所有后端的 Session 状态/连接变化刷新面板，开始按钮另从实时 resolver 取得当前身份与代际。旧快照为 generation 1、当前 resolver 为 2 的用例由 RED 转 GREEN。 |
| D-006 活动配额旧值 | 配置成功时在同一 worker 同步活动 store 的上限；1 MiB 开始后调高到 2 MiB，接收 1.5 MiB 后正常封口。 |
| D-007 读取错误仍 Lossless | 所有缺失/不安全/读失败路径归一化为 UnknownTail，清空未知丢失数量；不再保留零丢失。 |
| D-008 空闲递归扫描 | 只在实际存储变更后更新目录计量；已提交后空闲观察期间 DirectoryScan 计数不再增加。 |
| D-009 分页成本随历史增长 | 单次主键前驱定位可能覆盖起点的 gap，然后以 id 双边界有界查询；真实语句的 SQLite EXPLAIN 断言同时使用 rowid 上下界。读取只验证访问的目录/分块，不递归扫描全部历史。 |
| I-001 异步配置默认值竞态 | 加载完成前禁用表单与开始，快照和 completion 在同一锁内发布；加载后同步保存值，保留未提交编辑，提交期间禁止继续编辑。加载竞态、有效值编辑保留及尚未形成有效数字的 textEdited 部分编辑用例通过。 |

上述回归已先观察原实现失败，再验证修复通过。独立只读复核确认这批修复未发现新的 >80 置信度问题；复核不代替运行测试。

### 退出限制的明确修正

旧“500 ms 后 detach，迟到 worker 只有值状态”的说法不成立，因为它仍拥有 QSqlDatabase。当前不再 detach；不能同时在本进程中强制结束任意阻塞系统 I/O 又保证 Qt SQL 生命周期。选择先保证 SQL 在 QApplication 前收口：500 ms 是排空窗口，不是进程退出硬上限。底层不可中断系统调用可能延迟最终退出；如需严格时限，必须进一步隔离存储进程。设计任务清单相应保留此闸门，不将其勾选为已完成。

### 验证中新发现的 Windows 删除共享竞争

初次跨模块回归（排除已知 ConPTY）为 41/42，存储目标有一次间歇失败。单跑 31/31 与随后 8 轮通过，不能据此清除失败记录。加入每个录制 CTest 的 QTest 文件日志后，复验先定位清理用例，随后又捕获派生索引替换失败 `cannot replace derived index`。

两条真实 Windows 句柄夹具分别打开 chunk/index 文件并暂时拒绝 FILE_SHARE_DELETE，旧代码均可稳定复现相同失败。现将文件清理/索引重建/保留标记删除收敛到私有文件清理边界，目录清理同样有界重试；每次删除前重新检查路径，活动捕获、租约和保留策略不放宽。Windows 使用 2/4/8/16/32/64/128/256 ms 有界重试，永久失败仍报错。未将实际瞬态占用归因于未经确认的特定外部进程。

修复后，存储 CTest 按原调用方式 `--repeat until-fail:20` **连续 20 轮通过**（66.36 秒），最终存储 QTest **33/33**。前两次失败的回归日志与两个确定性 RED 夹具保留在本轮证据目录。

### 最终结果与命令

- 完整 Release 构建成功。
- 最终相关 CTest **5/5**（26.56 秒）：完整 Session + 四项录制；Store **33/33**、Service **13/13**、UI **8/8**。
- 广泛回归其余 41 项通过；存储间歇失败在本轮定位并修复，按相关目标重跑，未重复全部无关目标。已知 ConPTY 目标本轮按 AGENTS.md 排除，不记为通过。
- 不启动主程序、不自动重连设备、不连接用户 SSH 服务器。其他平台、真实渲染/CPU 目标及断电验收继续未完成。

```powershell
cmake --build build/Release -j 8
ctest --test-dir build/Release -C Release -E '^novaterm_conpty_tests$' --output-on-failure
ctest --test-dir build/Release -C Release -R '^novaterm_history_store_tests$' --repeat until-fail:20 --output-on-failure
ctest --test-dir build/Release -C Release -R '^novaterm_(history_(recorder|store|service|recording_ui)|session)_tests$' --output-on-failure
```

本轮原始记录见 [审查修复证据](../../../../../tests/benchmarks/archive/mcp-history/history-recording-review-fixes-windows-2026-10-22/README.md)。中期合成 64 MiB 复验零丢失，但不据单次合成数据宣称 CPU 目标改善或真实性能发布闸门通过。

## 基线

- 2026-10-22 Windows 现有 `build/Release`，Session CTest 在 DLL 初始化时退出 `0xc0000142`，无断言输出；不是通过结果，重建后复核。

## 当前限制

- 未验收的平台/性能/断电恢复不得由单元测试结果推断。
- 没有可信认证隔离适配器，不保证屏蔽远端秘密回显；录制与共享历史是独立意图。
