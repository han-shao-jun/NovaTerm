# B3/B3a 审查修复（2026-10-27，Windows）

## 1. 范围与取舍

用户要求保留无实际收益的机械风格写法，仅处理其它问题。本轮修复独立审查确认的D-001/D-002，不批量修改136项lint提示，不提交/推送、不连接真实设备、不实施B3b。前轮实现/全套和环境失败见 [B3实施记录](B3_Implementation_2026-10-27.md)，不由本轮专项改写历史成绩。

调查项单列：I-001的日志源仍为committed_history，已在02及使用说明明确“基线前仍在活动屏幕的旧文字稍后滚入历史仍可命中”，不能作出设备输出时间边界的保证；按轮次判断需要本轮标志和固件身份。I-002尚未找到当前生产Completion同步销毁owner的可达链路，没有加入猜测式生命周期重构。

## 2. D-001：新任务首次受理不能附着普通输入Replay

普通interact输入operationId=A后再次观察，取得当前inputToken；旧实现允许start_task以A/同动作借ObservationState的Replay分支返回新的taskId。Controller当前operationId仍为A，旧bind直接标记accepted；若原命令已完成，还会将旧execution/exitCode当作新任务的完成事实，而baseline是在旧执行后新建的。

修复在既有InputCapabilities受理点增加内部allowReplay策略和Submit事实通知：普通交互缺省仍允许精确Replay，新任务首次受理明确拒绝Replay并返回INPUT_SEQUENCE_CONFLICT。只有本次实际Submit、inputAccepted之后才通知TerminalTasks关联，通知在dispatcher最终交付复核之前，交付失败不等同于从未受理。删除任务完成回调和call返回后的盲目bind回退。已有task记录的精确重试仍优先返回原记录。

TerminalInteraction仅透传内部策略/通知，不增加公开wire参数、第二执行器或新的输入状态真源；回调后的存活由QPointer复核。原validateNewInput保留，继续拒绝过期/旧代际/旧序号的新受理。

新增两行确定性回归：raw输入与已完成命令（原exitCode=7）。均在普通输入后取全新inputToken，同operation/actions启动任务须冲突、零重复输入；同时保留普通交互使用旧token的精确重试，并验证真正新operation可正常创建任务。

原始证据 `build/b3-fixes-2026-10-27/task-red.txt`：**2pass/2fail**，已完成命令行真实错误返回completed和旧exitCode=7；实现后完整任务QTest **19/19**（`task-green.txt`，1488ms）。末次相关CTest另覆盖最终增加的新operation受理断言。

## 3. D-002：等待游标按实际访问数轮转

原poll每轮计划count=min(4,N)，但可被8ms墙钟预算提前截断；offset仍增加计划数。稳定8条等待、慢单步时形成0→4→0，其余六条无法续扫/交付自身期限。

修复按实际visited推进，非空本轮至少访问一条；每轮最多四条、8ms检查、原QPointer及GUI调度保持，不增加并发/缓存预算。

新回归使用四客户端各两条1s/30s混合等待，先经真实B1适配器建立八份基线，随后在既有LogReader调用前构造9ms慢依赖。固定假单调时钟使队列稳定；16次实际QTimer读取须覆盖八条，再核对短等待timeout/日志位置/resume、长等待后续实际PASS命中和零输入。sleep用于构造耗时依赖，不用于等竞争或放宽时序断言。

原始证据 `wait-red.txt`：真实只访问**2/8**，断言失败；修复后完整等待QTest **35/35**（`wait-green.txt`，3094ms），CTest专项 **1/1**（`wait-ctest.txt`，2.88秒）。详细命令见同目录`wait-report.md`。

## 4. 独立复核与实际验证

独立只读复核已关闭D-001/D-002，未发现>80置信度的新阻断项。复核核对Submit/Replay权威点、回调在最终交付复核前发生、普通交互及既有task重试、授权/回调存活及8条慢等待公平性；复核者未重新运行测试。

Windows Release默认全部目标构建成功：`build/b3-fixes-2026-10-27/full-build.txt`，退出0。无Qt版本/编译选项、CMake或第三方依赖改变。本轮只命中MCP/能力输入边界，Session通过原端口回归；按AGENTS选择MCP全部标签及Session，不重复无关全套/ConPTY/Core。

固定产物相关CTest **19/20，122.32秒**（`ctest.txt`），包含MCP标签18/19和Session通过；等待/任务目标分别2.79/1.47秒。唯一失败为`novaterm_mcp_tests`，文件复核`mcp-detail.txt`为 **150pass/1fail，89.642秒**，仍仅`unrelatedGroupOwnedDirectoryIsRejected`在夹具创建时设置Everyone owner返回Windows `ERROR_INVALID_OWNER=1307`，未进入生产secureDirectory断言。没有修改/跳过原有效断言，也不将结果记为全绿。

环境PATH为Qt6.8.3 bin、QT_PLUGIN_PATH同前缀plugins；TEMP/TMP为工作区测试目录，生产私有ACL检查不变，不全局设置offscreen。命令：`ctest --test-dir build/Release -C Release -R '^novaterm_(history_contracts|capabilities|capability_entry|mcp.*|sftp_session_safety|command_safety|resource_sharing|session)_tests$' --output-on-failure`。本轮未重跑无关Core/ConPTY或全套，不把较早全套成绩代入本次修复。

两代互操作 `interop-replay.txt` 已通过：官方SDK2025/rawHTTP2026，共17项Schema；新增普通交互→新token→同operation启动task冲突、普通旧token精确重试及真正新任务/查询/请求停止/精确重试。仅内存SSH夹具，不连接服务器。

固定产物测试开始后仅更新文档和Python互操作断言；C++产品逻辑及QTest可执行文件不再修改。原始RED与最终GREEN/CTest分别保存，不拼接成绩。

新增/修改文档链接检查0缺失，`git diff --check`通过。I-001仅澄清既有边界，无新增工具或数据源；I-002无生产可达证据而不改。没有批量修lint风格、升级编译选项或调整第三方。

## 5. 保留边界

未实施B3b，未做真实SSH/UART、同链路固件重启、跨平台、消毒器、性能/长稳验收。前轮ConPTY句柄失败、Windows外来属主夹具1307和Core首轮未复现瞬态保留原记录，不修改断言规避环境限制。

下一会话先读本记录及02契约，确认未提交工作区。B4需要用户明确目标设备和允许操作后再验收，不自动连接或自动提交/推送。

## 0.2.86 提交前收口

2026-10-27用户明确要求创建本地Git提交，按AGENTS当前0.2.x约定将根版本0.2.85→0.2.86，不采用commit_check.md旧的0.1.<提交数>写法。本次包含B3/B3a基础恢复/等待/任务、两项审查修复及其测试/契约/记录；另外补齐docs/architecture/README.md中过期的P8状态表。独立pelican-bicycle.html、本地测试产物和本机辅助配置不纳入，不推送。

同版本固定源码的默认目标完整Release构建退出0，原始`build/b3-commit-0.2.86/build-outside.txt`。此前沙箱`build.txt`被MSVC PDB管理器C1902阻断，未修改编译选项或源码规避，正常环境同源码重建成功。

提交前验证采用两批，MCP业务完整QTest单独文件输出用于读取Windows断言，其余相关目标交给CTest；没有删除/跳过业务目标中的任何有效断言：

- CTest **19/19，32.00秒**：MCP标签其余18目标与novaterm_session_tests。命令为`ctest --test-dir build/Release -C Release -R '^novaterm_(history_contracts|capabilities|capability_entry|mcp.*|sftp_session_safety|command_safety|resource_sharing|session)_tests$' -E '^novaterm_mcp_tests$' --output-on-failure`；日志`ctest.txt`。
- `novaterm_mcp_tests.exe -o build/b3-commit-0.2.86/mcp-detail.txt,txt`：**150pass/1fail，89.723秒**。唯一失败仍在unrelatedGroupOwnedDirectoryIsRejected创建外来属主夹具时SetNamedSecurityInfoW返回ERROR_INVALID_OWNER=1307，未到生产secureDirectory断言。日志`mcp-detail.txt`；不将该目标或本次两批合计写为全绿。
- 官方SDK2025/rawHTTP2026 **17工具Schema及等待/启动/普通输入Replay拒绝/查询/请求停止/精确重试通过**，日志`interop.txt`。

上述文件均在`build/b3-commit-0.2.86/`。本轮未重跑无关全套、ConPTY/Core、真实设备、其他平台、消毒器或性能。较早全套/Core瞬态与ConPTY成绩按B3实施记录保留，不代入0.2.86通过数。
