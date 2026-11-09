# NovaTerm

NovaTerm 是基于 Qt 6 的跨平台终端模拟器和 SSH 客户端，适用于日常终端操作、
远程服务器管理和嵌入式设备调试。界面采用 ElaWidgetTools 的 FluentUI 风格，
终端解析使用 libvterm，渲染使用 Qt QRhi。

## 主要特性

- **多协议连接**：本地 Shell、SSH、Telnet 和串口，支持多标签会话。
- **终端显示**：ANSI/VT 解析、Unicode 文本、历史回看、内容搜索与高亮。
- **GPU 渲染**：使用 QRhi，支持 OpenGL、Vulkan、D3D 等图形后端。
- **外观设置**：深浅色界面、独立终端配色方案、字体与文本高亮配置。
- **SFTP 文件管理**：上传、下载、目录浏览、创建、重命名、权限修改与删除。
- **串口文件传输**：支持 XMODEM、YMODEM、ZMODEM 发送和接收，提供进度与取消操作。
- **远程系统监控**：查看 SSH 主机的系统资源信息。
- **历史与录制**：可选 LZ4 内存历史压缩，以及手动启停的持久录制和离线查询。
- **AI MCP 接入**：外部 Agent 可观察和操作已连接终端、查询历史、等待文本、跟踪任务及传输文件。

项目面向 Windows、Linux 和 macOS。具体平台能力见 [MCP 使用说明](docs/MCP_Usage.md)
和[架构文档](docs/architecture/README.md)；macOS 的 MCP 本地文件访问后端尚未实现。

## 使用说明

### 连接与文件传输

在会话界面选择本地 Shell、SSH、Telnet 或串口并填写连接参数。
SSH 会话可通过 SFTP 面板管理远端文件，也可打开系统资源监控面板。

已连接的8数据位串口可从终端右键菜单打开“串口文件传输”，选择协议和发送文件
或接收目录。在设备端启动对应收发程序后再开始传输，NovaTerm 不自动注入设备命令。
XMODEM 接收可填写真实文件长度，留空时保留末包填充；YMODEM/ZMODEM 接收选择目录。
已有接收文件不会被覆盖，接收目录需支持硬链接。

### 历史与录制

设置中的“压缩终端历史”为可选功能，默认关闭。关闭压缩后已有历史仍可回看、
复制和搜索，无需重连；达到历史预算时仍会淘汰最旧内容。
内存历史压缩不等于持久保存，需要保留接收流时可在主菜单“历史录制”中手动启停录制。

### AI MCP 接入

MCP 服务默认关闭，启用后默认地址为 `http://127.0.0.1:38973/mcp`。
所有 AI 客户端共用统一 Bearer token 和权限，只访问已连接会话。
新就绪连接默认开启读取、输入、提权选择和清空权限，用户可撤销；
权限开启不代表该连接的提权适配器可用。

文件传输使用完整路径，首次上传需要确认，相同路径组合可记忆；SFTP 默认不覆盖目标。
启用 MCP 后可读取已有录制，录制本身仍由用户手动开启。
客户端配置、工具说明和权限边界见 [MCP 使用说明](docs/MCP_Usage.md)。

## 从源码构建

### 环境要求

- CMake 3.20+、Qt 6.8+、支持 C++17 的编译器。
- Qt 组件：Widgets、Svg、ShaderTools、LinguistTools、SerialPort、Network、HttpServer、Sql；测试还需 Test。
- Linux 另需 Qt DBus 和系统 OpenSSL 开发库；macOS 需 OpenSSL 开发库。
- Windows 需 Windows 10 1809+、MSVC 2022，并预编译仓库内 OpenSSL。
- 以下 Ninja 构建示例需要安装 Ninja。第三方源码随仓库提供。

### Linux / macOS

从仓库根目录运行：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="/path/to/Qt/6.8.3/platform"
cmake --build build --parallel 4
```

将 `CMAKE_PREFIX_PATH` 替换为本机 Qt SDK 路径。项目也提供按平台预置的路径，
与本机一致时可以省略该参数。已有构建目录应继续使用原生成器；切换生成器请新建目录。

### Windows

首次构建前运行 OpenSSL 预编译脚本，PATH 中需要 `perl.exe` 和 `nasm.exe`：

```bat
scripts\build-OpenSSL-thirdparty.bat
```

在 **x64 Native Tools Command Prompt for VS 2022** 中运行：

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="C:/path/to/Qt/6.8.3/msvc2022_64"
cmake --build build --parallel 4
```

也可使用 Visual Studio 多配置生成器：

```bat
cmake -S . -B build-vs -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/path/to/Qt/6.8.3/msvc2022_64"
cmake --build build-vs --config Release
```

`scripts/build-novaterm.bat` 含本机固定路径，不作为通用构建入口。

## 打包与安装

先完成对应配置的构建，再从仓库根目录执行：

```bash
cpack --config build/CPackConfig.cmake -C Release -B build/packages
```

默认生成器按平台选择：

| 平台 | 产物 | 安装方式 |
| --- | --- | --- |
| Linux | DEB、ZIP | 安装 DEB，或解压 ZIP 后运行包内启动程序 |
| Windows | NSIS 安装程序、ZIP | 运行安装程序，或解压便携包；生成安装程序需安装 NSIS |
| macOS | DMG、ZIP | 打开 DMG 安装应用，或使用解压后的应用包 |

若使用 `build-vs` 等其他构建目录，将命令中的配置文件路径一并替换。
也可用 `-G ZIP` 只生成压缩包，Linux 可用 `-G DEB` 只生成安装包。
产物名称包含版本、平台和架构，保存在指定的输出目录。

Debian / Ubuntu 安装示例，将文件名替换为实际 DEB 产物：

```bash
sudo apt install ./build/packages/实际文件名.deb
novaterm
```

更新安装不会自动重启已有实例；保存当前工作后重新启动以加载新程序。
便携包需要完整解压，不能只拷贝单个可执行文件。Linux 包包含输入法及桌面主题插件，
输入法服务仍由系统提供；历史录制需要包内 QSQLITE 插件。

## 配置与凭据

全局设置保存在用户配置目录的 `NovaTerm/novaterm.json`：

| 平台 | 默认位置 |
| --- | --- |
| Linux | `~/.config/NovaTerm/novaterm.json`，遵循 `XDG_CONFIG_HOME` |
| Windows | `%LOCALAPPDATA%\NovaTerm\novaterm.json` |
| macOS | `~/Library/Preferences/NovaTerm/novaterm.json` |

首次运行使用默认配置。会话历史和凭据由独立存储服务管理。
Linux 凭据使用 freedesktop Secret Service（如 gnome-keyring / KWallet）；
没有可用密钥环时仍可运行，但 SSH 密码不会持久保存到下次启动。

## 文档与参与

- [MCP 使用说明](docs/MCP_Usage.md)
- [统一架构](docs/ARCHITECTURE.md)与[模块文档索引](docs/architecture/README.md)
- [开发协作与测试指南](AGENTS.md)
- [项目仓库与问题反馈](https://gitee.com/han-shao-jun/NovaTerm)

欢迎提交 Issue 和 Pull Request。代码贡献应遵循现有风格、完成相关验证并同步使用说明。

## 许可证

项目源码采用 GPLv2+，详见 [LICENSE](LICENSE)。使用开源 Qt HTTP Server 构建并
分发的组合程序需遵守 GPLv3 分发要求，发行包附带 [LICENSE.GPL3](LICENSE.GPL3)。
LZ4 库采用 [BSD-2-Clause](third_party/lz4-1.10.0/lib/LICENSE)；
其他依赖的许可见各自源码目录。
