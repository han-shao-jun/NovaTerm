# NovaTerm

NovaTerm（原名 WindTermQt）是一款基于 Qt 6 的跨平台终端模拟器和 SSH 客户端。
终端解析使用 libvterm，渲染使用 Qt QRhi，界面使用 ElaWidgetTools，采用
FluentUI 风格。

## 主要特性

- **跨平台支持**：面向 Windows、Linux 和 macOS；各平台的验证范围和待验收项见阶段文档
- **多协议支持**：支持本地Shell、SSH（libssh）、Telnet（libtelnet，含 RFC 1143 选项协商）和串口连接
- **现代界面设计**：采用FluentUI设计语言，提供深色/浅色主题切换
- **GPU加速渲染**：基于Qt QRhi实现高效GPU渲染，支持Vulkan、OpenGL、D3D等多种图形API
- **SFTP文件传输**：内置SFTP客户端，支持文件上传下载和目录操作
- **XMODEM / YMODEM / ZMODEM 文件传输**：纯 C++17 协议库已实现双向传输，
  并已接入串口 Session 与手动发送／接收窗口；Linux 独立协议测试及 lrzsz
  互通已验证。真实 UART 与跨平台验收仍待补齐，Windows 串口文件传输测试有
  已知失败，见 [AGENTS.md](AGENTS.md)。独立协议阶段的记录见
  [P9 文档](docs/architecture/stages/P9_File_Transfer_Protocols.md)，
  其中串口接线状态尚未同步当前源码
- **系统监控**：支持远程SSH会话的系统资源监控
- **AI MCP 接入**：默认关闭的本机 stdio 服务，提供七个工具，支持按会话授权读取输出、搜索已捕获内容、固定诊断、交互命令及 LocalShell/SSH 脚本任务；确认与权限边界见 [使用说明](docs/MCP_Usage.md)
- **终端解析**：使用 libvterm 处理 ANSI/VT 控制序列，具体语义由终端测试覆盖
- **搜索与高亮**：支持终端内容搜索和可配置文本高亮

## 快速开始

### 环境要求

- Qt 6.8 或更高版本（`find_package(Qt6 6.8 REQUIRED)`）
- CMake 3.20 或更高版本
- 支持 C++17 的编译器（`CMAKE_CXX_STANDARD 17`）
- Qt 组件：Widgets、Svg、ShaderTools、LinguistTools、SerialPort、Network；
  默认启用测试时还需要 Test
- 对于Windows：需要Windows 10 1809或更高版本，以及 MSVC 2022
- Linux/macOS 需要系统 OpenSSL 开发库；Windows 使用下方脚本预编译
  vendored OpenSSL
- 对于Linux/BSD：需要 Qt 的 **DBus** 组件（凭据存入 freedesktop Secret
  Service，即 gnome-keyring / KWallet；`find_package` 里按平台条件加入），
  运行时没有密钥环也能启动，但保存的 SSH 密码活不过进程重启

### Linux / macOS 编译

以下命令均从仓库根目录运行。以单配置 Ninja 生成器为例（需要 Ninja）：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Qt SDK 默认前缀按宿主平台预置：Windows 为
`C:/Programs/Qt/6.8.3/msvc2022_64`，Linux 为
`/home/super/Qt/6.8.3/gcc_64/`，macOS 为 `/opt/Qt/6.8.3/macos`。
首次配置时可追加 `-DCMAKE_PREFIX_PATH="/path/to/qt"` 指定本机安装位置。
已有构建目录应继续使用原生成器；切换生成器时使用新目录。

### Windows 编译

Windows平台使用ConPTY实现本地终端，需要Windows 10 1809+版本支持。

首次构建前需先预编译 OpenSSL（一次性，幂等）：

```bat
scripts\build-OpenSSL-thirdparty.bat
```

该脚本要求 PATH 中有 `perl.exe` 与 `nasm.exe`，产物落在
`third_party/openssl-3.5.7/install/`。libssh、libvterm、libtelnet 与
ElaWidgetTools 都随项目从源码构建，无需额外准备。

在 **x64 Native Tools Command Prompt for VS 2022** 中，从仓库根目录运行：

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Ninja 构建需要 MSVC 环境中的 `cl.exe` 和 Ninja 均在 PATH 上。也可使用
Visual Studio 多配置生成器，此时构建须选择配置：

```bat
cmake -S . -B build-vs -G "Visual Studio 17 2022" -A x64
cmake --build build-vs --config Release
```

`scripts/build-novaterm.bat` 写死了本机 Visual Studio 与构建目录路径，
不适合作为通用构建入口，也不执行首次 CMake 配置或 OpenSSL 预编译。
第三方源码均随仓库保存，无需 vcpkg、conan 或 FetchContent。

## 项目架构

项目采用分层架构设计，权威说明见 [统一架构总览](docs/ARCHITECTURE.md)。
所有 Transport 的入站字节共用
`ITransport::readyRead → SessionInputPump → TerminalCore::writeInput` 通路；
Parser 单写，Renderer / Search 读取快照，跨线程队列有界。
核心组件包括：

### 核心层 (`src/core/`)

- `TerminalCore`：当前的 QObject 门面，管理解析线程、终端状态与快照发布
- `ScreenBuffer`：屏幕缓冲区管理
- `ScrollbackBuffer`：回滚缓冲区，支持分块存储和快照
- `VTAdapter`：VT序列适配器
- `SearchEngine`：异步搜索引擎

核心去 Qt 化仍在进行：`TerminalCore`、`SearchEngine`、`ReflowEngine`
尚依赖 Qt，`novaterm_core` 仍链接 Qt Core/Gui；不能将其当作无 Qt 库使用。

### 传输层 (`src/transport/`)

- `LocalShellTransport`：本地Shell传输
- `SshTransport`：SSH连接传输
- `SerialTransport`：串口传输
- `TelnetTransport`：Telnet传输

### 会话层 (`src/session/`)

- `TerminalSession`：终端会话管理
- `SessionDirectory`：MCP 使用的不持有所有权的会话目录，生命周期仍由各 TerminalView 管理
- `SessionInputPump`：输入泵，处理数据流和背压
- `SerialFileTransferController`：串口文件传输控制器，经 Session 的输入分流与出站独占机制接入协议库

### 协议与 MCP (`src/filetransfer/`、`src/mcp/`)

- `src/filetransfer/`：不依赖 Qt 的 XMODEM/YMODEM/ZMODEM 协议引擎，可独立构建
- `src/mcp/`：本机 MCP 协议与授权；`tools/novaterm-mcp/` 提供 stdio 入口

### 渲染层 (`src/renderer/`)

- `TerminalRenderer`：终端渲染器
- `RenderScheduler`：渲染调度器
- `GlyphAtlas`：字形图集管理
- `FontManager`：字体管理

### UI层 (`src/ui/`)

- `TerminalView`：终端视图组件
- `MainWindow`：主窗口
- `SessionPage`：会话配置页面
- `SettingsPage`：设置页面
- `SftpPanel`：SFTP面板
- `SystemMonitorPanel`：系统监控面板

### UI 控件约定（约束）

**改动 `src/ui/` 界面、新增或替换控件时，优先使用
`third_party/ElaWidgetTools/` 提供的 Ela 控件**（`ElaPushButton`、`ElaLineEdit`、
`ElaComboBox`、`ElaScrollArea`、`ElaScrollBar`、`ElaDialog`/`ElaContentDialog`、
`ElaMessageBar`、`ElaTreeWidget`、`ElaText` 等），以保持 FluentUI 外观与主题
（含深色模式）一致。只有 Ela 没有对应实现、或语义明显不匹配时才退回原生 Qt
控件，且配色一律跟随 `QPalette` / 主题，不要硬编码颜色。

配套注意（细节见 `AGENTS.md` 的「容易写错的地方」与「第三方依赖约定」）：

- 不要给 Ela 控件再 `setStyleSheet()`，也不要改它的 `objectName` —— 两者都会
  顶掉控件自带的 QSS；行高用 `setItemHeight()`、去边框用
  `setIsFrameVisible(false)` 这类专用接口表达。
- `ElaScrollArea` 构造时会把两个方向设为 `ScrollBarAlwaysOff`。需要可见滚动条
  时必须在构造后显式设置 `setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded)`；
  `SystemInformationDialog` 是该用法的回归范例。
- 提示 / 确认框不要直接用 `QMessageBox` 或裸调 `ElaMessageBar` 静态方法，走
  `NovaTerm::Ui::confirm()` / `warn()`（`src/ui/widgets/MessagePrompts.h`）：
  它们已经正确处理了 `parent`（Ela 浮层对模态对话框需要传窗口一级，否则会落到
  对话框背后）。
- 为补齐能力而新增的 Ela 适配组件必须放在 `third_party/ElaWidgetTools/` 库内：
  绘制依赖的 `DeveloperComponents/Ela*Style.h` 没有 `ELA_EXPORT`，放在 `src/`
  里会直接链接失败。往该目录加文件后需显式重跑 `cmake -S . -B build`
  （Ela 子项目的 `FILE(GLOB ...)` 没有 `CONFIGURE_DEPENDS`）。

## 文档资源

统一架构总览位于 `docs/ARCHITECTURE.md`，配套细节文档见 `docs/architecture/`（索引：[docs/architecture/README.md](docs/architecture/README.md)）：

- [架构概述](docs/ARCHITECTURE.md)
- [配置、Profile 与主题](docs/architecture/Configuration_Profile_Theme.md)
- [渲染架构](docs/architecture/Rendering_Architecture.md)
- [阶段路线图](docs/architecture/Development_Roadmap.md)
- [各阶段实现文档](docs/architecture/stages/)
- [AI MCP 使用说明](docs/MCP_Usage.md)
- [开发与测试注意事项](AGENTS.md)

配置、Profile 与主题文档包含目标设计，当前名称与实现需结合其对照表和源码
阅读；文档冲突的处理规则见 [架构文档索引](docs/architecture/README.md)。

## 目录结构

```
NovaTerm/
├── src/
│   ├── core/           # 终端核心功能
│   ├── transport/      # 传输层实现
│   ├── session/        # 会话管理
│   ├── renderer/       # 渲染引擎
│   ├── ui/             # 用户界面
│   ├── service/        # 公共服务
│   ├── credential/     # 凭据管理
│   ├── profile/        # 配置管理
│   ├── mcp/            # AI MCP 协议与授权
│   ├── filetransfer/   # XMODEM/YMODEM/ZMODEM 协议库（不依赖 Qt）
│   └── platform/       # 平台特定代码
├── tools/              # novaterm-mcp（stdio 入口）、novaterm-local-diag
├── scripts/            # OpenSSL 预编译、MSVC 依赖兼容层等脚本
├── tests/              # 测试代码
├── docs/               # 文档
├── resources/          # 资源文件
└── third_party/        # 第三方依赖
```

## 测试

SSH、Core 吞吐、异步 glyph 和 Agent 文本状态的历史验证方法及实测数据见
[性能优化记录](docs/architecture/Performance_Optimization_2026-09-10.md)。
Linux 可用 `novaterm_ssh_transport_check --local-ssh-check` 显式启动隔离本机
SSH 验收（需要 sshd、ssh-keygen、ninja 和 c++；不修改用户 SSH 信任文件）。

项目包含单元测试、集成测试和性能护栏，部分 UI 与真实设备路径仍需人工验收。
`BUILD_TESTING` 与 `NOVATERM_BUILD_BENCHMARKS` 默认均为 ON。
默认注册到 CTest 的测试按平台不同：
**Windows 23 项、Linux 21 项**（默认启用 benchmark 构建；注册数以 `ctest -N`
为准）。`novaterm_conpty_tests`、`novaterm_terminal_session_tests` 仅 Windows，
`novaterm_pty_tests` 仅 Linux。

**日常只运行与改动相关的测试。** 测试注册数不代表已全部通过；仅在公共接口／
数据布局变更、涉及三个以上模块、公共编译选项／Qt／第三方依赖变更或合并分支
后运行全套。各模块覆盖关系与已知失败详见 [AGENTS.md](AGENTS.md)。

```bash
# 核对当前构建目录的注册项（不执行测试）
ctest --test-dir build -N

# 单配置 Ninja：运行相关测试（用 CTest 注册名）
ctest --test-dir build -R '^novaterm_scrollback_tests$' --output-on-failure
ctest --test-dir build -R '^novaterm_(renderer|renderer_p5)_tests$' --output-on-failure

# core 标签包括 core、terminal_ops、scrollback 三项
ctest --test-dir build -L core --output-on-failure
```

Visual Studio / Ninja Multi-Config 必须用 `-C` 选择已构建配置，否则会报
"Test not available without configuration"。按上面的 Release 构建示例：

```bat
ctest --test-dir build-vs -C Release -R "^novaterm_scrollback_tests$" --output-on-failure
```

确需全套时，可运行 `ctest --test-dir build --output-on-failure`，或使用
`check` 目标（会构建相关目标并执行全套，多配置时传入对应 `--config`）：

```bash
cmake --build build --target check
# Visual Studio 示例
cmake --build build-vs --config Release --target check
```

环境前提：

- Windows 上运行测试需要 Qt 的 `bin` 目录在 PATH 中（构建输出目录只部署了
  应用自身的运行时，不含 `Qt6Test.dll`）。offscreen 测试需要对应平台插件；
  CMake 会尝试部署该插件，也可显式设置 `QT_PLUGIN_PATH` 指向 Qt 的 `plugins`。
- 不要在 Windows 全套测试前全局设置 `QT_QPA_PLATFORM=offscreen`；
  `novaterm_terminal_session_tests` 需要 D3D11，相关单测由 CMake 各自设置 offscreen。
- `novaterm_session_tests` 会在 Linux/BSD 上读写真实密钥环
  （freedesktop Secret Service）；没有 `org.freedesktop.secrets` 时相关用例
  自动 `QSKIP`；没有默认集合时也会跳过。有可用密钥环时测试写入临时条目并删除。
- 已知失败包括平台／环境问题及 Windows 串口文件发布用例失败，见
  [AGENTS.md 的「已知测试失败」](AGENTS.md#已知测试失败不是回归别去追)。

Windows PowerShell 环境示例（Qt 安装路径按本机调整）：

```powershell
$env:PATH = "C:\Programs\Qt\6.8.3\msvc2022_64\bin;" + $env:PATH
$env:QT_PLUGIN_PATH = "C:\Programs\Qt\6.8.3\msvc2022_64\plugins"
ctest --test-dir build -R '^novaterm_renderer_tests$' --output-on-failure
```

部分 Windows 测试使用 WIN32 子系统，失败时 stdout 和 CTest 日志可能为空。
可用 QTest 文件输出定位断言，例如单配置构建：

```powershell
& .\build\bin\novaterm_renderer_tests.exe -o build/renderer-tests.txt,txt
Select-String -Path build/renderer-tests.txt -Pattern 'FAIL!|Totals'
```

多配置构建的可执行文件位于 `build-vs/bin/Release/`，需相应调整路径。

### Windows 虚拟串口压力测试

手工验收夹具覆盖 1M、3M、5M、8M 波特率，64 KiB／1 MiB／8 MiB 数据量，
双向收发端切换、可见／隐藏窗口及 64 MiB 不限速突发。
使用生产接收、解析与渲染通路，检查帧序号、CRC32、整流 SHA-256、队列收敛及
解析后的末尾文本。需事先准备互连且未被占用的两个串口；不加入默认 CTest。
构建与运行方法见 [串口压力夹具说明](tests/benchmarks/serial-stress/README.md)。
本机虚拟串口的短测与长测结果、已复现的丢帧及定位对照见
[串口压力验收报告](docs/architecture/Performance_Serial_Stress_2026-10-07.md)。

### 渲染大文本测试

Windows 大文本夹具的参数与文件 I/O 回归测试不启动 GPU：

```bash
cmake --build build --target novaterm_renderer_large_input_tests
ctest --test-dir build -R '^novaterm_renderer_large_input_tests$' --output-on-failure
```

真实 GPU 测量手工运行 `novaterm_renderer_large_input_benchmark`，参数为结果 JSON
路径、分块字节数（65536/262144）与可选 `--stable-timers`。
构建与同机验收口径见[渲染验收报告](docs/architecture/Performance_Render_Rollback_2026-10-07.md)。

### P9 独立协议测试（无需 Qt）

```bash
cmake -S src/filetransfer -B build/filetransfer -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/filetransfer
ctest --test-dir build/filetransfer --output-on-failure
# 已配置根工程时，精确选择六项纯协议测试
ctest --test-dir build -R '^novaterm_(filetransfer_(checksum_tests|support_tests|no_qt_link_check)|[xyz]modem_tests)$' --output-on-failure
```

`filetransfer` 标签还包含 `novaterm_serial_file_transfer_tests` 和
`novaterm_serial_file_transfer_ui_tests`，因此根工程的 `-L filetransfer`
会运行八项；串口接线改动可用 `-L serial` 选择这两项。
多配置构建的协议测试同样需要 `-C` 选择已构建配置。

独立库包含 XMODEM checksum/CRC/1K、标准 YMODEM batch、ZMODEM CRC16/32。
XMODEM 未提供真实长度时保留末包填充；协议库不打开文件、不占用串口。
外部 lrzsz 互通需要显式运行，使用方法及两个上游 CRC16 空文件缺陷跳过项见
[P9 阶段文档](docs/architecture/stages/P9_File_Transfer_Protocols.md)。

## 许可证

本项目采用GPLv2+许可证开源，详见 [LICENSE](LICENSE) 文件。

## 贡献

欢迎提交Issue和Pull Request。在提交代码前，请确保：

1. 遵循现有代码风格
2. 添加适当的单元测试
3. 更新相关文档

## 联系方式

- 项目地址：[Gitee / NovaTerm](https://gitee.com/han-shao-jun/NovaTerm)
- 问题反馈：请在Gitee Issues中提交
