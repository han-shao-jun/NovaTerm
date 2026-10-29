# 文件资源浏览器与审查修复 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** 修复目录选择生命周期/上下文和字号，完成SSH远端目录浏览，并排查用户补充的UART/ZMODEM问题。

**Architecture:** 远端浏览复用每个SSH Transport已有SftpPanel/SftpSession队列；新增带独立请求ID的只读目录结果，不消费或扰动原面板的directoryListed。GUI目录浏览器只接收异步列举端口，接入适配器在请求/交付前重验身份和完整Session绑定；关闭/撤权/换绑/超时取消交付。路径选择不自动共享。

**Tech Stack:** C++17、Qt6 Widgets/Ela、libssh/SFTP、Qt Test、现有UART/ZMODEM Controller。

**Spec:** 用户于本轮明确要求全部修复且包含目录浏览器，保留已有历史录制关闭修复，不再将浏览缺口交回用户。远端浏览支持home/上级/输入路径/进入目录/明确选择，目录条数和等待有界且截断显式提示，不创建第二套SFTP执行器。UART具体表现已异步询问，继续独立排查已知软件缺口。

## Global Constraints

- 不自动连接用户设备做验收、不自动提交/推送，版本保持0.2.103。
- 本地目录返回使用QPointer并复核身份/目录类型代际；切走再切回也拒绝旧结果。
- SFTP只列目录，不传输/执行/创建文件，不自动授予权限，不改变原SFTP面板目录。
- 串口复用原Controller，输入lease、连接/I/O代际、取消与协议完成事实保持独立。

## Review Focus

- 父窗口在本地/远端选择期间销毁：无悬垂回调。
- 身份、类型、SSH连接或I/O代际变更：旧结果不回填/不授权。
- 同SFTP队列有其他请求：按独立ID交付，不污染原面板。
- 错误、断流、队列满、慢目录/大量条目：有界并可取消/重试。
- ZMODEM的授权、接收准备、lease、文件源与完成/取消语义：依真实复现修复，不掩盖平台缺口。

### Task 1: 审查问题

- [x] 新增本地选择正常/类型切换/切回/令牌重置/无关刷新/取消和父窗口销毁子进程回归。
- [x] 真实RED为6pass/4fail；补QPointer、选择代际和状态12px字号。
- [x] 复验GREEN、原有文件授权与历史录制关闭回归。

### Task 2: 远端浏览

- [x] SftpSession独立只读请求ID、错误、目录/扫描预算及旧generation隔离回归后实现。
- [x] 复用SshPanelStack/SftpPanel所有者，提供只读目录会话；适配器绑定身份和连接、限额、超时和context生命周期。
- [x] 实现独立RemoteDirectoryDialog及Fake端口回归；连接文件授权按钮、类型占位/提示和最终回填复核。
- [x] 检查本地受控SFTP测试能力，尽可能验证真实worker，不用设备验收替代软件证据。

### Task 3: UART与收口

- [x] 症状未补充；独立复现并修复Linux串口发送安全文件源缺失，保持原Controller和协议/取消语义。
- [x] 运行相关协议、UI、SFTP、UART测试及Release all，更新中英文和文档。
- [x] 实际截图复核、六领域行为复核、diff检查，准确记录失败/平台/设备验收缺口。

**完成记录：** [范围、真实RED/GREEN与交付证据](../../architecture/File_Resource_Browser_Fixes_2026-10-30.md)。实施版本0.2.103；用户于2026-10-30追加提交和推送要求，目录/文件源收口递增至0.2.105，历史关闭另以0.2.104提交。设备与其他平台验收未执行。
