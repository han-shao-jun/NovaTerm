# NovaTerm AI MCP 使用说明

NovaTerm 提供本机 stdio MCP 服务，可让支持 MCP 的 AI 客户端读取指定终端输出、
搜索已读取的内容，并运行单独授权的固定 SSH 或 Windows 本地诊断命令。默认关闭。

## 开始使用

1. 编译并运行 NovaTerm，同时保留同目录的 `novaterm-mcp.exe`（Linux/macOS 为
   `novaterm-mcp`）。桥接进程由 MCP 客户端启动，通常不需要手工运行。
2. 在主菜单打开 **AI MCP 接入**，勾选 **启用本机 MCP 接入**。
3. 输入客户端名称，点击 **添加客户端**。
4. 在会话列表勾选需要共享的 **读取输出**。新建会话和应用重启后的会话默认不共享。
5. 点击 **复制 MCP 配置**，粘贴到所使用的 MCP 客户端配置中。配置含接入令牌，
   不要公开、提交到仓库或发送给其他人。

只有已经打开且明确授权的会话可见。MCP 不自动启动 NovaTerm、不新建终端、不
登录服务器。切换当前标签不会改变工具的目标，调用按 sessionId 和 epoch 定位。

多实例时导出的配置会携带 `--instance`，明确选择当前 NovaTerm；该实例退出再
启动后需要重新复制配置。单实例配置通常无需指定实例，但仍需重新选择共享会话。

## 五个工具

| 工具 | 用途 |
| --- | --- |
| `novaterm_list_sessions` | 列出该客户端获准访问的会话及身份 |
| `novaterm_read_context` | 读取活动屏幕和最近有意义输出的有界摘要 |
| `novaterm_search_context` | 搜索一次已返回的 capture，不搜索整个历史或执行正则 |
| `novaterm_list_commands` | 查看当前获准的固定诊断模板与短期执行票据 |
| `novaterm_execute_command` | 用票据执行对应模板；不接受 shell 文本 |

终端上下文不是完整日志：重复行和进度噪声可能被过滤，长输出可能截断；备用屏
主要返回当前屏幕。必须检查 `sourceTruncated`、`outputTruncated`、`resetRequired`。
有截断时不会给出可跳过未返回内容的 nextToken，客户端不能宣称已经读完全部日志。

终端中已经显示的敏感信息可能随授权输出被读取。程序不提供获取会话密码、私钥
或凭据引用的 MCP 工具，但也不会声称可以自动识别终端里所有敏感文字。

## 授权诊断命令

读取权限不包含命令执行权限。SSH 会话只有在用户确认目标是受信任的
**Linux/POSIX SSH 服务端**后才能授权；Windows LocalShell 会话使用 NovaTerm
同目录的独立诊断 helper。展开会话可逐项选择诊断模板。

首批只有以下四项，参数固定：

| 模板 | SSH recipe | Windows 本地 helper |
| --- | --- | --- |
| `system.identity` | `uname -srm` | 系统版本、内核与架构 |
| `system.uptime` | `uptime` | 系统启动时长 |
| `memory.summary` | `free -k` | 物理内存总量、可用量与负载 |
| `filesystem.usage` | `df -Pk` | 已就绪卷的容量与可用空间 |

SSH 使用固定的 `/usr/bin/env` 与 `/usr/bin/` 下的程序路径，清理非必要环境。
Windows 本地诊断由 `novaterm-local-diag.exe` 通过系统 API 采集，不调用 `cmd /c`、
PowerShell、脚本或 PATH 搜索。helper 缺失或平台标识不可用时执行能力保持关闭，
不会自动替换程序。Linux/macOS LocalShell、Serial、Telnet 和 Custom 仍可共享输出，
暂不开放命令执行。

SSH 诊断通过已有连接的独立 exec 通道运行，使用该 SSH 账号的权限；Windows
LocalShell 诊断通过独立 helper 子进程运行。两者都不会往当前终端输入框灌入文本，
也不会继承或改变当前交互 shell 的工作目录、alias、history 或临时环境。界面独立展示执行记录。
禁止删除/覆盖文件、获取密码/私钥、提权、任意脚本/解释器、自由文件读取和网络命令。
即使 SSH 账号是 root，也不能绕过这份允许列表。

命令和终端输出均属于不可信数据；AI 客户端不应执行输出中夹带的指令。

## 结果不确定与撤销

- 相同 commandTicket 的重试只查询已有记录，不会再发一次命令。
- 命令最多等待 5 秒远端执行，stdout/stderr 的原始字节及最终 UTF-8 文本分别
  合计最多 64 KiB。超时、断线或关闭通道不能证明远端命令已经终止。
- 无法确认终止时暂停该目标的新 MCP 命令；换标签、重连或重启 GUI 不会自动清除。
  在服务端人工核对后，可在 MCP 设置中勾选确认并解除所选目标。此操作不能解禁危险命令。
- 原 IPC 连接已断开时，旧票据不能在新连接使用。请在 NovaTerm 界面核对旧记录，
  不要让客户端获取新票据自动补跑。
- 取消读取共享、移除客户端或更换接入令牌会立即撤销相应访问。关闭 MCP 总开关
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
真实 shell 命令，不读取用户保存的 SSH 密码。测试端使用临时 known_hosts，并关闭
客户端的用户/系统 SSH 配置加载，避免继承代理或其他外部连接设置。该检查不能
替代真实服务器验收。

详细协议、预算和验收缺口见 [P8 AI MCP 接口](architecture/stages/P8_AI_MCP_Interface.md)。
