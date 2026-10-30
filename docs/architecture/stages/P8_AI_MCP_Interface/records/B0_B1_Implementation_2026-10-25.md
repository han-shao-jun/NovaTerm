# B0/B1 实施与验收（Windows，2026-10-25）

本记录属于本轮实际工作；旧 P 编号仅追溯。用户授权退役被替代或已放弃方向的内部实现，不自动提交/推送，不连接真实 SSH/UART。采用 effective-cpp 的 RAII、值语义、非 owning 会话引用和窄端口；实施范围与验证以下文为准，过程计划可从 Git 历史查阅。

## 1. B0 工作区与真实基线

- 初始 HEAD：`179222682190aa285811675e89e4756eade8550e`。原始 tracked patch/status 在 `build/b0-b1-2026-10-25/initial-tracked.patch`、`initial-status.txt`。工作树含文档重组和未发布 P4，未作 reset/checkout。
- 初始源码 ToolCodec 在冻结九工具及三个历史工具之后追加 P4 两项，源目录实际为十四项；fixture 仍冻结十二项，部分 P4 测试将计数改为十四并绕过历史尾部比较。十四项未通过本轮运行验收，不能称发布基线。
- 构建缓存为 `build/Release`、Ninja、MSVC 14.51、Qt 6.8.3，C++17。实际存在 `C:/Programs/MicrosoftVisualStudio/18/Insiders/VC/Auxiliary/Build/vcvarsall.bat`，仓库说明里的另一 BuildTools 路径不存在。本轮使用实际路径，不修改编译器/Qt 版本。
- 已冻结 B1 的字段、预算、owner、错误和测试；B2/B3 的清空权限/幂等、文本等待、长任务、文件/目标连接继续分批冻结，不宣称整个未来 B0 清单已完成。

## 2. 盘点分类及清理

| 分类 | 处置及消费者判断 |
| --- | --- |
| 当前闭环复用 | InputPump/Parser/Core/ChunkedScrollback；SessionDirectory/AccessStore/CapabilityDispatcher；CommandRuntime、Controller/Arbiter/EvidenceModel；十二工具中的固定命令和离线历史/录制/共享/证据 |
| 新实现替换 | 把模型 revision/去重摘要当精确日志的使用方式，由显式日志位置替代；新实现不维护摘要副本或客户端全文缓存 |
| 已放弃专属实现 | 删除未发布 `list_diagnostics/run_diagnostic`、diagnostics 目录（Provider/Resolver/结果/目标 Profile）、MCP codec/适配器、BackendDiagnostic 组合事件、确认/配置 UI、相关 include/链接/CMake 和三项 P4 测试及新增夹具 |
| 无生产消费者的旧内部链 | P4 删除后，P1 DiagnosticTaskManager/SessionRecipeAdapter/SessionExecutionRouter、EnvironmentTracker/TargetEnvironment 只剩专属测试；删除重复执行/取消/就绪状态和 Session 钩子、两个专属测试目标及两条 MCP Recipe 用例。保留 CommandRuntime/Controller 现行行为的安全回归 |
| 旧本机 Agent 原型 | 删除 LocalAgentAdapter 和 LocalAgent/Cli 来源、Recipe policy，单一实际入口的 CapabilityEntry 模板合并为 McpCapabilityAdapter RAII owner；通用准入、绑定、撤权、取消、断连与独立入口关闭测试迁移到两个独立 MCP 入口，未放宽断言 |
| 删除无调用旧投影 | TerminalSession::terminalContext 和 TerminalContextProvider::context/Request/Context 同步投影及无调用 reset helper已无生产调用；删除重复有界裁剪。折行接缝、进度过滤、重复摘要、备用屏与 UTF-8 预算断言迁移到 trySnapshot/Core tryTerminalState，真正 MCP 预算仍由现有 ObservationService/契约测试保护 |
| 有消费者的旧摘要 | TerminalContextProvider/TerminalStateCache/Filter、ObservationService、ObservationState 和 capture 搜索保留：服务旧 read_context、compact 交互结果、inputToken 和 search_context/expandToken；日志源绕过它们，兼容源各自明确，不是重复日志实现 |
| 无关/不明确改动 | 保留用户已有文档重组、历史证据和其他改动，不删除本机配置或持久录制/共享格式；P4 曾写入的本机 Profile 文件不清理，已发布十二工具的数据格式不迁移 |

P4 源码与定向退役接线 patch 分别归档在 `build/b0-b1-2026-10-25/retired-p4-sources.zip` / `retired-p4-integration.patch`；P1 源码在 `retired-p1-sources.zip`。这些是本地恢复材料，不参与编译或安装。历史 P4 失败记录保留；退役依据为产品范围及无消费者，不以失败为理由。

## 3. B1 实际实现

`TerminalLog.h/.cpp` 属于现有纯 C++ history 后端，`ChunkedScrollback::readLog` 在调用方持有 Core 模型锁时读已稳定历史逻辑行。`TerminalCore::tryReadLog` 只 try-lock；SessionLogPort → 无状态 readLog 应用用例 → 协议适配各司其职。没有第二采集线程、全历史 snapshot、同步冷解码或每客户端日志副本。

位置含 generation、LineId、cell/scalar 和累计 UTF-8 byteOffset；Cell/码点坐标使超长行从断点编码，无需反复扫描行首。查找按 chunk/行 ID 二分，活动行锁内只借用，封存行用有界 pin，出锁仅传值。保持真实相同内容的不同 ID、空行和 LF；当前仍可续接的软尾行不发布日志；重连后遗留的非尾部软行可读，以 hardBreak=false/lineComplete=true 明示，不人为补 LF。尾书签不清空。

页面上界独立于 model revision；只推进交付位置，UTF-8 标量不切断。hasMore/caughtUp 仅对本次采样上界成立。冷页 pending/busy/unavailable 与淘汰 gap 分开；有已交付片段时可携带部分进度。clear 与 resize 的 sb_popline 回填改变 generation；invalidated 的 nextToken 保持旧代际，必须显式选择 earliestToken/sampledEndToken 新基线。Session epoch 与客户端授权版本另外绑定。

签名密钥由 Service 持有，跨同一进程内同客户端的协议连接可续读；显式绑定 client/session/epoch/grant。日志本身不授予输入。交付前应用用例及 Service lease 再核验权限。参数错误沿用 INVALID_ARGUMENT，类型/签名/授权版本不符 CURSOR_STALE，不足一个标量 TEXT_BUDGET_TOO_SMALL；模型锁忙返回 BUSY，其他缓存状态在日志页中明确表达。

## 4. 兼容契约

工具仍为十二项。`read_context` 增加 source/start 和日志输出分支；旧参数的 required/default/预算、旧观察输出分支及其他十一工具对象保持。新 fixture 为 `tests/mcp/fixtures/read-context-log-2026-10-25.json`，旧 fixture 保留并逐旧字段检查，历史三工具仍完整比较。两代握手共用 agentInstructions，明确完整输入的广泛授权、UART 静默非验证完成、未知操作不重放以及未实现工具不能调用。

既有 model revision/sinceToken、compact/full、capture/expandToken 的含义未修改。日志不能用于 search_context；没有创建 capture/inputToken。活动行、CR 覆写、备用屏通过 observation 读取，日志不保存被覆写的屏幕帧。仅保证仍保留的已稳定解析后历史，不承诺原始字节、无限保留或硬件零丢帧。

## 5. 验证账本（持续追加）

- 契约 RED：新增 log 参数被旧 codec 拒绝，`red-contract.txt`；实现后同测试 GREEN：`green-contract.txt`。
- 缓存重复 Unicode/空行分页、tail/oldest、预算、淘汰、clear/pop 失效：`cache-tests.txt`，4/4 QTest（含 init/cleanup）。
- 纯应用/契约相关 CTest 5/5，`pure-tests.txt`。HTTP 新用例在沙箱内因 Windows 私有目录 ACL 失败于 enable()，未进入 B1；沙箱外 SSH/UART × 两代协议四组均通过，QTest 6/6，`log-integration-outside.txt`。
- 构建中发现缺少 TerminalCore 显式 include、测试误用私有版本常量、两处遗留 LocalAgent include，均在源码定位后修复；原始构建日志保留。未用删除行为测试或放宽断言处理。
- 冷块/长行回归通过，后续增加 >256 KiB 的未压缩活动行检查，未封存活动块；Core 屏幕/CR/备用屏与本机 clear/resize 兼容回归通过。
- 首轮全套 44/47（331.90 秒），失败为 Wire 对旧 dataSchema 层级的断言、审查/构建交错期间的 Scrollback 失败（该轮未冻结产物且 stdout 为空，不单独归因）及 ConPTY；此轮产物在审查中变化，仅保留为诊断记录，不当作冻结版本成绩。Wire 原保护行为已迁到 anyOf 的 observation 分支，并增加 log 分支严格字段断言，未放宽断言。
- 最终固定产物构建/全套结果在本节末追加；不得把首轮与历史成绩拼成一次全绿。

## 6. 剩余与下一会话起点

下一阶段 B2：先冻结清空工具的独立权限、operationId/幂等、范围、队列接受与 Parser 完成事实，再实现经现有命令队列的非阻塞清空事务。复用本轮缓存 generation；补齐 Provider/观察 capture 清空失效（G05），不能只让日志失效。旧摘要清空残留不属于本轮兼容改写。

B3a 文本条件等待/长任务、B3b SFTP 单文件及保存目标连接都未实施；也未连接真实服务器/串口。真实 UART/SSH、其他平台、消毒器、长期/高输出性能需另行验收。P4 不再是前置或待续实施路线。


## 7. 独立审查与裁决

按代码审查技能派发一个只读 reviewer，未递归分派。审查检查源查找、UTF-8/LF 分页、锁/pin 所有权、签名/授权、跨模式与清理消费者，检出两项 P1：

1. resize 回填后行 ID 可有空洞，id+1 不是实际后继。`logResumesAcrossIdHolesAfterHistoryPop` 先失败（next=3，应为4），改为元数据二分后继并在返回前规范化，之后通过；继续淘汰已交付前缀不会误报缺口。
2. 保留历史的 resetForConnection 可遗留非尾部 hardBreak=false 行。`logReadPassesAbandonedSoftHistoryAfterConnectionReset` 先返回 unavailable，修复为读取稳定旧片段，显式 hardBreak=false、不补 LF，随后通过。当前还在增长的软尾行仍不发布。

其余未发现高可信阻断项。reviewer 未判断真实设备/吞吐、跨平台/消毒器、全套最终成绩、尚在编辑的文档及旧 observation 的全量缺陷；这些保持相应未验收边界。作者负责后续未调用同步投影的机械清理和测试迁移，未再次派发 reviewer。当前没有主动延期的审查 minor。


## 8. 本轮可复现命令与产物

Windows 命令前置：PATH 包含 `C:/Programs/Qt/6.8.3/msvc2022_64/bin`，QT_PLUGIN_PATH 为同前缀 plugins；不全局设置 offscreen。构建先 `call C:\Programs\MicrosoftVisualStudio\18\Insiders\VC\Auxiliary\Build\vcvarsall.bat x64`，再 `cmake --build build/Release`。最终完整构建退出 0，日志 `build/b0-b1-2026-10-25/final-frozen-build.txt`。

最终 CTest 使用固定产物：`ctest --test-dir build/Release -C Release --output-on-failure`，日志 `build/b0-b1-2026-10-25/final-frozen-ctest.txt`。当前实际注册 47 项，MCP 标签 16 项（原 P1 两专属测试目标已退役，P4 三目标已撤销注册）；注册数不是通过数。

独立无 Qt 后端：`cmake -S tests/history-noqt -B build/b0-b1-2026-10-25/history-noqt-verified -G Ninja -DCMAKE_BUILD_TYPE=Release` → build → ctest，三步退出 0，CTest **1/1，0.12 秒**。日志 `noqt-configure-outside.txt`、`noqt-build-outside.txt`、`noqt-ctest-outside.txt`。首次沙箱配置被 MSVC C1902/PDB 管理器阻断，原日志 `noqt-configure.txt` 保留；不放宽工程要求，改在正常 Windows 环境验证。

无调用同步投影清理后，迁移的 Session 两用例 **4/4 QTest**（含 init/cleanup，6 ms），日志 `migrated-summary-tests.txt`；最终 Scrollback 目标 **31/31 QTest**（893 ms），日志 `frozen-scrollback-detail.txt`，包含冷页、>256 KiB 未压缩活动行、UTF-8、ID 空洞和淘汰。

本轮当前文档本地链接检查零缺失；`git diff --check` 通过。没有 Git 提交/推送、真实 SSH/UART、跨平台或性能/消毒器运行。


### 固定产物最终结果

- **Windows Release 全部默认目标构建成功**：`final-frozen-build.txt`，退出 0。
- **CTest 46/47，326.68 秒**：`final-frozen-ctest.txt`。唯一失败目标为 `novaterm_conpty_tests`；最终失败清单只有 #22。MCP 标签 **16/16**（81.19 秒），Core/Scrollback/Session、TerminalSession、渲染、UI、录制、历史/资源与文件传输均在本次全套中通过。未从这一结果推断 GPU 专项、真实设备或其他平台通过。
- 独立 no-Qt **1/1**，上述摘要迁移和日志 QTest 文件输出通过。新契约、legacy/历史 fixture、两代 instructions、授权事务与运行内核均在最终全套中通过。
- ConPTY 详细断言用 `novaterm_conpty_tests.exe -o build/b0-b1-2026-10-25/conpty-detail.txt,txt` 单独读取；该补充结果另列，不能把已知平台失败写成全套通过。

本轮产品代码和测试在最终全套开始前已固定；首轮诊断成绩与最终 46/47 分开保留。后续仅同步文档，未修改断言掩盖失败，未提交/推送。


ConPTY 文件复核：**10 passed / 2 failed，46.461 秒**，仍为既有 `injectedStartupStagesRollBack` 末条句柄上限与 `repeatedLifecycleReturnsResourcesToBaseline` 句柄严格相等断言。重复生命周期明确打印线程 **6/6**、测试子进程 **0/0** 并通过检查；该轮句柄 **243/246**（final/baseline），减少也触发严格相等，不据此推断历史 +200 句柄增长率。原生产代码和断言未修改。日志 `conpty-detail.txt`；本轮未重查系统根因，保持既有问题边界。


## 0.2.83 提交前收口（2026-10-25）

用户明确要求创建 Git 提交。按 AGENTS.md 当前 0.2.x 约定将根版本从 0.2.82 递增至 0.2.83；旧 commit_check.md 的 0.1.<提交数> 写法与现行约定冲突，本次以 AGENTS.md 为准。提交包含 B0/B1 日志契约/实现/回归、未发布原型清理、文档路线重组，以及随后按职责拆开的 history/recording/historysearch 模块和测试。

版本递增后 Release 全部默认目标重建通过，日志 `build/mcp-0.2.83-commit/build.txt`。PowerShell 设置 Qt bin/plugins 后运行 `ctest --test-dir build/Release -C Release -L 'mcp|recording|history-search|resources|evidence' --output-on-failure`，本轮 **23/23，90.28 秒**；资源共享另单项 **1/1，0.35 秒**。日志 `ctest.txt`、`resource-test.txt`。这是新的提交前专项，不声称本轮重跑全套。此前目录重构全套 **46/47，351.54 秒**、独立无 Qt **1/1** 单独保留，唯一既有失败为 ConPTY。

已完成两次独立只读审查及相应修复/迁移校验；当前 diff --check 通过。未实施 B2/B3，未连接真实 SSH/UART、未做其他平台/消毒器或性能验收。此次只创建本地 Git 提交，不推送。
