# P3 与 SSH 面板合并后审查修复（2026-10-23）

基于合并提交 `0eec9e9`，修复同日审查的五项 P2。保留本地 P3、原九工具及历史工具 Schema，不修改任何预算或已知 ConPTY 断言。本轮改动留在工作区，未自动提交/推送。

## 修复与回归

| 问题 | 最终行为 | 回归 |
| --- | --- | --- |
| 初始搜索完成后泄漏客户端名额 | 成功终结且输入 cursor 为空时释放 state；有非空旧 cursor 的终结页继续保留有界重试缓存 | 同客户端连续四次单页成功搜索 |
| 搜索重试忽略结果上限 | 缓存保存有效 maxMatches/maxResponseBytes；不相同返回 CURSOR_MISMATCH；合法推进下一页允许改变预算 | 原文服务式精确重试、两类预算修改、下一页缩小命中数；MCP 同 cursor 改 maxMatches |
| 导入完成回调越过撤回 | 按客户端共享 pending ImportBatch；撤回标记并移除当前 batch，旧回调拒绝发布并回收底层资源；新显式分享使用新 batch | 同一主线程 turn 导入→撤回、不同客户端不受影响、撤回后显式重新分享 |
| SFTP 切标签丢失查询终态 | TerminalPage 在发起时记住 QPointer<来源 SSH>，成功/超时携带来源；每个面板只消费所属连接的结果 | A/B 同时查询，切换后 A 的成功/失败仅结束 A；空来源不解锁；B 随后独立结束 |
| ESC(B / 控制字符串污染文本投影 | 消费 ESC intermediate/final 及 DCS/SOS/PM/APC 字符串至 ST，保留跨记录状态、原始偏移、CAN/SUB 取消与 4 KiB 序列限制 | 六类按字节跨记录拆分、匹配范围和隐藏载荷、长控制字符串显式 PROJECTION_LIMIT |

ImportBatch 只存在于在途任务期间；完成移除时复核 batch 身份，旧回调不删除撤回后新建的 batch。它不改变底层两 worker、16 MiB 文件、资源数及记录预算，也不将读取中断声明为硬限时取消。读取仍在后台，授权复核与发布在 owner 线程。

SFTP 不增加查询通道或轮询，不因切换标签取消正常文件传输。请求映射在成功、失败、视图销毁时移除，来源对象销毁由 QPointer 失效；面板断开仍执行原有 busy 清理。

## RED / GREEN 证据

- 搜索新用例先 **9 failed**：BUSY、重试参数、六类控制序列和控制字符串上限；对应修复后完整目标 **32/32**。
- 导入撤回用例先在 `!revoked->error.isEmpty()` 断言失败；修复后导入与 MCP 历史闭环两个函数 **4/4**（含 init/cleanup）。
- SFTP 两行回归先因缺少来源参数的槽契约失败；新接线后完整目标 **19/19**。这两行使用替身信号源驱动真实 SftpPanel/SftpSession 状态，不是实际 TerminalPage/SSH 端到端验收。
- RED/GREEN 原始日志位于 `build/fix-review-{history,import,sftp,mcp}-*.txt`；构建日志 `build/fix-review-green-build.txt`、`build/fix-review-final-build.txt`。
- 独立只读审查未发现需阻止合入的新增问题；指出真实 TerminalPage→TerminalView 请求映射和真实 SSH 尚无自动化端到端覆盖，本轮保持该缺口。

## 最终验证范围

Release 默认目标完整构建/重链接成功。沙箱外相关 CTest **21/21**（91.98 秒），
包含 MCP 标签 17 项、历史授权/搜索、证据与 SFTP 面板；不借用同日早前 47/48 全套成绩。
原始输出 `build/fix-review-related-ctest.txt`。历史读取/搜索 QTest **32/32**，SFTP 面板 **19/19**。

```powershell
cmake --build build/Release
ctest --test-dir build/Release -C Release -L 'mcp|history-search|evidence|sftp' --output-on-failure
```

Windows 的 Qt bin/plugins 按 AGENTS.md 设置；有私有临时目录/回环要求的测试在沙箱外执行。未运行无关全套、ConPTY、真实 SSH/UART、其他平台或消毒器，不将单测覆盖扩展为这些验收。
