# 合并后审查五项修复计划

目标：修复 0eec9e9 审查报告五项 P2；保持原九工具、历史工具 Schema、预算与用户选定的本地 P3。不新增功能或修改已知 ConPTY 断言。

按 writing-plans / executing-plans 逐项执行，使用当前干净工作区；本轮用户授权修复，不自动提交/推送。

1. `tests/history/HistorySearchTests.cpp` 新增连续四次完成搜索、相同 cursor 修改 maxMatches/maxResponseBytes 回归；先失败，再在 `HistorySearchService.cpp` 释放无句柄终态并记录缓存页面参数。adapter 缓存只在底层通过后复用，key 同步纳入有效 maxMatches。
2. 同目标新增跨记录 ESC(B、ESC intermediates、DCS/SOS/PM/APC、超限控制字符串测试；先失败，再在 `HistoryProjectionDecoder.*` 完整消费序列；保留 UTF-8 原始映射、CR 与控制序列上限。
3. `tests/mcp/McpTests.cpp` 新增异步导入后同一 turn 撤回、不同客户端不受影响、撤回后显式重分享测试。`HistoryCapabilityAdapter.cpp` 用每客户端共享撤回标记，捕获到 pending import，在任何晚到交付前复核；清理来源时也失效旧标记，不积累无界代际。
4. `TerminalPage.*` 的目录结果携带来源 `SshTransport*`；`MainWindow.cpp` 为每个 SFTP 面板接线；`SftpPanel.*` 只消费本连接的终态。`tests/ui/SftpPanelTests.cpp` 覆盖切标签后的成功/失败、不串线与断开/关闭。
5. 构建主程序及相关三个测试目标；运行 history search、MCP、SFTP/UI 相关专项。保存 RED/GREEN 断言日志；同步 P3/P7 与架构/AGENTS 的当前范围。必要时沙箱外运行私有临时目录测试，不能把环境失败记成产品回归。

验收：每项有对应回归、无生产同步磁盘 I/O/新采样通道/无界状态；未覆盖的真实 SSH/UART、其他平台和消毒器单列。最终独立审查后修正确认的问题；只运行改动相关检查，不重跑无关全套。

## 执行结果

五项实施及相关回归完成；搜索新增断言先 9 failed 后完整 32/32；导入撤回先断言失败后定向闭环 4/4；SFTP 来源槽契约先失败后完整 19/19。Release 默认目标构建/重链接成功，相关 CTest 21/21（91.98 秒）。独立只读审查未发现阻断项。没有自动提交或推送，未开展真实 SSH/UART、其他平台、消毒器和无关全套；记录见 P3_Review_Fixes_2026-10-23.md。
