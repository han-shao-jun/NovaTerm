# Gitee 最新界面修复合并（2026-10-26）

用户要求创建 Git 提交并同步最新代码。先将本地 B2 与审查修复提交为 `6f23cdc`（0.2.84）；提交前正常 Windows Release 完整构建成功，相关 MCP17项与Session单次 CTest **18/18，108.46秒**，原始日志位于 `build/b2-commit-sync-2026-10-26/0.2.84-build.txt`、`0.2.84-ctest.txt`。

## 合并内容与冲突

fetch 时 GitHub/master 为 `1792226`，Gitee/master 为 `e1fb2cc`。本次普通合并 Gitee 提交 `fix: 修复会话窗口按钮遮挡及 SFTP 重复覆盖确认`，保留其 SessionPage 固定按钮栏、四协议独立滚动，以及 SFTP 保存对话框禁用系统覆盖确认、由下载队列统一确认的改动；保留本地全部 B0/B1/B2 实现、十三工具与相关测试。

仅根 CMakeLists.txt 版本行冲突：本地0.2.84、远端0.2.83。按 AGENTS.md 每次提交递增 PATCH 的约定，将合并版本设为 **0.2.85**。docs/ARCHITECTURE.md 两侧修改自动合并，保留 B2 架构说明及远端界面说明；其他远端源文件无冲突。不 rebase、不 force-push、不丢弃两侧提交历史。

## 验证范围

按项目规则，合并他人分支后运行完整 Windows Release 默认目标构建与全套 CTest；本轮实际成绩如下。构建使用实际存在的 `C:/Programs/MicrosoftVisualStudio/18/Insiders/VC/Auxiliary/Build/vcvarsall.bat x64`；测试使用同前缀 Qt bin/plugins，不全局设置 offscreen，不连接真实设备。

原始日志统一位于 `build/b2-commit-sync-2026-10-26/`。既有 ConPTY 失败与本轮回归分开，不放宽断言；真实设备、其他平台、消毒器、性能及 UI 人工操作未运行时不记为通过。

### 0.2.85 固定版本结果

- 完整 Release 默认目标构建退出0，日志 `0.2.85-build.txt`。
- 全套 CTest **47/48，343.99秒**，日志 `0.2.85-ctest.txt`，退出8；MCP标签 **17/17（91.68秒）**，UI/SFTP/Session及其余目标均在此次全套通过。仅#23 `novaterm_conpty_tests`失败，不写成全套通过。
- ConPTY文件复核 **10 passed / 2 failed，49.028秒**，`0.2.85-conpty-detail.txt`。仍为 `injectedStartupStagesRollBack` 句柄上限与 `repeatedLifecycleReturnsResourcesToBaseline` 严格相等；后者句柄221/224、线程6/6、子进程0/0（final/baseline），线程/子进程检查通过。句柄减少也触发严格相等，不据此推断泄漏增长率，未修改生产代码或放宽断言。
- 比对Gitee目标版本，三个UI源码文件无差异；比对6f23cdc，本地Core/MCP/Session与tests无差异。合并冲突列表已清空，`git diff --check`通过。

产品代码与测试在0.2.85全套开始前已固定；之后仅补文档与提交说明。没有运行真实SSH/UART、其他平台、消毒器、性能或UI人工操作。保留0.2.84提交前18/18与本轮合并全套各自成绩，未拼接。同步目标为现有GitHub/master与Gitee/master，不force-push、不重写已有提交历史。
