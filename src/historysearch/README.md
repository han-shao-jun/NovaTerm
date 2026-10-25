# 离线历史授权、读取与搜索

构建目标 `novaterm_history_search`，负责独立资源授权、分页读取、字面搜索及 UTF-8/终端控制序列投影。只链接 Qt Core，使用 `HistoryReadPort` 注入数据源。

共享的记录值类型来自 `recording/HistoryTypes.h`；本库不链接具体 HistoryService、磁盘录制、SQLite、当前终端缓存、MCP 或 UI。已录制数据和显式导入资源均可由组合根提供端口。

MCP 与资源共享应用服务可以引用本库；协议解析和可信身份留在接入层。既有 `NovaTerm::Recording` 类型命名保留，本次只调整目录与构建归属，不改变接口/持久格式。

行为测试位于 `tests/historysearch/`，原 history-search 标签与目标名保持；QTest 输出在 `build/Release/tests/historysearch/`。
