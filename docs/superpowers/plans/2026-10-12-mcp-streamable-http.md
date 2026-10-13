# NovaTerm 进程内 Streamable HTTP MCP 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development or superpowers:executing-plans.

**Goal:** 客户端仅通过本机 URL 连接 NovaTerm，删除独立 MCP 子进程与旧私有 IPC。

**Architecture:** 应用拥有 HTTP I/O worker；HTTP、MCP wire 适配和现有
Service 授权／Session 调度分层。HTTP 不持有 Session 指针；只保留进程内
Streamable HTTP，旧 stdio/LocalMcpServer 不保留兼容入口。
HTTP TCP 生命周期与业务请求分别管理。

**Tech Stack:** Qt 6.8、C++17、Qt Network、Qt HTTP Server、QThread。

**Spec:** 用户已选择本对话提出的进程内 HTTP 架构；规范为 MCP
2025-11-25 与 2026-07-28 Streamable HTTP，业务约束见 docs/ARCHITECTURE.md。

## Global Constraints

- 延续现有工作树，不重置无关改动；用户随后明确 HTTP-only，旧 stdio 退出
  修复作为历史记录，桥接源码及构建目标删除。不自动提交或推送。
- 仅监听 127.0.0.1；每个请求核验 Bearer token、Host 与 Origin。
- HTTP worker 内构造和析构 socket；入口及 QThread 名共用 nvterm-mcp-http。
- 单帧 2 MiB，头部 16 KiB，单请求输出 4 MiB；跨线程队列有上限及停止语义。
- 保留七工具、客户端／Session 授权、风险确认、去重和未知命令结果。
- 2025：initialize 与 MCP-Session-Id；POST 通知／反向响应 202，GET 可 405；
  SSE 可携带 elicitation/create，TCP 断开不等于取消，DELETE 结束会话。
- 2026：每请求 metadata 与镜像头核验，不创建协议会话；MRTR 返回 input_required；
  请求流断开取消该请求。不得复用 2025 的断连语义。
- 用户选择补装 Qt HTTP Server，使用匹配的 Qt 6.8.3 / MSVC 2022 64-bit
  模块。原自定义 HTTP 解析器停止实施；模块就绪后采用官方 responder。
- 默认稳定地址 `http://127.0.0.1:38973/mcp`；主应用启动前可用
  `NOVATERM_MCP_HTTP_PORT` 覆盖端口。测试服务使用隔离的临时端口。
- `clientConfiguration()` 返回 `{type:"http",url,headers.Authorization}`；
  Codex 使用 `url`/`http_headers`，旧 `command`/`args`/`env` 配置需要迁移。
- Qt 6.8.3 安装头 `include/QtHttpServer/qhttpserver.h` SPDX 为
  `LicenseRef-Qt-Commercial OR GPL-3.0-only`；开源模块组合程序分发须满足
  GPLv3。项目 GPLv2+ 可选择后续版本，但仍需核对源码/许可交付与其他依赖，
  不作完整法律合规保证。

## Review Focus

- 分片／合并／畸形 HTTP、Content-Length/Transfer-Encoding 冲突、慢读与队列耗尽。
- Origin/Host 的 DNS rebinding 防护、无令牌、跨客户端会话借用和权限撤销。
- 2025 确认响应／取消及 2026 MRTR 重提、metadata 头正文不一致与跨请求 ID。
- 断流不重放命令、应用退出回收 socket、旧实例或旧会话票据失效。
- URL 在生产应用重启后稳定；测试使用隔离的临时端口，多实例冲突明确可见。

## Task 1：HTTP I/O 与协议入口

接口约定（供独立实施与接线）：

- `HttpMcpServer : QObject`：`start(quint16 port=0)`、`stop()`、
  `bool respond(QString requestId, int status, QByteArray body={},
  QJsonObject headers={}, bool sse=false, bool final=true)`。
  信号 `listening(bool ok, QString url)`、`request(QString id,QJsonObject envelope)`、
  `disconnected(QString requestId)`。envelope 含 method/path/headers（小写）、
  message（解析后的 JSON 对象），无效 JSON 用 parseError=-32700/-32600。
  Qt HTTP Server 解析请求，responder 写出 POST JSON 与 chunked SSE。
  HTTP 请求 I/O 生命周期仅由 requestId 标识，不代表 MCP Session。
- `WireSession : QObject`：构造 `(QString wirePrefix={}, QObject* parent=nullptr)`；
  `receiveBytes(QByteArray, QString route={})`、`receiveMessage(QJsonObject,QString route={})`、
  `receiveBackend(QJsonObject)`、`setBackendAvailable(bool)`、`instanceUnavailable()`、
  `cancelAll()`、`initialized() const`、`hasPendingCalls() const`。
  信号 `backendMessage(QJsonObject)`、`packet(QString route,QJsonObject,bool final)`、
  `completed(QString route)`、`attachmentRequested()`。route 为 HTTP exchange id，
  工具调用保存它以关联异步结果。wirePrefix 保证并行现代请求的内部 Job id
  不碰撞；两代协议适配由 WireSession/共享 ElicitationBroker 完成。

文件边界：HTTP 实施仅创建 `src/mcp/HttpMcpServer.h/.cpp` 和
`tests/mcp/HttpMcpServerTests.cpp`；wire 实施创建 `src/mcp/WireSession.h/.cpp`、
共享 ElicitationBroker 及 `tests/mcp/WireSessionTests.cpp`。
控制器负责所有 CMake、Service、Application、测试接线与配置导出。

- [x] 独立 HttpMcpServer，使用官方 Qt HTTP 解析、JSON/SSE 写出与限额。
- [x] 抽取复用的 WireSession，HTTP 直接调用 Service，保留两代协议隔离。
- [x] 移除 tools/novaterm-mcp 与 LocalMcpServer 源码、构建/安装目标及旧依赖宏。
- [x] 各组件先写回归，确认失败后实现；不连接用户设备或服务器。

## Task 2：应用级接线、认证与配置迁移

- [x] Service 统一发送／关闭抽象，HTTP 请求直接进入既有授权与作业调度。
- [x] 2025 会话与 2026 内部授权上下文分别管理；每 HTTP 请求重验 token。
- [x] 主应用稳定本机端口，只导出 URL/认证配置，移除 stdio 配置 API。
- [x] 更新设置窗口接入地址与互操作／关闭夹具。
- [x] 更新 README、统一架构、P8、AGENTS 与本计划的 HTTP-only 事实及 GPLv3 分发要求。

## Task 3：验收与交付

- [x] HTTP 真实 socket、授权／确认／取消／退出矩阵及迁移后的 MCP 回归通过。
- [x] 官方 MCP SDK HTTP 实接与本机 Codex URL 配置解析验证；
  Codex 桌面实接未验证，不修改用户配置或输出令牌。
- [x] 编译主程序，独立审查，记录跨平台／客户端验收实际边界。

## 执行记录

- Ruling: 用户已授权迁移，延续当前工作树；不重复请求批准同一架构。
- Ruling: 主应用使用稳定默认端口 38973（可显式配置），测试 Service 默认为端口 0；
  冲突显示失败，不静默切换端口或改投实例。
- Ruling: 2026 取消语义按新规范另行实现，纠正此前只适用于 2025 的概括。
- Ruling: 用户要求核对并准备安装 Qt HTTP Server，改用官方 HTTP 解析及
  Qt 6.8 chunked responder；安装期间继续独立的 wire 层工作，不重复实现解析器。
- Ruling: agent 线程总数限制阻止第二个实施代理启动，wire 层由控制器直接实施。
- Ruling: 用户随后明确只保留 HTTP；旧 stdio 兼容计划取消，删除桥接与
  LocalMcpServer，不以历史 stdio 退出验收成绩证明新入口通过。
- 当前源码新增 `novaterm_mcp_wire_tests`、`novaterm_mcp_http_tests` 和
  `novaterm_mcp_endpoint_tests`，Windows 默认注册项暂为 26，Linux 按条件预计
  24（未实跑核对）；实际 `ctest -N` 及本轮构建/测试成绩由控制器验收后记录。
- 许可依据：[Qt HTTP Server 官方说明](https://doc.qt.io/qt-6.8/qthttpserver-index.html#licenses)
  与本机 Qt 6.8.3 安装头一致；完整发行合规需另行核对。

### 最终验收（2026-10-13）

- Windows `ctest -N` 注册 26 项，Release 全套 26/26（233.97 秒）。
- 主程序/全部默认目标构建与 Qt HTTP Server/WebSockets 部署完成。
- SDK 2025 HTTP 与原始 2026 discover/catalog/read、关闭三矩阵通过；
  两代确认/MRTR/脚本与断流取消由业务、endpoint 回归覆盖。
- 独立审查的协议代际混用及 socket 拒绝后残留均先复现再修复；新增队列统计。
- Codex 仅验证配置解析，桌面客户端须自行用导出 URL/认证配置替换旧入口；
  跨平台与性能对照未重测。代码未提交。
