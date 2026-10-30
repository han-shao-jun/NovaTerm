# B2 清空与代际实施与验收（Windows，2026-10-25）

## 1. 基线与范围

起点 HEAD `3345fd3`，NovaTerm 0.2.83；实际源码已具备 B1 日志位置、UTF-8 分页、淘汰/清空/回填代际和十二工具兼容基线。本会话前置审查实跑 Scrollback 31/31、契约 17/17、读取用例 12/12、Core 日志专项 4/4（QTest 数含 init/cleanup），随后修正 01 第 7 节过期测试清单。该文档修改在 B2 开始时已存在，予以保留。

本轮只实施 B2/G04/G05，不提交、不推送、不连接真实设备；其他本机辅助目录（包括运行期间出现的 `.claude/`）不纳入修改。B3 文本等待、长任务、文件/连接工具和 B4 真机/性能仍未实施或验收。实施范围与验证以下文为准，过程计划可从 Git 历史查阅。

## 2. 实际实现与所有权

- 原十二工具对象兼容保留，独立追加第十三项 `novaterm_clear_terminal`。原 `ToolRequest` 仍固定九项；ClearRequest/BackendClearCall 只在协议组合层扩展。新工具使用既有能力准入和 Service 请求队列/租约，不创建第二执行器。
- `PermissionKind::ClearTerminal` / `PermissionSelection::clearTerminal` 默认 false，复用 AuthorizationService 行版本/绑定/客户端版本事务；读取撤回级联清空，连接换代隐藏旧授权。已有完整输入 grant 不升级。设置界面提供独立选择，旧 grant 本就仅在内存，无新增持久格式。
- 应用 `ClearOperations` 唯一持有有界清空操作记录：每客户端 32、全局 128、每操作最多六个等待者。终态保留到原票据过期；未完成记录不因超时丢弃。满即 BUSY，不驱逐有效结果。
- 日志页显式追加限时 `clearToken`，保持 B1 nextToken 的确定性。签名绑定 client/session/epoch/grant/cacheGeneration、现有 Arbiter 人工输入代际和单调签发时刻，期限十分钟。原票据过期不能重新受理原请求，即使记录已回收；窗口外不承诺永久记 UUID，调用方不得复用 ID 或在未知结果后更换票据重放。
- Session 窄接口转交 Core 现有有界 Parser 队列及 byteBarrier。队列拒绝不返回成功；Parser 执行前检查原子取消闸门及 expectedGeneration。Core 在执行点保存完成代际和空尾 nextLine，GUI 完成事件携带关联 ID；响应基线来自此事实，不从稍后状态重采样。精确重试回放原完成事实、时间戳与基线。
- ChunkedScrollback 仍唯一拥有 generation；Core 只发布常量时间原子投影。菜单、控制序列清历史、resize 回填与 MCP 共用失效源。Provider 重置摘要/历史水位、拒绝旧代际发布物；合并快照、两类 capture、expand/search、观察及输入 token 均复核代际，入口与迟到交付不依赖 GUI 失效通知及时到达。
- 已排队但未写出设备的旧观察输入在 Controller 真正提交给 Arbiter 前复核原 generation。拒绝本次输入不删除既有运行命令事实。清空不发送设备字节、不停止远端程序、不重连、不删除录制/离线共享。

## 3. RED/GREEN 与审查修复

证据目录：`build/b2-2026-10-25/`，除明确注明外均为本轮产物。

| 验证 | 原始证据与结论 |
| --- | --- |
| G05 Provider 残留 | `provider-red.txt`：clearAll 完成后旧 entries 仍存在；Provider 修复后专项通过。含旧 published 在真实模型忙时不能复活的回归 |
| Core 清空事务 | `core-b2-green-final.txt` 等：关联完成、字节屏障、预先/排队取消、代际冲突、队列拒绝、clear/control sequence/resize 同源。后续补两次清空间新输出的执行点基线隔离，最终全套覆盖 |
| 独立授权 | `permissions-red-build.txt`：新权限/API 缺失 RED；实现后纯授权和实际 UI 点击回归覆盖默认隔离、读取级联、重连、旧绑定/行版本与刷新保持 |
| G05 应用用例 | `g05-red-observation.txt` / `g05-red-read.txt`：旧 inputToken 借 Replay、同窗旧快照、旧 context 游标三项行为 RED；`g05-final-observation.txt` / `g05-final-read.txt` 为 GREEN |
| 新协议 | `clear-contract-red.txt`：工具未注册 RED；新 Schema/完整目录 fixture、旧十二项对象与 B1 字段兼容检查随后通过 |
| 两代 HTTP | `clear-integration-3.txt`：2025/2026 × SSH/UART 四组替身通过（QTest 6/6），覆盖独立权限、scope、完成基线、旧观察/log/capture/expand/inputToken 失效、跨连接精确重试、新日志不被二次清空和撤权 |
| 有界幂等 | `ClearOperationsTests` 覆盖原记录回放、参数冲突、票据过期后回收与拒绝、限额、取消等待者、迟到完成和回调重入回收 |

独立只读审查发现并修复：

1. Adapter 以 Session 对象指针去重 watch，却捕获首次 Session ID；同对象 resetForReuse 后会漏掉人工取消与销毁收尾。改为记录稳定来源标识，复用后仍能关闭所有该对象的未执行闸门并回收等待者。`reuse-behavior-red.txt` 确认旧映射会错误 applied；`review-final-green.txt` 两条专项通过（QTest 4/4）。
2. 已编码但 GUI 尚未写出的旧输入可在清空后迟到发送。Controller 保存调用方原 cacheGeneration，在实际 write 前复核；拒绝迟到回复时保留已有命令事实。`build/Release/b2-deferred-input-red.txt` 九组原始失败，`deferred-input-green.txt` 九组及旧代际直接提交用例共 QTest 12/12 通过。
3. 本轮自查补充人工输入先于首次清空受理的路径：clearToken 绑定 Arbiter 用户代际，旧票据新受理返回 CONTROL_INVALID；`review-behavior-red.txt` 与 `review-final-green.txt` 保留 RED/GREEN。已完成精确重试仍只回放原结果。

集成期间还修正了 HttpMcpEndpoint 未登记 BackendClearCall、Service 默认旧请求误入 list_sessions、fixture required 顺序与 MSVC 测试局部名称/不必要 constexpr 捕获等问题。原构建/失败输出保留，未放宽编译选项或断言。

复用回归初次大输入触发默认 Fake 不支持 setReadPaused，导致会话失败而非预期取消。专项改用支持背压的替身后复现真实缺陷，保留 CANCELLED/原结果与零重复清空断言。沙箱内 Session 四个私有临时目录拒绝访问也与产品错误分开：沙箱外原整目标 106 pass/0 fail/4 预期 skip（15.469 秒，`session-full-unsandboxed.txt`）；最终新增 Controller 用例的成绩以最终全套为准。

## 4. 最终固定版本验证

构建环境复用已有 `build/Release`（Ninja/MSVC 14.51、Qt 6.8.3、C++17）。构建先调用实际存在的 `C:/Programs/MicrosoftVisualStudio/18/Insiders/VC/Auxiliary/Build/vcvarsall.bat x64`，再 `cmake --build build/Release`。最终完整构建退出 0，`frozen-build.txt`。

测试 PATH 加入 Qt bin、QT_PLUGIN_PATH 指向同前缀 plugins；不全局设置 offscreen。正常本机环境执行 `ctest --test-dir build/Release -C Release --output-on-failure`，避免沙箱临时目录私有 DACL 造成环境失败。当前实核默认注册 48 项，MCP 17 项；注册不是通过结论。最终固定全套 **47/48，348.53 秒**，日志 `frozen-ctest.txt`，退出 8；唯一失败 #23 `novaterm_conpty_tests`。MCP 标签 **17/17（85.99 秒）**，Core/Scrollback/Session、TerminalSession、渲染/UI、录制/历史/资源及文件传输均在这次全套中通过，不拼接较早轮次，也不据此宣称 GPU 专项或其他平台通过。

新配置的独立无 Qt 后端：`cmake -S tests/history-noqt -B build/b2-2026-10-25/history-noqt -G Ninja -DCMAKE_BUILD_TYPE=Release` → build → ctest，三步退出 0；**1/1，0.13 秒**。原始命令输出为 `noqt-configure.txt`、`noqt-build.txt`、`noqt-ctest.txt`。未从此推断整个 Core 不依赖 Qt。

ConPTY 另用 QTest 文件输出复核：`conpty-detail.txt`，**10 passed / 2 failed，49.543 秒**；仍仅 `injectedStartupStagesRollBack` 的句柄上限和 `repeatedLifecycleReturnsResourcesToBaseline` 的严格相等断言。后者句柄 286/289、线程 4/6、测试子进程 0/0（final/baseline），线程与子进程检查明确通过。瞬态句柄读数有波动，且减少也触发严格相等，不据此推断增长率或修复系统根因；生产 ConPTY 代码和原断言未修改。

本轮新增文档链接检查零缺失；最终包含 AGENTS.md 历史段落的检查发现两条既有失效链接（Merge_Gitee_2026-10-22.md / Merge_Gitee_2026-10-23.md，HEAD 中已存在），保留为非 B2 文档遗留，未恢复已删除历史文件。`git diff --check` 通过（仅既有换行格式提示）。最终构建与全套开始后产品代码/测试未再修改；后续仅追加文档成绩。未自动提交/推送。

## 5. 剩余与交接

B2 替身/Windows 验收不替代真实设备、其他平台、消毒器、长时间/高输出性能或 B4 双通道闭环。清空不能追回已交付给 Agent 的文本；取消与执行已开始之间没有远端回滚承诺；应用重启无持久操作记录，旧票据签名失效，未知操作不得重放。

下一阶段为 B3a 指定文本条件等待/长任务；消费现有日志与屏幕位置、统一 cacheGeneration、Controller/CommandRuntime，不建立第二采集线程/日志库。B3b 文件传输/保存目标连接仍只在 06 设计中，未加入工具目录。

## 2026-10-26 后续审查修复

本记录固定版本之后又修复 expand 代际失效错误码，并明确/核验输入提交线性边界；新一轮构建与相关18项验收见 [审查修复记录](B2_Review_Fixes_2026-10-26.md)。本记录的47/48不替代后续专项，后续专项也不改写本轮历史成绩。
