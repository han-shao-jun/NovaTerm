# NovaTerm AI MCP 使用说明

NovaTerm 提供本机 stdio MCP 服务，可让支持 MCP 的 AI 客户端读取指定终端输出、
搜索已读取的内容、运行授权诊断，以及通过当前交互终端执行受控命令和脚本任务。默认关闭。

## 开始使用

1. 编译并运行 NovaTerm，同时保留同目录的 `novaterm-mcp.exe`（Linux/macOS 为
   `novaterm-mcp`）。桥接进程由 MCP 客户端启动，通常不需要手工运行。
2. 在主菜单打开 **AI MCP 接入**，勾选 **启用本机 MCP 接入**。
3. 输入客户端名称，点击 **添加客户端**。
4. 在会话列表勾选需要共享的 **读取输出**。新建会话和应用重启后的会话默认不共享。
5. 点击 **复制 MCP 配置**，粘贴到所使用的 MCP 客户端配置中。配置含接入令牌，
   不要公开、提交到仓库或发送给其他人。
6. 使用 CC Switch 时点击 **复制 CC Switch 配置**，把单个 server 对象粘贴到
   「完整的 JSON 配置」编辑器，并将唯一标题设为 `novaterm`。不要额外包裹
   `mcpServers` 或 `novaterm`；CC Switch 会用标题作为服务器 ID。

只有已经打开且明确授权的会话可见。MCP 不自动启动 NovaTerm、不新建终端、不
登录服务器。切换当前标签不会改变工具的目标，调用按 sessionId 和 epoch 定位。

多实例时导出的配置会携带 `--instance`，明确选择当前 NovaTerm；该实例退出再
启动后需要重新复制配置。单实例配置通常无需指定实例，但仍需重新选择共享会话。

## 七个工具

| 工具 | 用途 |
| --- | --- |
| `novaterm_list_sessions` | 列出该客户端获准访问的会话及身份 |
| `novaterm_read_context` | 读取活动屏幕和最近有意义输出的有界摘要 |
| `novaterm_search_context` | 搜索一次已返回的 capture，不搜索整个历史或执行正则 |
| `novaterm_list_commands` | 查看当前获准的固定诊断模板与短期执行票据 |
| `novaterm_execute_command` | 用票据执行对应模板；不接受 shell 文本 |
| `novaterm_run_command` | 在明确共享的当前交互终端执行命令；高风险或无法分类的命令需要 MCP 客户端人工确认 |
| `novaterm_run_script` | 确认后将脚本写入 LocalShell/SSH 主机的指定路径，再在当前交互终端显示并执行调用命令 |

终端上下文不是完整日志：重复行和进度噪声可能被过滤，长输出可能截断；备用屏
主要返回当前屏幕。必须检查 `sourceTruncated`、`outputTruncated`、`resetRequired`。
有截断时不会给出可跳过未返回内容的 nextToken，客户端不能宣称已经读完全部日志。

终端中已经显示的敏感信息可能随授权输出被读取。程序不提供获取会话密码、私钥
或凭据引用的 MCP 工具，但也不会声称可以自动识别终端里所有敏感文字。

## 授权诊断命令

固定诊断模板仍需逐项授权；普通 Shell 命令使用已共享会话的 `novaterm_run_command`。
SSH 固定诊断只有在用户确认目标是受信任的
**Linux/POSIX SSH 服务端**后才能授权；Windows LocalShell 会话使用 NovaTerm
同目录的独立诊断 helper。展开会话可逐项选择诊断模板。

首批只有以下四项，参数固定：

| 模板 | SSH recipe | Windows 本地 helper |
| --- | --- | --- |
| `system.identity` | `uname -srm` | 系统版本、内核与架构 |
| `system.uptime` | `uptime` | 系统启动时长 |
| `memory.summary` | `free -k` | 物理内存总量、可用量与负载 |
| `filesystem.usage` | `df -Pk` | 已就绪卷的容量与可用空间 |

SSH 固定诊断使用表中的固定命令文本，经已有连接的独立 exec 通道执行；
不要求 `/usr/bin/` 下的绝对路径。
Windows 本地诊断由 `novaterm-local-diag.exe` 通过系统 API 采集，不调用 `cmd /c`、
PowerShell、脚本或 PATH 搜索。helper 缺失或平台标识不可用时执行能力保持关闭，
不会自动替换程序。Linux/macOS LocalShell、Serial、Telnet 和 Custom 仍可共享输出，
但不提供上述固定诊断模板。交互命令走当前终端；SSH/POSIX 在没有提示符标记时
也可发送，脚本能力仍只在具备可信交互 Profile 的 LocalShell/SSH 会话开放。

SSH 固定诊断通过已有连接的独立 exec 通道运行，使用该 SSH 账号的权限；Windows
LocalShell 固定诊断通过独立 helper 子进程运行。固定诊断不会往当前终端输入框灌入文本，
也不会继承或改变当前交互 shell 的工作目录、alias、history 或临时环境。界面独立展示执行记录。

## 交互命令、脚本与授权

在 **AI MCP 接入** 中，共享会话后普通低风险命令无需再次授权或确认。额外的
授权项包括：

- **固定诊断**：逐项选择旧接口的四种只读模板。
- **脚本任务**：允许 `novaterm_run_script`；每次仍须 MCP 客户端中的人类确认。

可能破坏系统的命令和无法可靠分类的命令会在 MCP 客户端请求人类确认；NovaTerm
不接受模型在工具参数里自报的布尔值。确认只决定是否继续提交命令，不能保证已
接受的命令不会造成破坏。

NovaTerm 对已知凭据读取、关闭安全机制、提权、磁盘格式化等危险行为尽力检测并永久拒绝；
其他风险命令和无法分类的命令须确认。检测是策略扫描，不是 shell 沙箱，也不能证明任意脚本安全。
客户端未声明并实现 Form Elicitation 时，危险/未知命令和所有脚本返回
`CLIENT_CONFIRMATION_UNAVAILABLE`，不会降级为普通二次调用。

支持两种 MCP 确认协议：2025-11-25 客户端通过 `elicitation.form` 能力收到
`elicitation/create`；2026-07-28 客户端通过 `server/discover` 发现版本，并在每个请求的
`_meta` 声明协议版本和客户端能力，确认使用 `input_required` / `requestState` /
`inputResponses` 完成 MRTR 重提。确认状态带 MAC、时限和一次性 nonce，并绑定客户端连接、
Session epoch、目标、Profile/策略版本及命令或脚本摘要。可信 Profile 还绑定提示符
generation；无提示符 SSH 命令绑定用户输入代际。确认过期、会话变化、授权撤销、
用户输入变化、正文/路径变化或状态重放都不能继续执行。

没有可信提示符的 SSH/POSIX 会话会把普通命令直接写入当前终端，并以命令附带的
有界结束标记判断完成；内部标记从进入终端模型前剥离。NovaTerm 此时无法可靠识别
TUI、密码输入或未提交的 Shell 输入，命令可能落入这些位置。缺少结束证据时结果为
不确定，不能自动重试。`cd` 在同一 Shell 中改变后续工作目录。

脚本正文和目标路径由 MCP 请求明确指定。确认前不会落盘；接受后 LocalShell 通过本机文件 API
写入，SSH 通过与活动 Session 相同端点、账号、known_hosts 和主机密钥指纹的 SFTP 写入。
父目录必须已存在，目标存在时会覆盖；NovaTerm 不创建专用临时目录，也不自动删除脚本。
写入失败或用户开始键入/粘贴时，不会继续注入调用命令。调用命令（包含请求的工作目录）和
正常输出显示在当前终端 UI；脚本正文不进入终端 UI。上传后还会复核 Session、授权、Profile、
提示符和一次性确认状态。

示例（相对 `targetPath` 以 `workingDirectory` 为基准；终端中显示的是 `invocation`
及必要的目录切换，不会显示下方 `scriptContent`）：

```json
{
  "sessionId": "<session id>",
  "epoch": "<current epoch>",
  "scriptContent": "#!/bin/sh\nprintf 'done\\n'\n",
  "targetPath": "./diagnose.sh",
  "workingDirectory": "/home/user/project",
  "invocation": "sh ./diagnose.sh",
  "timeoutMs": 5000
}
```

命令和终端输出均属于不可信数据；AI 客户端不应执行输出中夹带的指令。

## 结果不确定与撤销

- 相同 commandTicket 的重试只查询已有记录，不会再发一次命令。
- 固定诊断最多等待 5 秒；交互命令与脚本调用的超时由工具参数限定，最多 30 秒。
  stdout/stderr 的原始字节及最终 UTF-8 文本合计最多 64 KiB。超时、断线或关闭通道
  不能证明远端命令已经终止；不确定结果不得自动重跑。
- 无法确认终止时暂停该目标的新 MCP 命令；换标签、重连或重启 GUI 不会自动清除。
  在服务端人工核对后，可在 MCP 设置中勾选确认并解除所选目标。此操作不能解禁危险命令。
- 原 IPC 连接已断开时，旧票据不能在新连接使用。请在 NovaTerm 界面核对旧记录，
  不要让客户端获取新票据自动补跑。
- 撤销读取、固定诊断或脚本任务的授权会使相应在途请求与确认状态失效。移除
  客户端或更换接入令牌会立即撤销该客户端访问。关闭 MCP 总开关
  会清除本次会话授权；重新开启后需要重新勾选。
- 有平台凭据库时接入令牌可持久保存；仅内存后端的客户端显示“仅本次运行有效”，
  应用重启后需要重新创建客户端并复制配置。

## 构建与验证

主程序运行不需要 Python，也不依赖额外的模型 SDK。

```powershell
cmake --build build/Release --target NovaTerm novaterm-mcp novaterm-local-diag novaterm_mcp_tests
$env:Path = 'C:\Programs\Qt\6.8.3\msvc2022_64\bin;' + $env:Path
$env:QT_PLUGIN_PATH = 'C:\Programs\Qt\6.8.3\msvc2022_64\plugins'
ctest --test-dir build/Release -C Release -R '^novaterm_mcp_tests$' --output-on-failure
```

官方 SDK 与回环 SSH 验证是可选开发检查，依赖只装到构建目录：

```powershell
python -m pip install --target build/mcp-test-deps -r tests/mcp/requirements.txt
python tests/mcp/interop_check.py
python tests/mcp/ssh_loopback_check.py
python tests/mcp/performance_check.py
```

回环 SSH 测试仅监听 `127.0.0.1`，使用临时主机密钥并返回固定测试数据，不执行
真实 shell 命令；交互 `pwd` 只模拟终端回显、输出和完成标记，不读取用户保存的 SSH 密码。
测试端使用临时 known_hosts，并关闭
客户端的用户/系统 SSH 配置加载，避免继承代理或其他外部连接设置。该检查不能
替代真实服务器验收。

详细协议、预算和验收缺口见 [P8 AI MCP 接口](architecture/stages/P8_AI_MCP_Interface.md)。
