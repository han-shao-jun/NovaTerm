# B2 审查修复与提交边界核验（Windows，2026-10-26）

本轮处理 Qt/C++ 只读审查的 D-001 与 I-001，保留已有 B2 未提交改动，不提交/推送、不连接真实设备。此前 B2 全套 47/48、MCP17/17、独立 no-Qt1/1 是 2026-10-25 的历史固定版本成绩，不作为本轮结果。

## 1. D-001：expand 代际失效错误码

`TerminalInteraction::expand` 原先将 generation 失配和投影过期合并返回 `CAPTURE_EXPIRED`，与 B2 冻结的 `CAPTURE_NOT_AVAILABLE` 不同。

修复先验证签名/身份/授权绑定，再比较 token 与现存 capture 的 generation，最后检查投影是否仍保留及有效。代际失配返回 `CAPTURE_NOT_AVAILABLE`；未换代的投影淘汰/过期保留原 `CAPTURE_EXPIRED` 行为。即使投影已经淘汰，签名 token 的旧 generation 也能正确报告主动失效。

新增/加强断言：

- 两代协议 × SSH/UART 四组清空后的 expand 精确错误码。
- 本机 all/history 清空、设备 ESC[3J、resize 回填四组错误码。
- 同一旧 token 在“投影已淘汰、代际未变”时为 `CAPTURE_EXPIRED`，清空换代后为 `CAPTURE_NOT_AVAILABLE`。

原始证据在 `build/b2-review-fixes-2026-10-26/`：`expand-red-outside.txt` 八组确实因错误码不符失败；`eviction-red.txt` 额外复现了投影先淘汰再换代的分支。最终 `expand-green.txt` 为 **QTest 11/11（含 init/cleanup，7.786 秒）**。未以 `ok=false` 代替精确错误码或放宽断言。

## 2. I-001：明确输入提交线性点

该项原为 70/100 的待验证风险。最后一次 generation 检查和 ITransport::write 之间不能声称与 Parser 清空物理互斥。本轮没有添加模型锁下的 Transport I/O，也没有改变输入仲裁/Parser 的状态 owner。

裁决：以 Controller 的 GUI 编码回调内最后一次成功代际复核为输入受理点。清空在该点前完成则仍排队的旧输入零写入；通过该点的提交已开始，允许与 Parser 清空重叠。清空的 GUI 完成通知及 MCP 响应不会插入当前同步 GUI 提交回调。该约定不承诺已受理输入与物理字节发送互斥，已在 02/04 及代码注释明确。

新确定性测试 `cacheClearCanOverlapAnAdmittedTransportWrite` 使用实际 Core/Arbiter/Controller 和可控 Transport：在 write 回调内提交并等待 Parser clear，证明模型可以先变代际；GUI 完成通知仍须按 `write-enter → parser-cleared → write-return → clear-notified` 的顺序交付，无任意 sleep 或生产测试钩子。

原九组 `cacheInvalidationRejectsDeferredAgentInput` 与旧代际直接提交拒绝断言全部保留。`admission-green.txt` 合计 **QTest 13/13（含 init/cleanup，201 ms）**。这里是提交边界澄清与回归核验，不把它写成新增物理写入互斥修复。

## 3. 构建、专项与环境区分

- `green-build.txt`：相关 MCP/Session 目标构建退出 0。
- `final-build.txt`：沙箱默认构建在 NovaTerm 的 `/Zi` 编译处被 MSVC C1902/PDB 管理器阻断；未修改源码或编译选项掩盖问题。
- `final-build-outside.txt`：同一固定源码在正常 Windows 用户环境完整 Release 默认目标构建退出 0。
- `final-ctest.txt`：单次相关 CTest **18/18，106.60 秒**，包含 MCP 标签 **17/17（91.21 秒）** 与 Session 目标。命令为 `ctest --test-dir build/Release -C Release -R '^novaterm_(history_contracts|capabilities|capability_entry|mcp.*|sftp_session_safety|command_safety|resource_sharing|session)_tests$' --output-on-failure`。

本轮测试目标在该次 CTest 启动前已完成构建，18 个目标可执行文件均未在测试期间重建；核验其 LastWriteTimeUtc 均早于 `final-ctest.txt` 的创建时间。默认应用构建的沙箱失败与这些已固定测试产物分开报告，不拼接成全套成绩。

最初沙箱 `expand-red.txt` 在 `enable()` 被临时目录私有 ACL 阻断，未进入目标错误码断言；真正行为 RED 以正常环境的 `expand-red-outside.txt` 为准。原环境日志保留。

按 AGENTS.md 本轮仅改 MCP 局部行为、Session 提交注释及相关测试/文档，未改公共数据布局、CMake 或第三方依赖；未运行无关全套、ConPTY、GPU 专项、真实设备、其他平台、消毒器或性能验收。历史 ConPTY 失败不作为本轮回归。下一功能阶段仍为 B3a。

## 0.2.84 提交前收口（2026-10-26）

用户要求创建提交并同步最新代码。按 AGENTS.md 每提交递增 PATCH，将根版本0.2.83→0.2.84；README 工具目录描述同步为十三项。版本递增后正常 Windows 环境完整 Release 默认目标构建成功，原始输出 `build/b2-commit-sync-2026-10-26/0.2.84-build.txt`。同版本相关 CTest **18/18，108.46秒**，日志 `0.2.84-ctest.txt`，包含 MCP17项与Session；本轮未重跑无关全套。

fetch 核对 GitHub/master 仍为1792226，Gitee/master 新增e1fb2cc（会话窗口按钮固定/表单独立滚动与SFTP覆盖确认）。先提交本地B2及审查修复，随后另行合入远端UI变更并按项目合并规则验证；两轮成绩各自记录，不以本次专项代替合并后验证。
