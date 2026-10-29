# MCP Shared Access Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Track completed steps here.

**Goal:** 所有 AI 使用统一令牌及授权，取消用户可见客户端管理。
**Architecture:** AccessStore 存储唯一接入记录，复用内部身份代际及领域授权服务；UI 和协议入口使用该统一主体。
**Tech Stack:** Qt 6.8 / C++17 / Qt Widgets / Ela / Windows MSVC。
**Spec:** ../specs/2026-10-29-mcp-shared-access-design.md

## Global Constraints

- 用户后续范围覆盖当前布局增量：删除保存目标整表及4工具，MCP只暴露已连接会话，默认所有会话权限打开。不提交/推送、不连接设备、不改版本。
- 构建使用 scripts/build-novaterm_com.bat；构建由主代理串行调度。
- 旧客户端授权不自动合并；不在状态刷新时触碰系统凭据库。

## Review Focus

- 旧格式启用状态不能继续认证旧令牌。
- 凭据或 JSON 保存失败必须回滚；读取未初始化状态不弹密钥库。
- 外部实例令牌重置/停用必须及时使原请求和资源失效。
- 文件/历史窗口打开期间重置令牌不能向旧主体提交授权。
- 多连接共享权限仍保留请求重试、会话代际和人工接管检查。

## Task 1: 单接入存储

Files: src/mcp/McpAccess.{h,cpp}, tests/mcp/McpAccessInteractionTests.cpp。
Interfaces: accessId() const -> QString（只读）；ensureAccess() -> QString（显式本机创建）；rotateToken() -> bool；exportToken() const -> optional<QByteArray>。删除客户端列表/增删入口；保留领域操作的内部主体参数。
- [x] 补自动创建、旧格式拒绝/迁移、共享身份、失败回滚和跨实例失效测试并观察 RED。
- [x] 实现 version=2 单 access 记录和以上接口；setEnabled(true) 确保令牌已建立。
- [x] 同步该目标旧多客户端测试为统一主体/伪造主体拒绝测试，复验 GREEN。

## Task 2: 统一接入 UI 与服务配置

Files: McpSettingsDialog、SettingsController、FileResourceSharingDialog、McpService 配置接口、tests/mcp/McpTests.cpp。
- [x] 补无客户端控件/自动初始化及两个 HTTP 连接共享授权测试并观察 RED。
- [x] 删除管理窗口和选择器，复制统一配置，保留重置令牌与状态失败反馈。
- [x] 文件窗口使用统一主体并保留打开时身份代际绑定；更新旧测试。
- [x] 删除保存目标表/独占UI接线及list/open/query/close四工具，目录22项；旧fixture仅过滤退役项，保留工具按名核对。
- [x] Service 对新就绪连接初始化四项默认权限；只列出真实连接，普通刷新保留人工撤销。

## Task 3: 历史入口和其余测试迁移

Files: TerminalPage、HistoryRecordingPanel 相关文件及测试、其余 MCP 夹具。
- [x] 历史共享/导入移除客户端选择，以统一接入主体传给资源服务；关闭/重置后的旧窗口拒绝。
- [x] 其余测试改用统一接入 API，原跨客户端隔离场景改验证伪造主体/旧身份拒绝或多连接共享；不放宽文件/会话安全断言。

## Task 4: 收口

- [x] 中英文消息、使用说明与当前契约同步，历史记录保持原样。
- [x] 指定脚本构建、相关 CTest、完整 MCP 业务及 22 工具互操作（依赖可用时）。
- [x] 独立行为审查，修复确认问题并定向复验，记录真实轮次及未验证范围。

实际轮次、失败与限定范围见 ../../architecture/MCP_Shared_Access_2026-10-29.md；最终布局构建、专项11/11与深浅截图已完成。

## 后续审查修复及交付授权

用户要求修复“其他会话默认授权误使原任务失效”和“未知凭据存储方式误报仅本次运行”两项问题，然后创建Git提交并推送Gitee。本轮以0.2.103收口；前述不提交/不推送为原实施批次限制，已由本次明确指令覆盖。跨会话12行及存储三态回归先观察RED再修复，最终准确成绩见架构记录。
