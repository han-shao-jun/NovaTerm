# B3 基础恢复与 B3a 文本等待、长任务（Windows，2026-10-27）

本轮固定记录之后，用户另要求修复独立审查发现的两项P2；最新增量和新一轮成绩见 [B3审查修复](B3_Review_Fixes_2026-10-27.md)。后续专项不改写本页历史成绩。

## 1. 起点与范围

起点 HEAD `ffcd1de`（0.2.85），工作区初始干净。已核对 AGENTS、00～06、B0/B1、B2实施及2026-10-26审查记录与当前源码；B1日志位置、B2独立授权清空和缓存代际直接复用，未重复实施。用户要求本轮实施，不连接真实设备、不自动提交/推送、不实施B3b。

本轮新增四工具 `novaterm_wait_text`、`novaterm_start_task`、`novaterm_query_task`、`novaterm_stop_task`，当前共十七项。原十三工具对象和旧九项ToolRequest联合兼容保留；新增请求仅在协议组合层分派。精确字段/预算见 [02](../02_Current_MCP_Contract.md)，新目录夹具 `tests/mcp/fixtures/workflow-tools-2026-10-27.json`。根版本未修改。

## 2. 实现与所有权

- `WaitTextRequest` / `TerminalTextWait` / `TextWaitMatcher`：只读GUI有界调度，日志复用B1 `readLogContext`，屏幕复用ObservationService不可变基础快照，不产生额外capture/采集线程。日志默认首次成功采样尾部，屏幕显式current/next；any/all、大小写、UTF-8/页间尾部与真实LF、扫描位置和匹配状态一并续接。
- 等待最多8候选/每项256 UTF-8字节；每客户端8/全局32活动及保留状态，初始受理起固定十分钟。单次0..30秒，扫描默认256KiB/最大1MiB；每轮16KiB，最多四个等待或8ms后让出事件循环。只保留有界尾部、八份命中上下文及位置，不固定历史块、不保存任务全文。
- 日志`nextToken`明确为扫描进度，原`startToken`可独立回读全文；源pending/busy/unavailable的有效前缀也扫描并采用真实B1 next，空页不推进，不零延迟忙等。屏幕不跨revision拼接，预算不足不消费半帧；匹配不证明测试成功。
- `TerminalTaskRequest` / `TerminalTasks`：有界索引只保存原operation、Controller执行关联、提交前B1尾书签及有限完成事实；输入仍经TerminalInteraction→Controller→Arbiter→现有Transport，不建立第二执行器。raw输入不要求POSIX包装，command保持就绪规则；执行模式shared_terminal，不改cwd/env。
- 任务每客户端32/全局128，活动不提前驱逐，终态保留十分钟。短RPC使用独立关联key，取消/断流不删除已受理任务或再输入；按taskId或原启动operationId查询。新受理严格复用ObservationState输入token校验，不借旧普通交互的Replay分支创建新任务。
- query不触摸输入租约，活动任务自身按原Controller续期，人工输入立即抢占。期限到达使用现有release/unknown收口，无Ctrl+C。UART静默保持running，期限后unknown，退出码为空且未确认终止；SSH只采用实际Controller完成证据。
- stop经原输入通路请求一次Ctrl+C，受理和编码完成后的实际提交前均核对原user/control/execution；人工接管、可信ShellReset/FramingError、执行完成、新操作、撤权或断连拒绝旧停止，不能取得新token后抢回控制。停止请求不保证设备响应或进程终止。
- 目录availability分别报告可启动/可观察、完成证据、停止条件、可验证退出码与不可用原因。两代握手仍共用agentInstructions，工具描述、05/06与MCP_Usage同步。

## 3. 新增、替换和删除

新增上述两个独立DTO、两个适配器、有界字面匹配器、统一预算/TaskNotAvailable错误映射、新四工具fixture和两个专项目标。协议ToolCall/BackendEvents、Wire/HTTP、Service装配与能力注册扩展，不扩大旧终端应用联合。

替换/修正局部路径：服务重新启用时创建新任务索引；RPC无法交付续扫票据时用discard回收等待；Ctrl+C后丢弃未提交输入跟踪、仅在新可信prompt恢复command就绪；可信reset释放控制/推进代际并关闭排队停止闸门。SDK互操作脚本从历史12项目录更新为17项并实际调用四个新工具。

没有删除仍有效公开工具、原有测试或断言，没有恢复P4诊断/P1 Recipe/本机Agent原型，没有新增B3b工具。现有独立录制/历史资源授权及旧串口静默完成契约保留。用户运行期间出现的非本轮文件（例如根目录`pelican-bicycle.html`）未修改。

## 4. 基础交互证据

| 范围 | 源码/替身证据 | 限制 |
| --- | --- | --- |
| SSH/UART原始输入 | 新Session矩阵12行：CR/LF/CRLF、Ctrl+C、Tab、四方向，normal/application cursor；精确字节/动作数，原始输入无需prompt，Enter保持CR | 伪Transport，不是UART驱动或SSH服务器验收 |
| 集成Shell恢复 | 新4行：SSH/UART×空闲/未提交文本，Ctrl+C发送后仍不可command，新PROMPT才恢复；既有四行尾部/多行保护保留 | 不证明任意远端Shell支持集成 |
| 就绪/环境 | 既有Session/Framer测试覆盖配置prompt、密码提示、alternate screen、无prompt、trusted/unverified profile；基础核验矩阵`build/b3-2026-10-26/b3-audit.md`逐函数列证据 | 普通UART启动文本不猜epoch/固件轮次，需调用方核对 |
| 输入保护与恢复 | 原inputToken/replyToExecutionId/operationId、人工优先/撤权/断连/重新绑定断言保留；任务补4行可信reset前/编码中停止零写入 | 同链路真实重启、短U-Boot抢键待B4 |
| 两代完整接线 | 2025/2026×SSH/UART四组：旧PASS、屏幕login、新任务、跨连接原ID查询/精确重试、参数冲突、日志、人工接管旧停止零写入与撤权 | 本机HTTP+替身，不是外部产品客户端或板卡 |
| 取消/重新启用 | 两代各20次后端受理后的取消/断流回收，再新等待与停用/启用后的新任务 | 2025使用取消通知；2026以对应HTTP请求断流取消，不能向无状态入口发送旧通知 |

## 5. RED/GREEN 与独立审查

证据目录统一为 `build/b3-2026-10-26/`（任务开始于26日，完成记录为27日）。以下各轮各自保存，不拼接成一次测试成绩。

| 问题/功能 | 真实失败与修复证据 |
| --- | --- |
| 新契约缺失 | `contracts-red.txt`：wait工具未注册；实现后`contracts-final.txt`20/20 |
| 任务原始四项 | `task-red.txt`：NOT_IMPLEMENTED，2pass/4fail；实现后专项通过 |
| Ctrl+C永久不就绪 | `session-input-red.txt`四行真实失败（其余按键12行通过）；`session-input-green.txt`22/22含旧四行尾部保护 |
| task环境reset旧停止 | `task-reset-red.txt`四行额外写出03；修后`task-final-green.txt`17/17，`task-stable-1..10.txt`连续十轮通过 |
| task owner首次启用不可用 | `workflow-service-red-valid-temp.txt`四组canStart=false；Service重新启用重建索引，最终跨协议回归通过 |
| wait屏幕预算/冷页 | `wait-red-fixes-workspace-temp.txt`含屏幕revision丢失/冷页sourceStatus未明示；修后继续补充回归 |
| wait旧代际内容 | `wait-invalidated-red.txt`旧context非空；修后清空命中/上下文，保留旧游标和可得新earliest元数据 |
| wait断流配额 | `wait-discard-api-red.txt`缺API；新增discard与40轮无回调回收专项，Service两代20轮实际接线另验 |
| wait Busy续扫 | `wait-busy-red.txt`已消费resume后BUSY丢matcher；窄B1数据源端口确定性故障注入，修后保留位置/部分all事实及新resume |
| 非ok页有效前缀 | 独立审查F-001；`wait-prefix-red.txt`六行全失败，修后`wait-prefix-final-green.txt`完整34/34（1146ms）；pending/busy/unavailable前缀都消费一次 |

第一轮只读审查四项已修：任务owner生命周期、可信reset停止失效、无法交付续扫记录配额、失效响应Schema。完整独立审查新增F-001有效前缀漏扫，已修并静态复核关闭；最终报告`final-review.md`无尚未处理的重要发现。审查未自行运行测试，日志核对与执行成绩分开。

测试环境问题单列：默认QTemporaryDir在当前受限环境返回空路径，即使显式正常用户TEMP亦失败，未进入业务断言；原输出`wait-temp-diagnostic.txt`、`workflow-service-red.txt`保留。新夹具使用工作区唯一临时模板，最终CTest把TEMP/TMP指向工作区`build/b3-2026-10-26/temp`；生产私有ACL校验与有效断言保持。测试准备期偶发只读snapshot BUSY被误当token，`task-diagnostic-3.txt`定位后仅夹具有界重试该只读前置；未吞掉产品授权/输入错误。

## 6. 最终固定产物验证

Windows Qt6.8.3/MSVC14.51/Ninja，复用`build/Release`。沙箱默认构建的MSVC PDB管理器C1902记录于`full-build.txt`；同一源码未改编译选项，正常环境`full-build-outside.txt`退出0。全部默认目标构建`final-build.txt`退出0。首轮全套暴露历史契约的旧目录数量断言，保留原对象逐字段兼容检查，仅将13/旧九项+4改为17/旧九项+8；随后全部默认目标增量构建`acceptance-build.txt`退出0并固定产物运行相关复验。之后仅将最后几处新增英文注释改为中文，无产品逻辑/断言变化；全部默认目标末次增量构建`comment-final-build.txt`退出0，未为注释再次重跑功能测试。

当前实核默认注册50项，MCP标签19项；注册数不代表通过。

- **首轮完整CTest 46/50，382.60秒**，`final-ctest.txt`，退出8；失败为历史契约、MCP业务、Core、ConPTY。其余46项包括新增等待/任务、Session、TerminalSession、文件传输、录制/历史/资源均在本轮通过。不能把后续专项拼成一次48/50或全绿成绩。
- **最终相关CTest 20/21，128.44秒**，`acceptance-ctest.txt`，退出8；包含MCP19项中的18/19、Session和Core。唯一失败为MCP业务目标的环境夹具，详见下一条；历史契约更新后已通过，原逐对象兼容断言保持。
- MCP完整文件复核`mcp-detail.txt`为**150pass/1fail（90.630秒）**。唯一失败`unrelatedGroupOwnedDirectoryIsRejected`在调用生产`secureDirectory`之前，Windows `SetNamedSecurityInfoW(... Everyone owner ...)`返回`ERROR_INVALID_OWNER=1307`，未能建立外来属主夹具。属于当前受限环境的权限准备失败，未修改/跳过有效断言，不记为业务全绿；最终相关轮使用同一MCP可执行产物。
- Core首轮标准输出为空且失败；同一产物文件输出`core-detail.txt`为**100/100，7.582秒**，最终CTest Core又通过（8.06秒）。未改Core源码/断言，初轮失败未复现，原因未确定，不凭后续通过改写首轮成绩。
- ConPTY文件复核`conpty-detail.txt`为**10pass/2fail，47.779秒**，仍仅既有`injectedStartupStagesRollBack`末条句柄上限及`repeatedLifecycleReturnsResourcesToBaseline`严格句柄相等断言。重复生命周期句柄229/232、线程6/6、测试子进程0/0（final/baseline），线程/子进程明确通过。句柄瞬态读数亦可下降，不据此推断增长率；生产关闭路径及断言均未修改。

新增等待完整QTest34/34（`wait-prefix-final-green.txt`），任务17/17及连续十轮稳定（`task-final-green.txt`、`task-stable-*.txt`）；最终相关CTest再次覆盖两目标。没有为了制造全绿排除权限测试或ConPTY，也没有重复运行无关全套。

已实际完成的独立互操作：`interop.txt`，官方MCP SDK 2025及raw HTTP 2026，共17项输入/输出Schema检查；新等待/启动/原ID查询/请求停止与精确重试的实际响应均通过JSON Schema校验，structuredContent与content.text一致。仅使用内存SSH测试适配，不连接服务器。两代跨协议接线专项`workflow-service-green-3.txt`8/8（含setup/cleanup，10.069秒）；契约20/20、Wire31/31分别保存，不合并为全套成绩。

构建：`call C:\Programs\MicrosoftVisualStudio\18\Insiders\VC\Auxiliary\Build\vcvarsall.bat x64` 后 `cmake --build build/Release`。

测试PATH含`C:/Programs/Qt/6.8.3/msvc2022_64/bin`、QT_PLUGIN_PATH同前缀plugins；不全局设置offscreen。CTest：`ctest --test-dir build/Release -C Release --output-on-failure`。本轮同时命中MCP/能力注册/Session并新增构建注册，按AGENTS执行全套；非真实设备连接，不执行手工SSH/UART验收目标。

最终相关命令：`ctest --test-dir build/Release -C Release -R '^novaterm_(history_contracts|capabilities|capability_entry|mcp.*|sftp_session_safety|command_safety|resource_sharing|session|core)_tests$' --output-on-failure`。修改文档本地链接检查0缺失，`git diff --check`通过；AGENTS旧历史链接另按既有遗留记录处理。

## 7. 未验收与下一起点

B3/B3a源码与替身验证不替代B4：真实SSH/LinuxShell、UART设备CLI/U-Boot、同链路板卡重启、双通道配对、客户端实际交互、长时间任务、驱动丢帧与性能仍未验收。未运行macOS/Linux、消毒器、GPU专项或真机部署/烧录。任务终态十分钟保留由代码/原token期限及专项证明边界，未实际等待十分钟墙钟压力验收；等待TTL另有注入单调时钟的599999/600000ms确定性回归。

下一会话先读本记录及02，确认当前工作区；用户明确设备/操作范围后才安排B4验收。不自动实施B3b（受限单文件传输/保存目标连接），不恢复P4/P1原型，不自动提交/推送。若Core初轮瞬态再次出现，先获取具体断言，不能套用ConPTY或ACL结论；外来属主夹具在具有相应Windows能力的正常环境另验，保留原断言。
