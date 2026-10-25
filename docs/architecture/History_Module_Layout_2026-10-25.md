# 历史模块目录与构建边界重构（2026-10-25）

用户要求按职责分开，MCP 可以引用这些独立模块。本轮只调整源码/测试目录、include 和构建注册，不改线程、业务行为、MCP 契约、配置键或持久格式；保留此前 B0/B1 工作区内容，不提交/推送、不连接设备。

## 当前归属

| 目录 | 责任/目标 | 依赖边界 |
| --- | --- | --- |
| `src/history/` | 内存终端历史/LZ4 codec、后台预算与冷读，`novaterm_history` | 纯 C++，不查找 Qt；与 core/scrollback 的块/快照/日志代码共同编入后端 |
| `src/recording/` | 持久接收流录制、块文件、SQLite 索引、保留/配额，`novaterm_history_recording` | Qt Core、私有 Qt Sql/Threads；不依赖 MCP 或 LZ4 |
| `src/historysearch/` | 离线授权、分页读取、字面搜索和投影，`novaterm_history_search` | Qt Core；仅引用 recording 值类型，通过 HistoryReadPort 注入源，不链接具体录制后端 |
| `src/resources/RecordingHistoryReadAdapter.h` | 具体 HistoryService → HistoryReadPort 接线 | 应用/MCP 组合根使用；不把具体存储适配混入纯查询模块 |

MCP 可以经 Core/Session 读取当前终端缓存，或引用独立离线查询模块。界面也可以使用这些模块；源码不因 MCP 消费而归入协议模块。既有 `NovaTerm::Recording` 类型名及三个构建目标保持，未增加转发头、兼容空壳或新旧双实现。

## 移动与删除

移动 30 个 C++ 文件（生产 24、测试 6），包括私有 `detail/HistoryIndexSql.h`。所有消费者 include 已迁移；录制、搜索各有自己的 CMakeLists，根工程 add_subdirectory，并从主程序递归 glob 中排除，避免重复编译。

删除旧 `src/history/Recording.cmake`、`HistoryP3.cmake`；没有删除功能或测试断言。`tests/history/` 只剩纯后端测试/基准注册；录制四项位于 `tests/recording/`，授权/查询两项位于 `tests/historysearch/`。既有目标名、标签、预算与断言保持，QTest 文件输出目录随模块更新。

`tests/benchmarks/history/lz4/` 仍是内存历史基准；原 `tests/benchmarks/history/mcp/RecordingBenchmark.cpp` 继续链接同一个录制库，include 已迁移。历史归档路径与旧验收日志保持原样，不批量改写历史证据。

## 验证证据

原始工作区 patch/status 和逐文件 body hash 在 `build/history-module-layout-2026-10-25/`。移动后对照 30 个文件，去除 include 行后的 C++ 内容全部相同；旧路径均已删除。当前 CTest 名单仍为 47 项，与 B0/B1 最终名单逐项一致。

Release 全部目标构建成功（`build.txt`，125/125 完成）；独立 `tests/history-noqt` 配置、构建、测试成功，CTest 1/1（`noqt-configure.txt`、`noqt-build.txt`、`noqt-test.txt`），未引入 Qt/录制/查询依赖。全套本轮结果、只读构建边界审查结果在下文追加；不引用此前成绩替代本轮通过。

## 后续起点

本次目录整理不改变 MCP 阶段进度；下一功能工作仍为 B2 清空事务与 G05 观察/capture 失效，见 [B0/B1 记录](stages/P8_AI_MCP_Interface/records/B0_B1_Implementation_2026-10-25.md)。构建/测试时使用新目录，历史记录里的旧路径按当时版本理解。


只读审查：单个独立 reviewer 核对本轮目录迁移、CMake 归属/依赖、旧 include 消费者、无 Qt 入口及六项测试注册/日志位置，未发现高置信度可操作问题。审查未评价既有 B0/B1 业务、历史实现、历史归档、跨平台或真实设备；未另跑构建/测试。30 文件迁移 body hash、47 项测试名集合、移动文件 include 解析、当前文档链接和 diff --check 均通过。


## 本轮最终结果

- Windows Release 全部默认目标构建成功，`build/history-module-layout-2026-10-25/build.txt`，退出 0。
- 固定产物全套 **46/47，351.54 秒**，`ctest.txt`；唯一失败目标 `novaterm_conpty_tests`。本轮未改动该目标源码、断言或可执行文件（文件时间仍为 2026-10-21），保持既有平台问题范围，未另行追修或宣称本轮重新确认其全部根因。
- MCP **16/16**、录制 **4/4**、离线历史授权/查询与资源/契约，以及 Core/Session/渲染/传输等其余目标均在本次全套中通过。迁移后 QTest 文件实际生成于 `build/Release/tests/recording/` 和 `build/Release/tests/historysearch/`。
- 独立无 Qt 后端 **1/1**（测试 0.13 秒，CTest 总计 0.15 秒），`noqt-test.txt`。这次重新配置/构建，不沿用 B0/B1 成绩。
- 只读独立审查、30 文件正文一致性、47 项测试名集合、include/生成 moc、当前文档链接和 diff --check 通过。纯目录迁移没有新增镜像行为测试、删除断言或放宽断言。

未提交/推送、未连接真实设备、未执行消毒器或其他平台验收。前一轮 B0/B1 成绩与本轮目录重构成绩分别保留。

提交前 0.2.83 版本递增后 Release 构建通过，相关 CTest 23/23（90.28 秒）；完整命令与范围见 [B0/B1 收口记录](stages/P8_AI_MCP_Interface/records/B0_B1_Implementation_2026-10-25.md)。该专项不替代本页 46/47 的全套结果。
