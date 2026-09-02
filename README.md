

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

- Qt 6.5 或更高版本
- CMake 3.20 或更高版本
- 支持C++20的编译器
- 对于Windows：需要Windows 10 1809或更高版本

### 编译步骤

```bash
# 创建构建目录
mkdir build && cd build

# 配置项目（替换/path/to/qt为你的Qt安装路径）
cmake .. -DCMAKE_PREFIX_PATH=/path/to/qt

# 编译
cmake --build . --config Release
```

### Windows特殊说明

Windows平台使用ConPTY实现本地终端，需要Windows 10 1809+版本支持。

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

项目包含完整的单元测试和集成测试：

```bash
# 运行所有测试
ctest

# 运行特定测试
ctest -R ScrollbackTests
ctest -R TerminalCoreTests
ctest -R RendererP3Tests
```

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
