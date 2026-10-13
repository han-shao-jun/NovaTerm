# HTTP MCP 审查修复计划

用户授权顺序：全部 Deep findings → I-001 → 严重 lint 项，不自动提交。

- [x] D-001：连接使用完整远端端点身份；覆盖相同源端口不同回环地址、
  旧连接断开和连接上限。
- [x] D-002：在分配 context 前验证 JSON-RPC 与 initialize；无效通知不能
  返回接受；初次响应无法交付时回收尚不可引用的会话。
- [x] D-003：HTTP 与 wire 的版本错误使用统一 supported/requested 字段。
- [x] D-004：原始头验证后及时处理 Expect；兼容 Qt parser，避免重复 100。
- [x] D-005：线程退出后当前 socket 统计归零，历史峰值按定义保留。
- [x] I-001：最终 RPC 在 wire 一次编码成不可变字节，Endpoint 直接路由；
  删除 Service DTO 的重复编码；SSE 校验/组帧在 I/O worker，保持队列额度。
  先记录大响应编码基准，再对比；异常超限 fallback 必须继续有界。
- [x] Lint：补测试网络截止时间、reply 错误/释放契约；复核测试 QFile
  检查。回环 HTTP 不改 HTTPS，风格提示不机械改显式 QObject 构造。
- [x] MCP 四项目标、官方 SDK、退出矩阵及主程序构建；再做独立复核。

Ruling：延续当前工作树，保护未提交的 HTTP 迁移；修复不回退 stdio。
Ruling：网络 I/O 修复可独立委派，Endpoint/Wire 编码由控制器接线；避免
同文件并行编辑，全部 Deep 修复完成后才改 I-001。

## 最终验收（2026-10-14）

- D-001～D-005 分别记录 RED/GREEN；追加初始化断流、非法通知、2026 请求
  capabilities 和解析后拒绝响应排空回归，独立复核未发现高置信残留。
- 主程序构建成功；MCP CTest 四项 4/4（45.91 秒），SDK 2025 HTTP 与原始
  2026 discover/catalog/read、严格版本错误字段、三个退出场景全部通过。
- 256 KiB 结果 wire 基准：固定 50 次/median 3，99 → 64 ms，即约
  1.98 → 1.28 ms/次（减少约 35%）。不代表整机 GUI frame P95。
- 最终 RPC 正常路径只编码一次，Service 内部 DTO 不编码，Endpoint 直接
  传递不可变字节与错误码，SSE 校验/组帧在 I/O 线程；超限 fallback 有界。
- 严重 lint：补网络截止时间、区分传输/RPC 错误、reply 收尾释放。
  QFile 的 QVERIFY 检查有效；回环 HTTP 的 SSL/HTTPS 与初始化风格提示
  不机械改动。测试继续使用隔离回环端口及模拟 SSH，不连接用户设备。
- Ruling：2026 NotificationMeta 不等同 RequestMeta，HTTP 核心不定义客户端
  通知；明确拒绝通知并不分配 context，取消仍使用请求流关闭。依据：
  https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http
- 日志：build/mcp-review-verified-{build,ctest,interop,shutdown}.txt，
  build/mcp-review-codec-{before,after}.txt，build/Release/http-review-*.txt。
- 本轮仅跑相关 MCP 目标，未重跑其他模块全套、跨平台与整机 GPU 压力。
  未自动提交，原只读审查报告保留历史，以上为修复收口事实。

## 提交收尾（2026-10-14）

用户明确要求测试后提交并推送；本批版本递增为 0.2.68。提交前重新构建主程序与四个 MCP 测试目标，运行相关 CTest、SDK 互操作及退出矩阵；沿用 master 的 github 上游，正常快进推送，不强推。

提交前 0.2.68 复验：主程序与四个 MCP 目标重新构建；相关 CTest 4/4（45.30 秒），SDK 互操作与退出三矩阵通过。日志为 build/mcp-0.2.68-commit-{build,ctest,interop,shutdown}.txt。