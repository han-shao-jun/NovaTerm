# MCP 接入与旧命令通路清理 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** 删除四个旧命令工具及其界面，把客户端管理移入次级入口。

**Architecture:** 从工具目录、解码、能力注册、运行期组合向内删除旧通路；保留现行终端控制器和独立权限。Qt Widgets对话框继续消费原SettingsController，客户端管理复用原身份生命周期。

**Tech Stack:** C++17、Qt6.8.3、ElaWidgetTools、CMake/Ninja、Qt Test、MCP Streamable HTTP。

**Spec:** `docs/superpowers/specs/2026-10-29-mcp-cleanup-design.md`

## Global Constraints

- 保留新终端交互、文件传输、会话管理和历史录制。
- 四个退役工具不重定向到新输入，保留工具参数及独立授权不变。
- 保留未提交的CPack修复，不连接设备、不自动提交或推送。
- 当前0.2.100版本保持；不引入新执行器，不做机械格式化。

## Review Focus

- 直接调用退役工具必须失败，而非仅从tools/list隐藏。
- 无客户端时能创建、删除后不会向其他身份转移授权。
- 当前会话授权仍绑定原客户端、连接和行版本，过期行不得写入。
- 终端输入/长任务/关闭不依赖被删除的旧执行记录或隔离锁。
- 文件/历史授权保持独立，当前CPack修复仍可构建和打包。

### Task 1: 退役协议与运行期旧通路

**Files:** `src/mcp/protocol/ToolCodec.cpp`、`src/capabilities/CapabilityRegistry.cpp`、`src/mcp/McpService.cpp`及旧通路独占源文件；`tests/mcp/ContractsTests.cpp`、`tests/mcp/McpTests.cpp`、相关CMake目标。

- [x] 添加旧四工具目录缺失、解码/HTTP调用拒绝且不会触发执行的回归，构建运行观察RED。
- [x] 移除工具Schema/解码/能力注册和服务接线，删除无现行调用的独占实现；共用Session设施只在确认无消费者后删除。
- [x] 更新现行工具计数、相关契约与退役功能测试；保留当前功能回归的原断言。
- [x] 运行协议、能力和HTTP/业务相关测试，记录GREEN及外部失败。

### Task 2: 精简接入界面与客户端管理

**Files:** `src/ui/widgets/McpSettingsDialog.{h,cpp}`、`src/ui/models/{SessionPermissionModel,SettingsController}.{h,cpp}`、`tests/mcp/McpTests.cpp`。

- [x] 添加次级管理入口、初次创建和删除客户端、现行读取/输入/清空授权回归，观察RED。
- [x] 删除旧授权列、诊断子行、旧结果与解除保护控件及关联刷新逻辑；精简提示文案。
- [x] 将添加/移除/轮换令牌移入次级对话框，保留主界面客户端选择与两种配置复制。
- [x] 运行相关UI用例，验证身份/连接过期拒绝、撤权级联和次级窗口生命周期。

### Task 3: 文档、互操作与最终复核

**Files:** 当前README/架构说明、`tests/mcp/interop_check.py`及其Schema校验辅助、`AGENTS.md`和本计划。

- [x] 同步当前26工具与旧接口退役说明、AI instructions；历史记录不改写为现行状态。
- [x] 完成Release all构建、相关CTest与完整MCP业务；运行两代互操作或明确记录缺少依赖。
- [x] 运行默认CPack并检查解包启动；对界面做实际截图/行为复核。
- [x] 行为审查、安全边界检查、`git diff --check`；记录验证范围和未验收项。

## Ledger

- 2026-10-29：范围由用户确认，按当前会话内直接实施；前轮CPack改动保留。设计为已批准建议的书面记录，不再次要求确认。

- 收口：目录/解码真实RED四项，实际窗口RED为11列/无管理入口；HTTP拒绝与首次创建/轮换是其后补充验证，不冒充先行RED。最终相关CTest24/24和两代26工具互操作通过，Linux串口文件后端缺口及历史失败独立保留，见 `docs/architecture/MCP_Cleanup_2026-10-29.md`。此为实施阶段记录，后续提交授权见下项。

- 后续提交授权：用户明确要求创建Git提交并推送；按主题先提交CPack（c91a56c，0.2.101），MCP清理递增至0.2.102。先前不自动提交/推送的限制由本次明确授权更新；中间提交不冒充隔离重测，最终版本结果在清理记录另节保留。

- 0.2.102提交前验证：all、MCP/录制相关CTest28/28（37.66秒）、两代26工具互操作、DEB/ZIP和解包运行通过。Linux串口文件后端缺口单独保留；提交按用户授权执行。
