# 直接路径传输、一次确认记忆与历史访问精简

基线 `4a1333c`（0.2.105），2026-10-30 工作区增量。用户要求取消目录/文件预共享和同类前置授权，上传由工具要求外部 Agent 询问一次并回复确认，确认过的相同文件上传组合记忆；另明确取消历史逐条授权和独立文件授权 UI。未要求创建提交或推送，版本保持。

## 最终行为

- SSH 上传/下载直接传 `localPath/remotePath`；UART 上传直接传 `localPath`，`remoteName` 默认源名。沿用已连接会话的 `sessionId/epoch` 与精确重试 `operationId`，用户无需手工寻找授权资源 ID。
- `novaterm_list_file_resources/list_serial_file_targets` 退役，22→20工具；旧 resource/scope/target/revision 参数明确拒绝。独立 FileResourceSharingDialog 与 RemoteDirectoryDialog 源文件、入口及其退役 UI 测试删除。
- 首次上传返回 `confirmation_required` 和源路径、目标、覆盖意图、5分钟有效确认凭据。Agent只询问用户一次，接受后自动用原参数与 `confirmationToken/confirmed=true` 续调；false返回declined且不启动。没有新增本机弹窗；服务端不声称能验证Agent是否实际展示过问题。
- 本机私有 `upload-memory.json` 最多512条哈希记忆，绑定令牌身份、源路径、目标端点/位置、协议及覆盖意图，跨重启保留。按路径而非内容hash，同路径重新构建沿用；变目标/覆盖重新询问。最多64个待确认记录，绑定原运行期身份/连接/参数；旧凭据、改参、过期和断流拒绝。
- MCP设置只增加“清除上传确认记忆”；持久随机清除代次使其他实例的旧确认也失效。已建立的原传输精确重试直接返回原事实，不因清除记忆或确认到期再次发送。
- SFTP通过原SshPanelStack/SftpPanel owner按需准备连接并等待原作业空闲，复用原worker；普通目录错误不误报连接失败。UART在确认后自动释放同主体输入控制，包括已提交的接收准备命令；pending输入、其他主体和已有传输保持互斥，不自动rz/Ctrl+C。
- 历史资源服务按启用接入后的有效身份和实际资源generation即时授权全记录范围，无逐条share/revoke/persistent窗口或授权文件。保留手动录制、原游标/查询/证据预算和独立导入资源生命周期。

## 保留边界

固定句柄文件I/O、禁止链接/非普通文件、64MiB文件/64KiB块、默认不覆盖、临时文件原子发布、原身份和连接代际、取消及已发布事实保持。路径属于NovaTerm运行主机，绝对路径最多1024 UTF-8字节，文件名最多255字节。Linux Reader保留，Linux Writer与macOS后端未实现；SFTP可信目录写入者前提及同步库调用内部无法硬截止的边界保持。

本机Windows软件验证，不连接真实SSH/UART设备，不跑无关全套/Core/ConPTY、其他平台或打包。MainWindow全窗口异步准备没有独立整窗动态夹具；面板接线、目录错误、连接绑定、传输runtime和HTTP层分别验证。内部旧纯授权store/适配器仍供绑定设施和既有单测使用，不再作为公开传输门槛或用户入口。

## 分轮证据

所有日志位于本机 `build/`，不纳入提交；没有把不同批次拼作一次全套。

| 批次 | 实际结果与日志 |
| --- | --- |
| 直接路径接口 RED | 2pass/3fail，三工具均因Missing resourceId；`direct-transfers-contract-red.txt` |
| 盘根固定句柄 RED | 2pass/1fail，Reader拒绝root；`direct-path-root-red.txt` |
| 历史免共享 RED | 服务2pass/1fail（PERMISSION_DENIED），UI2pass/1fail（historyShare存在）；`direct-history-{service,ui}-red.txt` |
| 首次集成构建 | 失败于liveBinding为private；将现有连接快照查询公开后相关构建通过；`direct-transfers-green-build{,-2}.log` |
| 首轮专项 | CTest6/7；索引详细30pass/1fail为停用再启用新身份仍期待旧查询成功，保留发布取消断言并改为拒绝旧查询；`direct-transfers-targeted-tests.log`、`direct-file-index-diagnostic.txt` |
| 首轮完整MCP | 159/159，103.692秒；`direct-transfers-mcp-full.txt` |
| 独立行为审查修复 | 三个只读代理覆盖六领域，确认两项P2。跨实例清除RED2pass/2fail；SFTP错误回调RED2pass/1fail。分别修复清除generation及连接失败过滤；`upload-memory-clear-red.txt`、`sftp-preparation-error-red.txt` |
| Release默认构建 | 指定 `scripts/build-novaterm_com.bat` 成功；`direct-transfers-final-build.log` |
| 扩大专项初轮 | 26/28，失败为盘根仍列入非法根的旧断言和旧提示词片段；详细I/O37pass/1fail、Wire37pass/1fail。迁移夹具，保留其他非法路径和两代指令相等断言；`direct-transfers-final-related.log`、`direct-final-{file-io,wire}-diagnostic.txt` |
| 最终相关CTest | `ctest --test-dir build/Release -L 'mcp|recording' -E '^novaterm_mcp_tests$' --output-on-failure -j 2`，28/28，22.19秒；`direct-transfers-final-related-2.log` |
| 完整MCP独立复验 | 159/159，102.001秒；`direct-transfers-final-mcp.txt` |
| 面板独立复验 | CTest1/1，16.58秒；`direct-transfers-final-panel.log` |
| 两代互操作 | 初次不存在会话仍期待旧PERMISSION_DENIED失败；上传新入口明确SESSION_NOT_AVAILABLE，修正此预期后SDK2025/rawHTTP2026二十工具、10退役入口及工作流通过；`direct-transfers-final-interop{,-2}.log` |

末次补充可注入单调时钟，以定向测试覆盖5分钟到期、64项待确认额度回收、稳定令牌身份跨重启记忆；其构建/测试追加记录于下节，不把前述成绩改写为这一轮重跑。

## 末次确认边界复验

指定脚本Release默认构建成功（`direct-transfers-clock-tests-build.log`）。确认专项 **9/9，0.581秒**（`direct-transfers-confirmation-final.txt`），两代HTTP上传确认/记忆/真实清除按钮与覆盖幂等专项 **8/8，3.460秒**（`direct-transfers-http-final.txt`）。末次只增加默认仍使用QElapsedTimer的可注入时钟与边界测试，没有重跑上述完整159项或28个目标；各轮保持各自范围。`git diff --check`通过，CRLF转换提示不记作代码缺陷。

## 0.2.106 审查与提交收口

用户随后明确要求使用 `C:/Users/liurui/.cc-switch/skills/qt-cpp-review/SKILL.md` 审查，有问题修复，收口后创建提交并推送Gitee。本轮对全部未提交差异及新增文件运行候选扫描，并由六个独立只读代理分别复核模型契约、生命周期、线程、API/C++、错误验证、性能状态；未发现高可信新增缺陷。候选扫描提示均位于测试夹具，逐项结合断言/控制流判断，不作为缺陷计数。版本按提交约定0.2.105→0.2.106。

本次新版本验证，不套用上面的实施成绩：

- 指定 `scripts/build-novaterm_com.bat` 完成编译，但默认主程序链接因工作区运行中的 `NovaTerm.exe`（PID37376）被占用而LNK1104失败；原日志 `build/direct-02106-review-build.log` 保留。
- 未关闭用户实例。以原 `CMakeFiles/NovaTerm.rsp`、相同Release链接选项，仅替换exe/lib/pdb输出名，链接 `build/Release/bin/NovaTerm-review-0.2.106.exe` 成功；`build/direct-02106-isolated-link.log`。这证明新版本对象可链接，不宣称默认脚本全流程成功或已经替换运行中程序，也未运行此独立产物连接设备。
- MCP/录制相关CTest **28/28，22.55秒**（完整MCP另跑）：`build/direct-02106-commit-related.log`。
- 完整MCP业务 **159/159，102.587秒**：`build/direct-02106-commit-mcp.txt`。
- SFTP面板另轮CTest **1/1，16.65秒**：`build/direct-02106-commit-panel.log`。
- 官方SDK2025/rawHTTP2026 **20工具及10退役入口拒绝通过**：`build/direct-02106-commit-interop.log`。
- `git diff --check`通过；提交前fetch确认Gitee/master与原HEAD一致。构建产物/依赖/临时日志及本机 `scripts/build-novaterm_com.bat` 不纳入。

无本轮产品修复增量，不改变原功能语义；未连接真实设备、未跑无关全套/Core/ConPTY或其他平台，不打包。该提交统一包含直接路径、确认记忆、文件UI删除与历史授权精简，作为同一低门槛MCP使用流程变更。
