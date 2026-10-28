# 外部 Agent 的 SSH/UART MCP 终端工具

更新：2026-10-28。主线是本地外部Agent编码、编译和部署，通过NovaTerm MCP操作SSH/UART、观察终端反馈，再继续修复。NovaTerm提供明确授权的工具，不内置Agent、不托管模型、不编排开发流程。

当前二十六工具覆盖终端观察/输入、历史与日志、清空/等待/任务、保存目标连接/预授权、独立文件目录、SFTP上传/下载与UART发送。Windows基础软件已有分阶段证据，SFTP上传/下载显式覆盖本阶段Windows软件回归已完成，B4仍待用户验收，见[覆盖记录](records/B3b_File_Overwrite_2026-10-28.md)。本轮按Windows交付，Linux/macOS安全文件后端仍未实现；B4由用户验收，P8未整体完成。

UART发送复用原Controller、worker、唯一输入泵和协议owner，需要独立本地读取与UART发送授权。调用方独立准备接收端并显式release已有MCP输入lease；工具不自动rz、不抢占人工传输。protocolCompleted与fileConfirmed分别表示协议结束与单文件确认，ZSKIP不是送达；remotePublication始终unknown、contentVerified始终false。按真实大小/波特率计算总期限，超预算在首个协议字节前拒绝。

SFTP上传需要local.read+remote.write，下载需要local.write+remote.read；overwrite可选、默认false，true只授权可信目录中的目标路径替换，不承诺CAS，本地pinPath也未检查目录ACL/owner。下载的发布与临时文件事实仍指本地，contentVerified=false。托管提权保持unavailable且不作为闭环前置。软件范围见[收口账本](08_Software_Closeout.md)，历史证据见[验收档案](records/README.md)。

## 阅读顺序

| 文档 | 职责 |
| --- | --- |
| [总体架构](00_Overall_Architecture.md) | 目标、分工、唯一数据通路、所有权和兼容边界 |
| [现行契约](02_Current_MCP_Contract.md) | 当前二十六工具、独立权限、精确字段与有限预算 |
| [闭环缺口与证据](03_Gaps_and_Evidence.md) | 当前软件缺口与待验场景；历史静态盘点按日期追溯 |
| [游标与清空](04_Cursor_Observation_and_Clear.md) | 日志位置/分页/失效与清空事务语义 |
| [Agent使用流程](05_Agent_Workflow.md) | 从目标确认到部署、观察、重试和结果判断 |
| [等待、任务、传输与连接](06_Terminal_Workflow_Extensions.md) | 各工具分工，SFTP/UART交付与独立接收准备边界 |
| [实施顺序与验收](01_Phases_and_Acceptance.md) | B0～B4工作项、测试选择和退出条件 |
| [实机测试清单](07_Real_Device_Test_Checklist.md) | 当前SSH/UART交互、SFTP上传/下载、UART发送、连接权限及性能步骤；均未执行 |
| [软件收口账本](08_Software_Closeout.md) | 当前阶段、剩余软件任务与B4用户验收分工 |
| [验收档案](records/README.md) | 日期、平台、命令、失败和限制；不作为新方案 |

## 范围与边界

提供显式目标、屏幕观察、终端缓存增量读取、文本/按键输入、有界文本条件等待、长任务事实、可选清空、保存目标连接和独立授权的文件交付。SFTP与UART各自复用已有后端；查询不执行、上传/下载不运行文件、取消不自动证明远端回滚。

不建设聊天侧栏、模型宿主/压缩/规划循环、自动诊断平台、自动烧录平台或任意设备扫描。已有录制、历史搜索和证据能力独立保留，录制不自动授权，实时缓存观察不依赖录制。

sinceToken/nextToken的观察分支与日志位置游标各自使用；recentOutput是有过滤与去重的摘要。UART静默、命令完成、文件确认和板上测试成功分别判断。保存目标创建可见终端，默认不共享；只有本机显式预授权规则可对原新建连接一次绑定读取/输入/清空，文件权限始终独立。

兼容基线为九个终端工具与三个离线历史工具，各次扩展在02与fixture中冻结；旧mcp/P0～P8平台方案及未发布P4/无消费者原型已经退役，历史名称只作追溯。全项目约束以[统一架构](../../../ARCHITECTURE.md)为准，精确行为以02和源码为准；文档更新不产生测试通过结论。
