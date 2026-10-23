# Gitee 合并与审查（2026-10-23，0.2.81）

## 合并选择

起点为本地 `137c5f939861b0b77f4341260b8b3d4596944387`，抓取到的 Gitee
master 为 `c8f4cf0`，共同祖先为 `dffe6e4`。

本地 `137c5f9`（han-shao-jun，2026-10-23 00:57 +08:00）与远端
`c8f4cf0`（liurui，2026-10-23 21:16 +08:00）独立新增了不同接口的 P3，
直接合并产生 38 个冲突文件。用户明确选择“保留本地 P3，合入远端其余更新”。

本轮合入 `80712c8`（基准目录整理）、`6cc238d`（PowerShell 测试脚本迁移
Python）、`0e347e8`（SSH 辅助面板和资源采样）。本地历史授权、读取、搜索、
证据、MCP 和录制共享 UI 的 P3 源码保持 `137c5f9`；不混用两套接口。
远端 P3 已审查但未采用，Git 合并决议应记录这一选择，避免后续重复整合。

## Qt Code Review Report

**Scope**: `git diff 137c5f9...c8f4cf0`；检查新增行及调用上下文。
**Files reviewed**: 59 个新增/修改 C++ 头与源文件，另检查 Python runner 与构建入口。
**Issues found**: 2 项主要确认缺陷（深度分析）；另记录投影共同限制和实测失败。

### Lint findings

统一 Qt linter 已运行，产生 366 条原始诊断（含文件未修改的上下文）。
完整输出保留于 `build/review-gitee-c8f4cf0-lint.txt`，不是 366 项功能缺陷。
主体为初始化风格、optional 使用、头文件顺序等；历史规定保留的初始化风格及
chrono `count()` 不据此改写。本轮未自动修改这些诊断。

### Deep analysis findings

#### [D-001] [P2] 路径查询期间切换标签，原 SFTP 面板可永久忙碌

- **File**: `src/ui/app/MainWindow.cpp:1321`（关联 1323–1328）。
- **Category**: Ownership & Lifecycle。
- **Confidence**: 96/100。
- **Finding**: SSH A 点击“从终端同步 SFTP 路径”后，在应答/超时前切到 B。
  A 的完成和失败被 `TerminalPage` 的“仅当前终端”过滤丢弃；新面板栈保留 A，
  切回同一 transport 又提前返回，因此 A 的 `_busy` 无法清除，浏览/上传/路径动作持续禁用。
- **Trace**: `SftpPanel.cpp` 点击处理先 `setBusy(true)`；
  `TerminalPage::registerTerminalView()` 只为当前终端转发目录结果；
  `MainWindow` 仍通过 `currentPanel()` 消费完成；
  `SftpPanel::setSessionContext()` 同一 transport 分支保留原状态。
  新测试只覆盖连接和目录保持，未覆盖在途目录查询的标签切换。
- **Mitigation**: 查询、完成和超时绑定到发起请求的 transport/面板；为非当前标签也交付终态。
- **Disposition**: 本轮已合入的代码中仍存在；用户请求审查，未顺带修复。

#### [D-002] [P2] 并发续页可绕过历史读取游标数量上限

- **File**: 远端 `src/history/HistoryReadService.cpp:40`，关联 70–74 行。
- **Category**: Model Contracts。
- **Confidence**: 100/100（夹具复现）。
- **Finding**: 容量检查只作用于初始请求，同一 cursor 的 8 个并发续页各复制位置，
  各插入不同后继，但只有第一次删除旧 cursor 有效。每轮净增加 7 个。
  19 轮从 1 个增长到 134 个，超过 `MaxCursors=128`，新的读取返回 `BUSY`。
- **Trace**: `HistoryService::readCommitted()` 异步回调允许请求在首个完成前共存；
  adapter 的 8 项入口上限不足以避免重复消费。独立 Qt Core 夹具执行远端原始源码，
  输出 `Live successor cursors: 134; documented maximum: 128; fresh read: BUSY`。
- **Mitigation**: 每个游标串行消费并预留容量，或使用固定 stream 的有界位置更新。
- **Disposition**: 未采用远端 P3；本地实现具有 `stream->busy` 检查和固定 stream 数量上限。

### Investigation targets (human verification needed)

没有额外列入的 60–79 置信疑点。以下作为范围/现有限制记录，不混记为新增合并缺陷：

- 远端 decoder 对 `a ESC ( B b` 投影为 `aBb`，应为 `ab`；独立夹具已复现。
  本地 decoder 也有相同的 ESC intermediate/control string 支持限制，不能称为本次拉取回归。
- 新资源采样在同一帧同步执行 `df`，慢挂载会拖延基础指标直到响应超时。
  远端架构文档已明确承认这一取舍；本轮未做真实 SSH/慢挂载验收，不另报重复设计风险。

### Summary

| Category | 主要确认缺陷 | 本轮合入 | 仅远端未采用 P3 |
| --- | ---: | ---: | ---: |
| Ownership & Lifecycle | 1 | 1 | 0 |
| Model Contracts | 1 | 0 | 1 |
| Thread Safety / API / Error Handling / Performance 其他项 | 0 | 0 | 0 |

六个审查使命均完成；没有启动真实 SSH 或串口连接，没有修改被审查的生产行为。

## 验证

- 合并后的 Windows Release 全部默认目标构建成功；沙箱内主程序 PDB 的 C1902
  经沙箱外重建消失，不能归为代码编译错误。
- Python serial-stress 单测 **22/22**、history/lz4 单测 **17/17**。
- 远端 P3 独立 Qt Core/Test 夹具 **4/5**：Authorization、Read、Search、Evidence 通过；
  SearchService 目标 **9 passed / 1 failed**，`eofSpanBudgetKeepsDeferredHit`
  第一页命中数为 0、预期 1。该测试在跨大量异步 batch 时使用默认 2 秒页面预算。
  独立复制该测试，仅将页面预算放宽为 15 秒后，同一断言通过（单例耗时 2.189 秒），
  确认为测试假定“第一页扫描到 EOF”与默认页面时间预算冲突。未修改远端原始测试或
  产品限制，原始五目标成绩仍是 4/5，不能替换成远端文档的 5/5。
  详情见 `build/review-gitee-harness/search-service.txt`、`search-relaxed.txt`。
- 游标/投影最小夹具与输出：`build/review-gitee-harness/CursorRepro.cpp`、
  `build/review-gitee-harness/cursor-repro.txt`。夹具不注册产品 CTest。
- 合并后 `ctest -N` 实核 **48** 项。沙箱外 Release 全套 **47/48**（325.73 秒）；
  唯一失败为 `novaterm_conpty_tests`。原始 CTest 日志：
  `build/review-gitee-merge-ctest.txt`、`build/Release/Testing/Temporary/LastTest.log`。
- ConPTY 完整文件输出复跑 **10 passed / 2 failed**（49.685 秒），仍为
  `injectedStartupStagesRollBack` 和 `repeatedLifecycleReturnsResourcesToBaseline`
  的句柄断言。重复生命周期线程/子进程检查通过（threads 4/6、children 0/0），
  该轮最终/基线句柄为 219/222；不把这一轮计数下降宣称为复现旧调查的增长率，
  不放宽断言。完整输出在 `build/review-gitee-conpty.txt`。
- 新 SFTP 面板目标 **17/17**，录制存储 **33/33**、录制 UI **9/9**。
  全套覆盖不代表 D-001 的未覆盖交错已经正确。

构建/回归命令（PowerShell，Qt bin 和 plugins 已设置）：

```powershell
cmd /c scripts\build-novaterm.bat
ctest --test-dir build/Release -C Release --output-on-failure
python -m unittest discover -s tests/benchmarks/serial-stress -p 'test_*.py'
python -m unittest discover -s tests/benchmarks/history/lz4 -p 'test_*.py'
```

其他平台、真实 SSH/UART、GPU 人工验收、性能矩阵及消毒器未执行。
