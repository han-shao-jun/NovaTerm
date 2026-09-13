

# NovaTerm

NovaTerm（原名WindTermQt）是一款基于Qt框架开发的跨平台终端模拟器和SSH客户端，采用FluentUI设计理念，提供流畅的用户体验和强大的终端功能。

## 主要特性

- **跨平台支持**：完美运行于Windows、Linux等主流操作系统
- **多协议支持**：支持本地Shell、SSH（libssh）、Telnet（libtelnet，含 RFC 1143 选项协商）和串口连接
- **现代界面设计**：采用FluentUI设计语言，提供深色/浅色主题切换
- **GPU加速渲染**：基于Qt QRhi实现高效GPU渲染，支持Vulkan、OpenGL、D3D等多种图形API
- **SFTP文件传输**：内置SFTP客户端，支持文件上传下载和目录操作
- **系统监控**：支持远程SSH会话的系统资源监控
- **高性能终端**：采用libvterm处理终端解析，支持完整的VT序列
- **搜索与高亮**：支持终端内容搜索和语法高亮

## 快速开始

### 环境要求

- Qt 6.8 或更高版本（`find_package(Qt6 6.8 REQUIRED)`）
- CMake 3.20 或更高版本
- 支持 C++17 的编译器（`CMAKE_CXX_STANDARD 17`）
- 对于Windows：需要Windows 10 1809或更高版本，以及 MSVC 2022
- 对于Linux/BSD：需要 Qt 的 **DBus** 组件（凭据存入 freedesktop Secret
  Service，即 gnome-keyring / KWallet；`find_package` 里按平台条件加入），
  运行时没有密钥环也能启动，但保存的 SSH 密码活不过进程重启

### 编译步骤

```bash
# 创建构建目录
mkdir build && cd build

# 配置项目
# Qt 前缀已按宿主平台预置在 CMakeLists.txt 顶部；
# 若本机 Qt 装在别处，再传 -DCMAKE_PREFIX_PATH=/path/to/qt 覆盖
cmake ..

# 编译
cmake --build . --config Release
```

### Windows特殊说明

Windows平台使用ConPTY实现本地终端，需要Windows 10 1809+版本支持。

首次构建前需先预编译 OpenSSL（一次性，幂等）：

```bat
scriptsuild-OpenSSL-thirdparty.bat
```

该脚本要求 PATH 中有 `perl.exe` 与 `nasm.exe`，产物落在
`third_party/openssl-3.5.7/install/`。libssh、libvterm、libtelnet 与
ElaWidgetTools 都随项目从源码构建，无需额外准备。

## 项目架构

项目采用分层架构设计，核心组件包括：

### 核心层 (`src/core/`)
- `TerminalCore`：终端核心逻辑，处理终端会话状态管理
- `ScreenBuffer`：屏幕缓冲区管理
- `ScrollbackBuffer`：回滚缓冲区，支持分块存储和快照
- `VTAdapter`：VT序列适配器
- `SearchEngine`：异步搜索引擎

### 传输层 (`src/transport/`)
- `LocalShellTransport`：本地Shell传输
- `SshTransport`：SSH连接传输
- `SerialTransport`：串口传输
- `TelnetTransport`：Telnet传输

### 会话层 (`src/session/`)
- `TerminalSession`：终端会话管理
- `SessionManager`：会话管理器
- `SessionInputPump`：输入泵，处理数据流和背压

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
│   └── platform/       # 平台特定代码
├── tests/              # 测试代码
├── docs/               # 文档
├── resources/          # 资源文件
└── third_party/        # 第三方依赖
```

## 测试

本轮 SSH、Core 吞吐、异步 glyph 和 Agent 文本状态的验证方法及实测数据见
[性能优化记录](docs/architecture/Performance_Optimization_2026-09-10.md)。
Linux 可用 `novaterm_ssh_transport_check --local-ssh-check` 显式启动隔离本机
SSH 验收（需要 sshd、ssh-keygen、ninja 和 c++；不修改用户 SSH 信任文件）。

项目包含完整的单元测试和集成测试：

```bash
# 运行所有测试
ctest

# 运行特定测试（用 ctest 注册名，不是 QTest 类名）
ctest -R novaterm_core_tests
ctest -R novaterm_scrollback_tests
ctest -R novaterm_renderer_tests
```

Windows 上运行测试需要 Qt 的 `bin` 目录在 PATH 中（构建输出目录只部署了
应用自身的运行时，不含 `Qt6Test.dll`）；渲染相关测试还需要
`QT_PLUGIN_PATH` 指向 Qt 的 `plugins` 目录。

## 许可证

本项目采用GPLv2+许可证开源，详见 [LICENSE](LICENSE) 文件。

## 贡献

欢迎提交Issue和Pull Request。在提交代码前，请确保：

1. 遵循现有代码风格
2. 添加适当的单元测试
3. 更新相关文档

## 联系方式

- 项目地址：https://gitee.com/han-shao-jun/NovaTerm
- 问题反馈：请在Gitee Issues中提交
