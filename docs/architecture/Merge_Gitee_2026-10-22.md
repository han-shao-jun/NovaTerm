# Gitee master 合并验证（2026-10-22，0.2.80）

合并本地 `e9cff9e`（历史视口 pin 复用）与 Gitee `633a6cd`，保留双方提交历史。
远端包含重连状态复位、串口传输窗口重绑定和 P2 历史录制，未重写远端提交。

唯一文本冲突为 `HistoryGuiScrollBenchmark.cpp` 的 `ProbeRenderer` 构造函数：
双方均在消除 MSVC C4623；保留本地已验证的显式转发构造函数。Core 的
`resetForConnection()` 和 pin 复用、各自测试均保留。合并提交版本为 0.2.80。

## 本机验证

- Windows / MSVC / Qt 6.8.3，Release 全部目标构建通过；实际部署目录含
  `Qt6Sql.dll` 与 `sqldrivers/qsqlite.dll`。
- `ctest --test-dir build/Release -C Release --output-on-failure`，实际注册
  **43** 项，全套 **42/43**，329.33 秒；唯一失败为 `novaterm_conpty_tests`。
  新增 Recorder/Store/Service/录制 UI 四项、Session、Core、渲染、串口传输均通过。
- ConPTY 文件日志复跑 **10 passed / 2 failed**，44.515 秒：
  `injectedStartupStagesRollBack` 的句柄上限和
  `repeatedLifecycleReturnsResourcesToBaseline` 的严格句柄相等断言失败，
  对应 AGENTS 已记录的失败位置；其余用例及重复生命周期线程/子进程检查通过。
  本轮后者句柄为 214/217，前者打印 190/193 后断言仍失败，存在计数波动；
  不将其直接写成历史 Windows 10 的 +200 Process 句柄泄漏已经复现或已修复。
  本轮未修改 ConPTY 生产代码或放宽断言。
- 合并后的真实 OpenGL GUI `--self-test` 通过。此前 0.2.79 的性能矩阵和
  ASan 成绩仍仅代表当时源码，不推断为合并后复测通过。

本机原始日志在 `build/gitee-merge-2026-10-22/`。未补测 Linux/macOS、
真实 UART、用户 SSH 服务器、多会话长稳或合并后完整性能矩阵。

本轮只推送 Gitee master；工作区原有 `.dide/` 不纳入合并提交。
