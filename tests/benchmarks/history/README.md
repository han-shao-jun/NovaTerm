# 历史相关基准

| 目录 | 所属模块 | 测试范围 |
| --- | --- | --- |
| [lz4/](lz4/README.md) | 终端回滚历史与 LZ4 压缩策略 | 压缩 OFF/ON、预算、冷读、GUI 滚动、显示保护及矩阵脚本 |
| [mcp/](mcp/README.md) | MCP 历史模块（P2 录制链路） | `HistoryService` / `novaterm_history_recording` 合成输入性能 |

两组使用不同的历史库和验收标准，不能相互替代。
历史结果分别位于 [LZ4 归档](../archive/lz4/) 与 [MCP 历史归档](../archive/mcp-history/)。
