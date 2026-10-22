# mcp/P2 保真历史录制与存储实施计划

> For agentic workers：实施使用 superpowers:executing-plans；按下列复选任务逐步验证。

**Goal:** 用户主动开启、非阻塞、有界、可恢复且可管理的终端接收流录制。
**Architecture:** InputPump 单次只读观察→有界队列→worker 分块写入→已提交高水位与 SQLite 索引。历史不使用去重摘要/Scrollback 代替。
**Tech Stack:** C++17、Qt Core、后台文件 I/O、SQLite 元数据索引（新增依赖须显式配置）。
**Spec:** [总体架构](00_Overall_Architecture.md)；依赖 [P1 资源/授权](P1_Capability_Runtime.md)，输出供 [P3](P3_History_Search_and_Evidence.md)。

## Global Constraints

录制默认关闭；开启录制不自动共享，读取历史另授权。不记录本地输入/密码缓冲/认证私有通道，不承诺自动识别远端秘密。不能阻塞 GUI、Parser 或运输关键线程；文件传输二进制不是本录制范围。SQLite 只作元数据索引，不假定二进制可用 FTS。

所有模块必须遵守[总体架构](00_Overall_Architecture.md)§2.1～§2.4的低耦合、高内聚规则；下列专项结构验收是本阶段退出条件。

## Review Focus

- 背压重投同一字节重复录制：T1 在分流后/Framer前仅观察一次。
- 队列末批丢失后没有新 payload：T1/T2 停止/恢复也写尾部 gap。
- chunk 已写但索引未提交/部分写入：T2 仅公开校验后的提交前缀。
- 磁盘满和慢盘读者无限阻止清理：T2/T3 固定内存、租约与显式丢失。
- 远端敏感回显和路径重定向：T3 权限/拒绝符号链接；适配器未实现不声称 privacy gap 已保护。

## 文件与公共契约

新建 src/history/HistoryTypes.h、HistoryRecorder.*、HistoryChunkStore.*、HistoryIndex.*、HistoryService.*、HistoryRetentionPolicy.*、CMakeLists.txt。
修改 src/session/SessionInputPump.*（先审查挂点）、TerminalSession.*（录制生命周期）；新增 src/ui/models/HistoryCaptureModel.*、src/ui/widgets/HistoryRecordingPanel.*；修改根/测试 CMakeLists.txt。
新增 tests/history/HistoryRecorderTests.cpp、HistoryStoreTests.cpp，目标 novaterm_history_store_tests；测试夹具只用临时私有目录。

接口规划：
- HistoryRecord：HistoryCaptureId、SessionId、connectionGeneration、独立单调 recordId、monotonicTimestampNs、wallTimestampMs、QByteArray payload。
- GapRecord：缺失 ID/字节数/原因/时间/连接边界，独立事件而非只靠下一条 gapBefore。
- HistoryRecorder::submit(HistoryRecord) → SubmitResult（Accepted/GapRecorded/Disabled），仅有界入队，不等待磁盘。
- HistoryService::start/stop(RecordingIntent) → async RecordingResult；start 仅由本机用户意图，MCP 不默认开启。
- HistoryChunkStore::readCommitted(HistoryRange) → 有界不可变记录；恢复返回 validated high-water mark。
- HistoryIndex 维护 record→chunk offset、时间、hash、状态；元数据不得反向成为字节真实性依据。

目录：
~~~text
history/<capture-uuid>/
  manifest.json
  index.sqlite
  chunks/00000001.bin
~~~

设计初值：chunk4MiB、单Session队列16MiB、全局64MiB。原草案要求磁盘总额、单capture额和保留期限都有可配置默认值，但未给出数值；T3必须基于T2磁盘基线冻结默认值并记录依据，在此之前不得将录制能力标记可发布。
达到上限先清理无活动租约的最旧停止 capture；仍不足则停止当前录制并记录原因/gap，不静默覆盖活动分块。活动读取租约有界，到期可清理并使续读报告缺失。

> 2026-10-22：下列实现/专项复选项已按 Windows 本机证据核对；勾选不表示跨平台、断电或真实性能发布门槛通过。具体结果、默认容量依据及保留闸门见 [实施与验证记录](P2_Implementation_and_Verification.md)。

## T1：采集挂点和有界接纳

**Consumes:** Session lifecycle、P1 ResourceBinding；**Produces:** 保真 HistoryRecord/GapRecord。
- [x] 写 preservesRepeatedAnsiCrLfNullInvalidUtf8、recordsOnceAcrossBackpressure、omitsFileTransferWithBoundary、tailDropProducesGap，断言拼接原始字节完全相等。
- [x] 注册测试并验证缺失采集实现导致失败。
- [x] 审查实际 InputPump 文件传输分流、Framer、Parser 顺序；仅在分流后/Framer前接只读观察，forwardFramerRemainder/重投不重复入队。
- [x] 接纳点分配独立 recordId；超限不等待，写可合并但有界的 gap 元数据；跨重连/boot 边界保留标记。
- [x] 启停/Session销毁/重连有显式收口；停止时即使没有后续 payload 也记录尾部缺口。
- [x] 跑 store 与 Session/文件传输相关测试，证明普通输入路径未改变。

## T2：分块提交、索引和恢复

**Consumes:** 有序 records/gaps；**Produces:** 可验证持久前缀和只读范围。
- [x] 写 partialRecordTailNotPublished、indexLagsChunkRecovers、corruptIndexRebuilt、diskFullIsBounded、queuedBytesLostAreReported。
- [x] 实现带长度/recordId/hash 的 chunk 记录；完整写入/提交后再发布索引，manifest 持已提交高水位。
- [x] 崩溃恢复扫描校验尾部并重建索引，丢弃半条记录，不承诺恢复尚在队列的字节。
- [x] SQLite/WAL 与 chunk 不是跨文件原子事务；使用故障注入逐点中断验证。
- [x] 本项目当前无 Qt Sql 依赖；若用 Qt SQL，明确 find_package/link Qt6::Sql 并与 SDK 版本一致，或明确独立 sqlite3 依赖，不依靠传递链接。
- [x] 后台索引/文件 I/O 不反向持 Session/GUI 指针；按批次/记录协作取消，worker 释放全部 SQL 后再销毁 QApplication。
- [ ] 不可中断的系统 I/O 的严格退出时限：当前给 500 ms 排空窗口后取消并 join；系统调用未返回时保留 Qt 运行时等待，不再宣称 detach 可安全提供硬时限。隔离存储进程才能进一步关闭此闸门。

## T3：保留策略、私有存储与管理 UI

**Consumes:** RecordingIntent、配额、用户删除/保留选择；**Produces:** 状态与离线 capture 清单，不自动授权。
- [x] 测 globalQueueDoesNotStarveOtherSession、leaseExpiresBeforeCleanup、symlinkRejected、deleteRemovesWalAndEvidenceDependencies。
- [x] 私有目录/分块/临时文件拒绝符号链接重定向；删除涵盖 WAL/SHM 和后续派生证据索引。
- [x] 根据磁盘基线冻结全局/单capture容量与保留期限默认值并记录依据，再落实配额/期限/读租约与 stopped/disk_full/gap 状态；清理后的读取不能跳过缺失范围伪装完整。
- [x] Ela UI 提供开始/停止、缺口、容量、删除和保留设置；模型仅发本机意图。
- [x] 后续可信认证隔离才能暂停敏感输出并记 privacy gap；当前无适配器时明确尚无该保证。
- [x] 运行相关 UI/存储测试；不启动可能自动重连真实设备的主程序做自动验收。

## 验证与退出

~~~bash
cmake -S . -B build
cmake --build build --config Debug --target novaterm_history_store_tests
ctest --test-dir build -C Debug -R novaterm_history_store_tests --output-on-failure
~~~
新目标加入后才执行。涉及 InputPump 另跑 Session/对应文件传输专项。性能按 P8 开关录制对照吞吐、GUI P95、CPU/峰值内存；5%/10%是目标非成绩。退出要求保真/队列/恢复/权限/清理全部有证据，P3 可只读消费已提交范围。

## 低耦合、高内聚验收

- History内聚录制接纳、提交、恢复、配额与回收；Session仅向有界接纳端口提供字节/边界，History不反查Session或操纵终端。
- 存储/索引/时钟仅在真实I/O边界可替换，不为了每个内部算法增加虚接口；公开结果不泄露SQLite连接、文件句柄或存储内部容器。
- [x] 独立链接录制/存储测试，不需要Session/Widgets/Agent/诊断Provider；用假字节源验证关闭或替换录制不改变普通终端流程。
- [x] worker停止、磁盘失败与gap由History owner收口，不让UI或Session同时维护另一份录制状态。

## 补充契约：持久提交与异常尾部

选择批量持久提交而非逐record同步磁盘。worker先写完整chunk记录并同步数据，再以临时manifest写入并原子替换提交高水位，完成平台要求的文件/目录持久化后发布committed范围。SQLite仅为可重建索引，可落后于manifest；不得先用索引推进可读高水位。平台不能提供所需持久性时记录限制，不宣称断电恢复通过。

区分accepted（仅入队）、written（写入但未确认持久）、committed（持久提交点已发布）。readCommitted只返回committed前缀。恢复以有效manifest高水位和逐record校验共同限制前缀；高水位后即使有完整记录也不自动升级为已提交，首版忽略该尾部。manifest损坏时重建可校验前缀并标recovered_unsealed，不将重建前缀当原提交证明。

异常恢复后不向旧capture追加新接收数据；重新录制分配新HistoryCaptureId并记录关联边界，防止未持久recordId被复用。

Capture在接纳首条数据前持久写入open标记。stop先停止接纳、排空可提交数据/gap，再持久写入seal（最终高水位、停止原因、已知gap摘要）；仅有与数据一致的有效seal才是正常封口。正常封口也可能有已知gap，不等于无丢失。

无seal、半条尾部、同步失败、进程崩溃或磁盘满无法记录尾部时，恢复结果为unknown_tail：最后可验证边界明确，lostRecordCount/lostByteCount允许null，完整性为unknown。不得填0或宣称完整。恢复标记可先作为内存状态返回，存储恢复可写后再持久化；不依赖故障发生时一定还能写GapRecord。GapRecord支持known_gap和unknown_tail，后者只提供已知起点与未知终点。

- [x] T2增加crashBeforeSealReportsUnknownTail、diskFullWithoutGapWriteStillUnsealed、writtenBeyondManifestNotPublished、sealWithGapIsNotLossless。
- [x] 分别在数据写入/同步、manifest替换、索引提交、seal持久化处注入失败；正常停止有seal，异常恢复不出现“丢失0字节”的假完整结果。
- [ ] P3读取/证据、P5时间线、管理UI携带完整性状态；消费者不能通过去掉尾部gap将未知变成完整。P2结果/管理UI已携带并测试；P3/P5消费者尚未实现，随对应阶段验收。
