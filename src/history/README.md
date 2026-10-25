# 内存终端历史与 LZ4

本目录只负责当前终端 Scrollback 的块编码、可选 LZ4 压缩、后台预算与冷块读取，构建目标为 `novaterm_history`。压缩默认关闭；关闭压缩仍使用同一个历史后端。

`ChunkedScrollback`、快照、块和 B1 日志分页代码位于 `src/core/scrollback/`，与本目录共同编入该纯 C++ 库。独立入口为 `tests/history-noqt`，不查找或链接 Qt、MCP、录制、搜索应用或 UI。

- 持久接收流录制：[`../recording/`](../recording/README.md)。
- 离线历史授权、读取和搜索：[`../historysearch/`](../historysearch/README.md)。
- MCP 通过 Core/Session 窄端口读取当前缓存；读取不会自动开启压缩或录制。
