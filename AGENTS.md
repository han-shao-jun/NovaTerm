# AGENTS.md

给 AI 助手的项目上手说明。目标是让新会话在不重复摸索的前提下开始工作。
**权威信息在 `docs/`，本文件只做导航与记录"读文档看不出来的东西"。**

## 项目

NovaTerm（原 WindTermQt）是基于 Qt 6 的跨平台终端模拟器 / SSH 客户端。
终端解析用 libvterm，Telnet 协商用 libtelnet，SSH 用 libssh，渲染走 QRhi
GPU 管线，UI 用 ElaWidgetTools（FluentUI 风格）。GPLv2+，仓库在 Gitee。

## 文档地图

| 文档 | 内容 |
| --- | --- |
| `docs/ARCHITECTURE.md` | **统一架构总览，最权威**。架构原则、分层、数据流、线程模型、所有权表、背压水位 |
| `docs/architecture/README.md` | 阶段文档索引 + 统一术语表 + 文档权威性说明 |
| `docs/architecture/Development_Roadmap.md` | P0–P7 依赖、状态表、**统一完成定义** |
| `docs/architecture/stages/P*.md` | 各阶段实施说明。P6 含逐步进度表与剩余工作 |
| `docs/architecture/Rendering_Architecture.md` | Snapshot、调度、命令缓存、QRhi、Glyph |
| `docs/architecture/Configuration_Profile_Theme.md` | 配置分层、Profile、Session、主题职责 |

冲突时以 `docs/ARCHITECTURE.md` 和源码为准（`docs/architecture/README.md`
的"文档权威性"一节有明确规定）。旧概念设计文档不作依据。

## 构建与测试

环境要求以 `CMakeLists.txt` 为准，**`README.md` 的描述已过期**：实际需要
Qt **6.8**（README 写 6.5）、`CMAKE_CXX_STANDARD` 是 **17**（README 写 C++20）、
CMake 3.20+。

Qt 前缀**硬编码**在 `CMakeLists.txt:14-23` 按宿主平台分支，不是通过
`CMAKE_PREFIX_PATH` 传入：

- Windows `C:\Programs\Qt\6.8.3\msvc2022_64`
- Linux `/home/super/Qt/6.8.3/gcc_64/`
- macOS `/opt/Qt/6.8.3/macos`

Windows 首次构建前需先跑 `scripts/build-OpenSSL-thirdparty.bat` 预编译
OpenSSL（需 PATH 有 `perl.exe` 与 `nasm.exe`）。产物在
`third_party/openssl-3.5.7/install/`，已 gitignore。libssh / libvterm /
libtelnet / ElaWidgetTools 都随项目从源码构建，无需预处理。

> `scripts/build-novaterm.bat` 里硬编码了陈旧路径（`E:\code\Qt\...`），
> 在当前机器上跑不通，别用它。

### Windows 构建（Ninja + MSVC）

```bash
# 必须先进 MSVC 环境；Ninja 生成器需要 cl.exe 在 PATH 上
cmd /c "call \"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat\" x64 && cmake -S . -B build && cmake --build build"
```

### 跑测试

```bash
# 测试可执行文件需要 Qt bin 在 PATH（build/bin 只有 windeployqt 部署的
# NovaTerm 运行时，缺 Qt6Test.dll）
PATH="C:/Programs/Qt/6.8.3/msvc2022_64/bin:$PATH"
# renderer 测试的 CMake 属性设了 QT_QPA_PLATFORM=offscreen，但
# build/bin/platforms/ 只有 qwindows.dll，必须补插件路径
QT_PLUGIN_PATH="C:/Programs/Qt/6.8.3/msvc2022_64/plugins"
ctest --test-dir build
```

**不要给全套 ctest 设 `QT_QPA_PLATFORM=offscreen`** —— `novaterm_terminal_session_tests`
会初始化 D3D11，offscreen 下直接崩（`0xc0000409`）。

测试目标：`novaterm_core_tests`、`novaterm_scrollback_tests`、
`novaterm_session_tests`、`novaterm_renderer_tests`、`novaterm_renderer_p5_tests`、
`novaterm_pty_tests`(Unix)、`novaterm_conpty_tests`(Win)、
`novaterm_terminal_session_tests`(Win)、`novaterm_ssh_transport_check`、
`novaterm_telnet_transport_tests`。另有 `novaterm_renderer_p5_gpu_acceptance`
默认不注册，需 `-DNOVATERM_RUN_GPU_ACCEPTANCE_TESTS=ON`。

## 已知测试失败（不是回归，别去追）

| 测试 | 原因 |
| --- | --- |
| `novaterm_conpty_tests` 的 `injectedStartupStagesRollBack`、`repeatedLifecycleReturnsResourcesToBaseline` | **平台缺陷**：`CreatePseudoConsole`/`ClosePseudoConsole` 每个生命周期泄漏约 1 个句柄。排除性证据见 `tests/transport/conpty_handle_leak_repro.c`（单线程无子进程最小复现，实测 1.04/循环）。ConPtySession 自身 8 个句柄全部正确关闭 |
| `novaterm_conpty_tests` 的 `duplexLoadAndBackpressure`、`latestResizeWins` | 偶发；`duplex` 是 20s 超时，`latestResize` 偶尔拿到旧尺寸。未查明 |

## 不可违背的架构约束

摘自 `docs/ARCHITECTURE.md` §2，改动前必读全文：

1. Parser 单写，Renderer / Search / 插件只读
2. **libvterm 类型只能存在于 VTAdapter 实现边界**（同理 libtelnet 只在 TelnetTransport 内）
3. Transport 只处理字节和连接状态，不理解终端 Cell
4. Renderer 不理解 ANSI、SSH 或配置格式
5. 所有跨线程队列必须有上限、统计和停止语义
6. 跨线程只传不可变快照或版本化数据
7. UI Theme / Terminal Scheme / Font Config 三者分离
8. Session 是运行期边界，Profile 是创建模板

`P6_Session_and_Transport.md` 的"实施禁止项"一节列了 11 条硬禁止，涉及
Session / Transport / Renderer 时先看那一节。

## 分层与关键类

```
src/core/       TerminalCore ScreenBuffer ScrollbackBuffer VTAdapter SearchEngine
src/transport/  ITransport ← LocalShellTransport SshTransport SerialTransport TelnetTransport
src/session/    TerminalSession SessionManager SessionFactory SessionInputPump SessionStore SftpSession
src/renderer/   TerminalRenderer RenderScheduler GlyphAtlas FontManager
src/ui/         TerminalView MainWindow SessionPage SettingsPage SftpPanel SystemMonitorPanel
src/platform/   windows/conpty/ linux/pty/
```

数据通路唯一入口：`ITransport::readyRead` → `SessionInputPump`
（`SessionInputPump.cpp:32`）→ `TerminalCore::writeInput`。四种 Transport
全部经 `TerminalView::attachTransport()` → `TerminalSession::attach()` →
`startPump()`，**不要新开第二条通路**。

背压水位：ByteQueue 8 MiB，暂停/恢复 6/4 MiB；`SessionInputPump`
`MaxPendingBytes` 8 MiB、`InputChunkBytes` 64 KiB。写侧 Serial / SSH / Telnet
各有 `MaxPendingWriteBytes` = 1 MiB；LocalShell 不用该常量，走
`tryEnqueueInput()` 由 PTY/ConPTY 会话层自己限容。

## 代码约定

- 注释和 Doxygen 全部中文，`@brief/@param/@return/@note`。文件头有 `@file`/`@brief` 块
- 中文注释按显示宽度折到 ~80 列
- 纯 C 库用 **PImpl 隔离头文件**：`class Impl; std::unique_ptr<Impl> _impl;`
  （范本 `VTAdapter.h:92-94`、`TelnetTransport.h`）
- 成员 `_camelCase`，常量 `static constexpr`，`[[nodiscard]]` 用在查询函数上
- 新 transport 以 `SerialTransport` 为结构模板（单通道、事件驱动）；
  需要阻塞库时以 `SshTransport` 为模板（独立线程 + 非阻塞轮询）

## 第三方依赖约定

`third_party/` 全部是 **vendored 完整上游源码树、纳入版本控制**，无 submodule、
无 vcpkg / conan / FetchContent（机器上也没装 vcpkg 和 conan）。

新增纯 C 依赖的做法：源码拷进 `third_party/`，**自己写最小 CMake target**
（范本 `third_party/libvterm-0.3.3/CMakeLists.txt`），消费方 PRIVATE 链接。
若上游自带 CMakeLists，先确认它不会 `install()` / `include(CPack)` /
`add_subdirectory(examples)` 污染主工程 —— libtelnet 就因为这个改成在根
`CMakeLists.txt` 里直接声明 target（见那一段注释）。

两个已踩过的坑：

- 根 `project()` 只声明了 `LANGUAGES CXX`。在根作用域建 C 目标必须先
  `enable_language(C)`，否则报 `CMAKE_C_COMPILE_OBJECT` 未设置
- **MSVC 上不要设 `C_STANDARD 11`**：`/std:c11` 会让 MSVC 定义
  `__STDC_VERSION__ >= 199901L`，命中某些 C 库 `INLINE` 宏的 GNU 分支
  展开成 `__inline__` 而编译失败（libtelnet 就是）

许可证：项目 GPLv2+；ElaWidgetTools MIT、libvterm MIT、libtelnet public
domain、libssh LGPL-2.1、OpenSSL Apache-2.0、Clink GPL-3（仅二进制随包分发）。
无 NOTICE / THIRD_PARTY_LICENSES 汇总文件。

## 当前进度与主要缺口

P0–P2 已完成，P3 实现完成待实机验收，P4 / P5 / P7 计划中。
**P6（Session/Transport）进行中**：Transport 层四种全部实现，但编排层未接入。

最重要的一条：**`SessionManager` 与 `SessionFactory` 实现完整，但生产代码
零使用** —— `src/ui/` 和 `src/main.cpp` 中均无命中，实际是 `TerminalPage`
每个 Tab `new TerminalView`、由 View 自建并持有 Session，会话集合由
`TerminalPage::_terminalViews` 隐式代表。这是 P6 未落地的根因，多数其他
缺口（detach 语义、Challenge 发布层）都要等它才有落点。

其余缺口与按依赖排序的剩余工作见
`docs/architecture/stages/P6_Session_and_Transport.md` 的"实现进度"与
"剩余工作"两节，不在此重复。

## 容易写错的地方

**`ITransport` 两个错误信号有顺序约定**：实现若同时发 `transportError` 与
`errorOccurred`，必须**先发 `transportError`**且 message 一致 ——
`TerminalSession` 用前者补充错误分类，由后者统一上报一条 `sessionError`。
顺序颠倒会让分类回落到 `Io`。约定写在 `ITransport.h` 的信号注释里。

**`TerminalSession` 的迟到信号有两道守卫，缺一不可**：世代号校验拦"发出时
属于旧世代、投递时接线已重建"；`isConnected()` 启发式拦跨线程投递 ——
SshTransport / LocalShellTransport 用 `invokeMethod(QueuedConnection)` 把
`emit` 推迟到 GUI 线程，`emit` 发生在重接线之后、世代号已是新值，仅靠世代号
识别不了。**不要以为有了世代号就能删掉那条启发式。**

**libtelnet 的 telopt 表不发起协商**：它只被 `_check_telopt` 用于决定是否
*接受*对端发起的协商。客户端意向必须显式 `telnet_negotiate()`。只填表会得到
"能连上但窗口尺寸与终端类型永不上报"的静默故障。该表还必须与 `telnet_t`
同寿命（`telnet_init()` 只存指针）。

**`resizeTerminal()` 不支持时不要伪装成功**：能力位不声明 `ResizeTerminal`，
接口做空实现（`SerialTransport` 是范本）。Telnet 在对端拒绝 NAWS 后同样不发报文。

**windeployqt 的副作用**：`CMakeLists.txt` 的 POST_BUILD 会往 `build/bin`
部署 `platforms/`，其中只有 `qwindows.dll`。Qt 优先用 exe 同级的插件目录，
所以任何依赖 offscreen 插件的测试都需要显式 `QT_PLUGIN_PATH`。

## 提交约定

- 提交消息中文，`type: 摘要` 开头（`feat`/`fix`/`docs`/`test`/`chore`）
- 按主题拆分提交；vendored 第三方源码单独一个提交（先例 `47f2c4e`、`5ed630c`）
- 历史提交直接在 `master` 上，未走 PR 流程
- 提交前跑 `ctest`，并对照上面"已知测试失败"确认没引入新的红灯
