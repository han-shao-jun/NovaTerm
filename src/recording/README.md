# 持久接收流录制

构建目标 `novaterm_history_recording`，负责 HistoryRecorder、块文件存储、SQLite 索引、保留/配额及异步 HistoryService。依赖 Qt Core、私有 Qt Sql 和线程库，不依赖 MCP 或 LZ4 Scrollback。

`HistoryTypes.h` 是记录、范围、完整性和捕获描述的值类型契约；离线读取可以引用这些类型而不链接具体录制后端。既有 `NovaTerm::Recording` 类型名、磁盘格式、配置键和构建目标保持。

本目录不负责授权、搜索、终端屏幕或 Agent 编排。录制源与离线 HistoryReadPort 的具体接线位于 [`../resources/RecordingHistoryReadAdapter.h`](../resources/RecordingHistoryReadAdapter.h)，由应用/MCP 组合根使用。

行为测试位于 `tests/recording/`，CTest 标签为 recording；QTest 文件输出在 `build/Release/tests/recording/`。
