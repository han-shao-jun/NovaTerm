# mcp/P3 实施与验证

日期：2026-10-23。执行计划：[P3](P3_History_Search_and_Evidence.md)。基线：0e347e8，工作区初始干净。

## 实施账本

- T1：独立 capture 授权与固定提交快照分页。
- T2：有界后台投影搜索及服务端续页状态。
- T3：来源无关证据与显式导入。
- T4：追加三个 MCP 工具、本机共享入口与兼容验证。
- Pre-flight：T1 的资源身份/范围/授权版本供 T2/T3 消费；T4 只适配可信客户端与已实现用例。
- Ruling: 沿用当前工作区，不创建分支/worktree、不自动提交 — 遵守阶段总计划 §1；变更保留供审阅。
- Ruling: 离线授权绑定客户端令牌摘要而非 Session accessVersion — 后者会因 Session 授权/关闭改变；每次交付仍核对当前客户端和摘要，轮换强制重新共享。
- Ruling: wire 首版收紧到 16 KiB 原文/8 命中/128 原始片段每页 — MCP 同时编码 structuredContent 和文本副本，须给最终 64 KiB 留余量；代价是更多续页。
- Ruling: text 另限 4096 字符列/4096 来源片段 — 在 64 KiB 原始行和 4 KiB 控制序列之外约束 decoder/映射/缓存总量；超限明确停止并给原始起止位置，代价是部分长行须改 raw_bytes。
- Ruling: 首版导入接纳本机明确共享的不可变字节，不提供模型文件路径或导入工具 — 文件选择和诊断发布消费者归 P4/P5 产品接线；不将其写为本轮已交付入口。

## 已落地范围和 owner

| 职责 | owner/依赖 |
| --- | --- |
| capture 范围/时间授权、持久授权序列化 | HistoryAuthorizationStore；仅 Qt Core，不见 Session/HTTP |
| 元数据列举、固定提交前缀及二进制切片 | HistoryReadService，经 HistorySourceReader；磁盘仍由 P2 worker 处理 |
| UTF-8/ANSI/CR 状态、匹配、页缓存/租约 | HistoryProjectionDecoder / HistoryLiteralMatcher / HistorySearchService；CPU 工作池最多 2，回 GUI 只做授权/交付 |
| 来源无关引用、哈希、owner/到期复核 | novaterm_evidence，只链接 Qt Core；不 include History/诊断实现 |
| capture 来源读取与授权、P1 准入、MCP 编码 | HistoryCapabilityAdapter；组合根装配，不向 Dispatcher 添加业务分支 |
| 本机共享意图 | HistoryRecordingPanel → HistorySharingPort；显式客户端/范围/时间/持久化选择 |
| 追加目录/请求类型 | HistoryToolCodec/HistoryToolRequest/BackendHistoryCall；原九工具 variant 和 fixture 保持原样 |

历史工具稳定公开，即使无共享录制也不删除目录项。应用未装配 HistoryService 的测试门面返回 HISTORY_UNAVAILABLE；正式组合根启动后装配。
持久保存只保存用户勾选的独立授权；保存失败回滚内存变化，不报告成功。持久文件使用原有 private JSON/DACL 原子替换，读取前核对私有文件。

## 独立审查及修复证据

一次只读 fresh-context 审查；审查者未修改代码、未运行全套，也未判断实际 GUI 或其他平台效果。

- 跨记录的 spanCount 每次 worker batch 清零：`rawSpanBudgetSurvivesReadBatches` 在放宽**测试**时间预算后真实失败（spans >128）；修为从本页已有命中累计，再计 pendingMatch。
- EOF 有 pendingMatch 时假 complete：`eofSpanBudgetKeepsDeferredHit` 真实失败（complete=true）；修为投影队列、待续页命中和终止错误同时核对，待命中保留续页。
- 自查：无可达 cursor 的初页完成搜索占容量，第三个短查询 BUSY，`shortCompletedSearchesDoNotConsumeCursorSlots` 真实失败后修正；错误/取消状态同样释放。
- 自查：缺口前 flush 先 reset matcher，导致合法跨行匹配丢失，`textMatchCanEndImmediatelyBeforeGap` 先失败后通过；reset 移到有序边界 marker。
- 自查：新 BackendHistoryCall 漏登 HTTP jobs 表，真实 HTTP 用例失败后补登记；2025/2026 双协议现已跑通。
- 自查：相同页重新创建 evidenceId 会改变重试结果；同 owner/来源/范围/hash/投影版本的有效引用复用，双协议端到端用例比较整页对象。
- Final: minor (deferred): 删除客户端/录制后的旧授权未自动清理，长期累计可触及 1024 条上限；现有 UI 不能选择已删除资源撤销。需要后续 owner 事实触发的清理，避免界面猜测资源状态。

## 明确未覆盖/尚未产品接线

- Linux/macOS、真实 UART、长日志性能、崩溃/断电恢复和多进程竞争未在本轮重新验收；P2 的历史成绩不替代这些验证。
- 诊断与导入来源的 immutable memory 发布/解析/授权/删除由独立应用测试覆盖；本轮无文件选择器、导入 MCP 工具、诊断结果发布或跨客户端分享入口，不算 P4/P5 交付。
- 未实现任意正则/编码猜测；text 投影不是完整终端屏幕模拟器，ANSI 首版剥离、CR 按字符覆写，不解释所有光标定位命令。
- 证据句柄为内存，五分钟后/重启后过期；capture 原始数据仍按 P2 配额保留，删除/缺失/损坏或哈希变化不能继续宣称可验证。
- UI offscreen 专项和面板截图只验证本机可用性/布局，不冒充真实显示器、字体、其他平台交互验收。

## 验证

首次沙箱构建在 Ninja 子进程阶段阻塞，沙箱内新测试 DLL 初始化失败（0xc0000142），均未执行断言；后续构建和测试使用沙箱外本机环境。
已记录专项：授权/分页 2/2；搜索投影/状态/证据 3/3；双协议 HTTP 专项 3/3 QTest、契约 17/17。
审查前 MCP 标签 20/21（89.87 秒），唯一失败为新加入的搜索回归测试；保留该红色记录，不能当最终结果。
最终完整重建成功。当前 `ctest -N` 实核 49 项、MCP 标签 21 项（旧文档 43 项为历史）。Windows Release 全套 **48/49，416.03 秒**；MCP 子集 **21/21**（累计 89.50 秒），录制四项 **4/4**；新增五目标 **5/5**，共享 UI QTest **9/9**。

唯一失败 `novaterm_conpty_tests`。只复跑 `injectedStartupStagesRollBack`、`repeatedLifecycleReturnsResourcesToBaseline` 并写 QTest 文件，62.872 秒：前者 baseline/final 232→269；后者 handles final/baseline 488/291，threads 6/7，children 0/0，线程/子进程检查先通过，仍仅两条句柄断言失败。未改断言或生产 ConPTY 路径；本轮数值不冒充旧调查的严格增长率。

全套之后的局部收尾只修正 CR 覆写可能使原始片段逆序时的 gap envelope 起止位置，并补显式标准头；主程序及相关目标复建成功，最后双协议 HTTP **3/3**、搜索状态 QTest **10/10** 通过。不把局部复验写成第二次全套。

复测命令（先进入 vcvarsall x64；Qt bin/plugins 来自 C:/Programs/Qt/6.8.3/msvc2022_64）：

```powershell
cmake --build build/Release
ctest --test-dir build/Release -C Release --output-on-failure
ctest --test-dir build/Release -C Release -L mcp --output-on-failure
```

完整日志在 `build/p3-build-final.log`、`build/p3-final-ctest.log`；录制 UI 的断言位于 `build/Release/tests/history/HistoryRecordingUiTests.ctest.txt`。
ConPTY 文件为 `build/p3-conpty.txt`；最后局部构建/复验为 `build/p3-post-review-build.log`、`build/p3-final-http.txt`、`build/p3-final-search.txt`。
`git diff --check` 通过；面板截图 `build/p3-history-panel.png` 已查看（该预览未绑定共享 port，按钮禁用属预期）；实际共享客户端/范围选择由 UI 用例验证。
实现与上述首次验证阶段未提交/推送，未连接真实设备；后续提交按用户明确授权执行。

## 0.2.81 提交前增量

用户明确要求创建提交并推送；依 AGENTS.md 将 PATCH 从 0.2.80 升至 0.2.81，沿用 master 并推送项目主仓库 Gitee。
补齐根 README 测试入口、架构目录映射和两处项目阶段状态链接；不更改业务实现。
用户随后明确要求不再运行测试，停止提交前追加复验；未执行 0.2.81 新一轮 CTest，不把上述 0.2.80 实测结果改写为升版后的新成绩。
