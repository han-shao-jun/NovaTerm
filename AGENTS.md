# AGENTS.md

## 本文件的用途

本文件只保留 Agent 接手项目所需的概要、入口和长期工作约束。
**禁止向本文件或 README.md 追加开发日志、版本流水账、提交/打包记录、逐轮测试成绩、排错过程。**
更新已有条目，不按日期不断追加章节；不为每次任务另建过程文档。
接口、架构、配置或用法变化时，更新对应说明。只有用户明确要求时才另写开发记录。

## 接手任务的顺序

1. 先看用户当前要求和 `git status --short`、`git diff`，识别已有改动，不覆盖用户工作。
2. 按下表找到源码入口，只读相关架构说明及调用链；普通局部任务不用通读历史记录或扫描整仓。
3. 修复问题先用现有测试或最小复现确认失败，再验证修复能覆盖原触发条件；复用已有接口，避免扩大改动范围。
4. 核对现有构建目录的 CMake 配置，构建改动涉及的目标，再按测试表验证；文档修改不跑业务测试。
5. 收尾检查 `git diff --check` 和变更范围，简要说明改了什么、验证结果及限制。提交、打包、安装按用户要求执行。

## 项目概要与当前方向

NovaTerm 是 Qt 6 / C++17 跨平台终端，支持本地 Shell、SSH、Telnet、串口、
SFTP、串口文件传输、历史录制与 MCP 接入。终端解析使用 libvterm，
SSH 使用 libssh，渲染使用 QRhi，界面使用 ElaWidgetTools。

- 现有四类 Transport 共用会话和输入通路；每个 TerminalView 拥有自己的 TerminalSession。
- MCP 为外部 Agent 提供已连接终端和文件操作，不内置模型、聊天或开发流程编排。
- 内存历史、录制、离线查询已分为独立模块；整个终端 core 尚不能视为无 Qt 库。
- 后续工作以用户当前任务及阶段文档中的缺口为准。MCP B4 实机验收、跨平台能力和文件后端边界需单独核实。
- 不擅自恢复用户暂停的核心正则匹配器替换，也不重新引入已退役的 SessionManager、保存目标 MCP 工具或旧授权窗口。

## 阅读入口

| 入口 | 用途 |
| --- | --- |
| [README.md](README.md) | 项目特性、使用、构建和安装 |
| [统一架构](docs/ARCHITECTURE.md) | 分层、所有权、线程和数据通路 |
| [架构索引](docs/architecture/README.md) | 模块与阶段文档导航 |
| [路线图](docs/architecture/Development_Roadmap.md) | 阶段依赖、剩余工作和完成条件 |
| [Session / Transport](docs/architecture/stages/P6_Session_and_Transport.md) | 会话、连接与 SFTP 边界 |
| [MCP 现行契约](docs/architecture/stages/P8_AI_MCP_Interface/02_Current_MCP_Contract.md) | 工具参数、授权、预算及结果语义 |
| [MCP 缺口](docs/architecture/stages/P8_AI_MCP_Interface/03_Gaps_and_Evidence.md) | 尚未完成或验收的能力 |
| [串口文件传输](docs/architecture/stages/P9_File_Transfer_Protocols.md) | XMODEM / YMODEM / ZMODEM 与会话接线 |

源码与统一架构是判断依据；阶段状态以对应阶段文档为准，旧设计不能覆盖当前实现。

## 按任务找入口

| 任务 | 优先查看 |
| --- | --- |
| 乱码、换行、显示异常 | `src/core/terminal/VTAdapter.cpp`、`TerminalCore.cpp`；绘制问题再查 `src/renderer/` |
| 连接、重连、会话关闭 | `src/transport/`、`src/session/TerminalSession.cpp`、`SessionInputPump.cpp`；平台后端在 `src/platform/` |
| SFTP 文件管理与传输 | `src/session/SftpSession.cpp`、`src/ui/widgets/SftpPanel.cpp`；MCP 接入另查 `src/mcp/FileTransfers.cpp` |
| 串口文件传输 | `src/session/transfer/`、`src/filetransfer/` |
| 内存历史、录制、离线搜索 | `src/history/`、`src/recording/`、`src/historysearch/` 及各自 README |
| MCP 工具或授权 | 先读现行契约，再查 `src/mcp/McpService.cpp`、`src/mcp/` 和 `src/capabilities/` 对应模块 |
| 设置、配色、窗口交互 | `src/service/ConfigManager.cpp`、`TerminalSchemeStore.cpp`、`src/ui/`；主窗口在 `src/ui/app/MainWindow.cpp` |

## 架构约束

- 入站字节统一走 `ITransport::readyRead → SessionInputPump → TerminalCore::writeInput`，不得另建旁路。
- Parser 单写；Renderer / Search 读取稳定快照。跨线程队列必须有上限、背压与停止语义。
- libvterm 类型保持在 VTAdapter 实现边界；libtelnet 类型保持在 TelnetTransport 实现边界。
- 新增核心算法和数据结构不依赖 Qt；现有 QObject 门面按实际边界处理，不声称 core 已全部去 Qt。
- Transport 不解释终端 Cell；Renderer 不解释 ANSI、SSH 或配置格式。
- Session 不持有 QWidget、TerminalView 或 Renderer；SessionDirectory 是非 owning 的目录。
- UI Theme、Terminal Scheme、Font Config 分离。应用主题变化不得覆盖用户选择的终端配色。
- 异步回调必须检查对象存活、原连接身份和 generation；析构前解除可能回调已销毁状态的连接。
- 密码、token、私钥口令不写入日志和普通配置；持久配置仅保存 credentialRef。

## 编码与依赖

- 遵循现有 C++17 风格；成员使用 `_camelCase`，注释及 Doxygen 使用中文。
- 使用 RAII 管理资源；C 库实现细节通过 PImpl 隔离。不要为机械风格建议改写正确 API。
- 新线程入口调用 `NovaTerm::setCurrentThreadName()`；线程名使用不超过15字节的 ASCII，并与 QThread objectName 共用常量。
- UI 优先使用 Ela 控件；提示和确认走 `NovaTerm::Ui::confirm()` / `warn()`。
- 避免覆盖 Ela 控件自带 QSS 或运行期替换应用 style；需要库内绘制接口的适配组件放回 Ela 库内。
- `third_party/` 使用仓库内源码。ElaWidgetTools、libssh、libvterm 可做有依据的本地修正；libtelnet 与 LZ4 保持上游源码不改。
- 新增 Ela 源文件后显式重新配置 CMake；新增依赖须核对构建、部署和许可证材料。

## 构建与验证

完整环境及跨平台命令见 README。当前工作区已有 `build/Release`，继续使用已有生成器：

```bash
cmake --build build/Release --target NovaTerm -j 4
ctest --test-dir build/Release -N
ctest --test-dir build/Release -R '^目标名$' --output-on-failure
```

先构建受影响测试目标，再运行相关用例；不要把旧测试二进制的结果当成当前源码验证。
默认只跑相关测试。公共接口/数据布局、公共构建选项或依赖变化、大范围跨模块改动、
合并分支后才考虑全套；纯文档改动只检查内容、链接和 diff。

| 改动范围 | 主要测试入口 |
| --- | --- |
| 终端解析 / libvterm | `novaterm_core_tests`、`novaterm_terminal_ops_tests` |
| 历史 / 搜索 | `novaterm_scrollback_tests`；`history`、`recording`、`history-search` 标签 |
| 渲染 | `novaterm_renderer_tests`、`novaterm_renderer_p5_tests` |
| 会话 / SSH | `novaterm_session_tests`、`novaterm_ssh_transport_check` |
| SFTP / 本地文件访问 | `novaterm_sftp_session_safety_tests`、`novaterm_sftp_panel_tests`、`novaterm_mcp_local_file_io_tests` |
| MCP / 能力准入 | 按改动选择 `mcp`、`capabilities` 标签下的目标 |
| 串口文件传输 | `filetransfer`、`serial` 标签下的相关目标 |

目标和标签以当前 `tests/CMakeLists.txt` 及 `ctest -N` 为准，不在这里维护测试数量或成绩。
多配置构建须传实际构建配置，如 `-C Release`，不要固定为 Debug。
Windows 核对 Qt bin / 插件路径传递，必要时使用 PowerShell；QTest 断言可用 `-o file.txt,txt` 获取。
不要在 Windows 全局强制 offscreen，GPU 与平台插件按目标要求配置。
监听端口、PTY、密钥环等环境限制与产品失败分别判断；不凭旧记录忽略当前失败。

## MCP 与设备操作

- MCP 仅操作用户已连接的明确会话；本地源码编辑、构建和部署使用本地工具。
- 按实际工具 schema 提供必填中文操作、目标、内容；目标使用真实 IP / 串口号。
- 输入前观察终端；相同操作精确重试保留 operationId，结果未知时先查询，不重复启动。
- 统一令牌下仍须尊重用户撤权；不得自动启动 UART 接收程序或抢占人工传输。
- 区分命令输出、任务退出、协议完成、文件确认、独立哈希与真实发布；静默不代表成功。
- 不因构建或测试需要擅自重启用户实例、占用真实设备或执行破坏性操作。
- `B4_Test_Report.html` 只记录真实 MCP 工具使用；单测、模拟后端、构建、审查和消毒器结果不能充当 B4 实机证据。

## 文档与提交

- README 面向使用者：项目介绍、特性、用法、构建和打包安装；AGENTS 面向协作者：入口与长期约束。
- 架构、数据通路、所有权或目录变化更新 `docs/ARCHITECTURE.md`，接口约定写入头文件；构建、依赖和用法变化同步 README 及许可证材料。
- 阶段状态变化同步阶段文档、架构索引和路线图；只更新受影响内容，不为每次修改追加过程记录。
- 保留真实失败与未验证边界，不把编译成功、模拟测试或某个平台成绩外推成整体验收。
- 不修改与当前任务无关的用户文件，不把构建产物、日志、临时凭据纳入提交。
- 仅在用户明确要求时创建提交。按具体文件暂存，避免 `git add .`；提交前核对 `git diff --cached` 和 `git diff --cached --check`，排除无关改动及敏感信息。
- 审查实际 diff：重点看生命周期/悬空引用、线程与重入、异常安全、move/copy 和整数转换，以及高 DPI/平台差异、热路径开销与回归覆盖；不以审查为由改写无关代码。
- 每次创建 Git 提交，递增根 `CMakeLists.txt` 中版本号的 PATCH；未经规划不改变 MINOR。
- 提交消息使用中文，按 `fix:` / `feat:` / `docs:` / `test:` / `chore:` 开头。
- 按主题拆分；vendored 第三方改动单独提交。提交消息列明实际测试目标、未验证范围及重要偏差，不写“全绿”等超出证据的结论，不罗列开发过程。
