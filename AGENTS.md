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
| `docs/architecture/Configuration_Profile_Theme.md` | 配置分层、Profile、Session、主题职责。**描述目标设计**，开头有与当前源码的名称对照表 |

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

> `scripts/build-novaterm.bat` 当前可在本机使用：它加载
> `C:\Programs\MicrosoftVisualStudio\18\Insiders` 的 MSVC 环境并构建已配置的
> `E:\code\Qt\NovaTerm\build\Release`。它不会完成首次 CMake 配置或 OpenSSL
> 预编译，因此仅适合该构建目录已经存在的增量 Release 构建。

### Windows 构建（Ninja + MSVC）

```bash
# 必须先进 MSVC 环境；Ninja 生成器需要 cl.exe 在 PATH 上
cmd /c "call \"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat\" x64 && cmake -S . -B build && cmake --build build"
```

### 跑测试

**默认只跑与改动相关的测试目标，不要跑全套。** 全套 9 项实测约 **190 秒**，
其中 `novaterm_conpty_tests` 单项 94s、`novaterm_terminal_session_tests` 46s、
`novaterm_core_tests` 30s；而多数改动只需要其中一两项、几秒就跑完。

```bash
# 测试可执行文件需要 Qt bin 在 PATH（build/bin 只有 windeployqt 部署的
# NovaTerm 运行时，缺 Qt6Test.dll）
PATH="C:/Programs/Qt/6.8.3/msvc2022_64/bin:$PATH"
# renderer 测试的 CMake 属性设了 QT_QPA_PLATFORM=offscreen，但
# build/bin/platforms/ 只有 qwindows.dll，必须补插件路径
QT_PLUGIN_PATH="C:/Programs/Qt/6.8.3/msvc2022_64/plugins"

# 按名字挑（首选，最精确）
ctest --test-dir build -C Debug -R novaterm_scrollback_tests
# 按标签挑，label 见下表；注意 -L core 是 core + scrollback 两项
ctest --test-dir build -C Debug -L core
# 一次改动跨了多个模块就挑多项
ctest --test-dir build -C Debug -R "novaterm_(renderer|renderer_p5)_tests"

# 全套 —— 只在下面「什么时候才跑全套」列出的场景用
ctest --test-dir build -C Debug
```

`-C Debug` 不能省：上面的 cmake 命令不传 `-G`，默认落到 Visual Studio 多配置
生成器，不带 `-C` 时每个测试都报 "Test not available without configuration"
并整体失败。

#### 改哪测哪

| 改动位置 | 跑这个 | label | 耗时 |
| --- | --- | --- | --- |
| `src/core/terminal/`（TerminalCore、ScreenBuffer、VTAdapter、ScrollbackBuffer、BoundedByteQueue、KeyMapper）—— 后三者经 `TerminalCore.h` 传递覆盖；KeyMapper 有专项单测 | `novaterm_core_tests` | `core` | ~30s |
| `src/core/scrollback/`、`src/core/search/` | `novaterm_scrollback_tests` | `scrollback` | <1s |
| `src/session/`、`src/profile/`、`src/credential/` | `novaterm_session_tests` | `session`／`p6` | <1s |
| `src/renderer/` 的 RenderCommandBuffer / RenderScheduler / TerminalRenderer | `novaterm_renderer_tests` | `renderer` | ~2s |
| `src/renderer/` 的 RowBlockDamageTracker / ScrollDamageHandoff / TerminalHighlighting、`src/session/SerialHighlightRules` | `novaterm_renderer_p5_tests` | `p5` | <1s |
| `src/transport/LocalShellTransport` 与 ConPty 路径 | `novaterm_conpty_tests`(Win)／`novaterm_pty_tests`(Unix) | `conpty` | ~94s |
| `src/transport/SshTransport`、`SshMonitorProtocol` | `novaterm_ssh_transport_check`（失败路径 + 监控帧协议） | `ssh` | <1s |
| `src/transport/TelnetTransport` | `novaterm_telnet_transport_tests` | `telnet` | ~5s |
| TerminalSession + TerminalRenderer + LocalShellTransport 的联通路径 | `novaterm_terminal_session_tests` | `terminal-session` | ~46s |
| `TerminalTabWidget` 的连接动作/紧凑标题、`SystemInformationDialog` 的滚动范围/布局与 app bar 关闭按钮、`SshHostKeyDialog` 的 Ela 控件与端点标题 | `novaterm_ui_dialog_layout_tests` | `ui` | <1s |
| `src/ui/`、`src/platform/`、`src/service/` | **无覆盖测试** —— 编译通过 + 实跑程序看效果即可（`KeyMapper` 已移出此列，现由 `novaterm_core_tests` 覆盖） | — | — |

SSH 资源监控另有不注册到 ctest 的
`novaterm_ssh_monitor_integration_check`：它读取 AppData 中唯一的 SSH 历史会话
（或标题含 zynq 的会话）及 Windows 凭据引用，验证慢命令并发、交互 I/O、暂停
回收和重连。仅在明确允许连接对应测试服务器时人工运行，且不得输出凭据。

**Linux/BSD 的凭据后端要 Qt6::DBus，`novaterm_session_tests` 会读写真实密钥环**：
`find_package(Qt6 ... DBus)` 只在 `UNIX AND NOT APPLE` 时条件加入，
`NOVATERM_SECRET_SERVICE_LIBS`（根 `CMakeLists.txt` 定义，供 `tests/CMakeLists.txt`
复用）贯穿三个直接编译 `src/credential/CredentialStore.cpp` 的目标：`NovaTerm`、
`novaterm_session_tests`、`novaterm_ssh_monitor_integration_check`。
`secretServiceCredentialStoreSurvivesRestart` 直接连会话总线：没有
`org.freedesktop.secrets` 或没有默认集合时 `QSKIP`，有则写入
`service=NovaTerm` + `novaterm-ref=novaterm-test-<uuid>` 的条目并在结束时删除，
因此要求当前会话有密钥环（gnome-keyring/KWallet）；CI 或纯 headless 环境 skip
属预期，不是失败。

上表 UI 覆盖的例外有两处：`TerminalView` 启动、尺寸传递与生命周期已由
`novaterm_terminal_session_tests` 的 `TerminalSessionSmokeTests.cpp` 覆盖；
终端标签动作、系统信息对话框的滚动范围/app bar 关闭按钮与 SSH 主机密钥对话框由
`novaterm_ui_dialog_layout_tests` 覆盖（offscreen 运行）；前者断言
连接动作/紧凑标题，系统信息断言"内容高度 == max(视口, heightForWidth)"与
"滚到底内容底边贴视口底"，防
"能滚进空白页"回归，后者断言 Ela 控件类型与变更主机端点标题。关闭按钮那条
用 `mallopt(M_PERTURB)` 污染已释放内存，让"关窗后仍访问已析构窗口"的
use-after-free 稳定复现（否则释放内存内容未变，可能碰巧不崩）。这类改动应跑
对应目标，普通面板外观改动仍按编译与实跑验证。

拿不准某个文件被哪个测试覆盖，就看测试源码的 include。`tests/core`、
`tests/renderer`、`tests/session`、`tests/transport`、`tests/ui` 五个目录，
**一个 `.cpp` 对一个测试目标**，翻一眼就能确认。
测试、人工检查与 benchmark 目标统一声明在 `tests/CMakeLists.txt`；根
`CMakeLists.txt` 只负责启用 CTest 并通过 `add_subdirectory(tests)` 引入。

#### 什么时候才跑全套

只有这几种情况值得付那 190 秒，此外一律按上表挑：

- 改了各模块共用的地基，且动到**接口或数据布局**：
  `core/terminal/TerminalTypes.h`、`ScreenBuffer`、
  `core/scrollback/ScrollbackTypes.h`、`transport/ITransport.h` 这类；
- 一次改动同时命中 3 个以上模块；
- 改了 `CMakeLists.txt` 的编译选项、Qt 版本或第三方依赖；
- 合并他人分支之后。

单纯的 UI 改动、注释与文档改动、单模块内的局部修复都不在其中 —— 那些情况下
跑全套只是在等 190 秒，不会多发现任何东西。

**测试可执行文件是 WIN32 子系统程序，stdout 不接管道** —— 从 Git Bash 直接跑
它们会看到"零输出、退出码非零"，ctest 的 `LastTest.log` 里同样是空的。要看
断言详情用 QTest 自带的文件输出：

```bash
./build/bin/Debug/novaterm_renderer_tests.exe -o D:/qt/NovaTerm/build/rt.txt,txt
grep -E "FAIL!|Totals" build/rt.txt
```

**不要给全套 ctest 设 `QT_QPA_PLATFORM=offscreen`** —— `novaterm_terminal_session_tests`
会初始化 D3D11，offscreen 下直接崩（`0xc0000409`）。

默认注册到 ctest 的测试即上表九项。另有不注册的人工
`novaterm_ssh_monitor_integration_check`；
`novaterm_renderer_p5_gpu_acceptance` 也默认不注册，需
`-DNOVATERM_RUN_GPU_ACCEPTANCE_TESTS=ON`。

SSH 可选本机验收：Linux 上显式运行
`build/Release/bin/novaterm_ssh_transport_check --local-ssh-check`。
需要 `sshd`、`ssh-keygen`、`ninja`、`c++`，临时 sshd 仅监听
127.0.0.1:42222；密钥、known_hosts、32 目标构建产物均位于临时目录，结束时
回收。默认 ctest 不启动该服务。覆盖 idle CPU、10/50/100 MiB 字节内容、
持续输出时 command/monitor/resize、Ctrl+C 和真实 Ninja 构建。

## 已知测试失败（不是回归，别去追）

| 测试 | 原因 |
| --- | --- |
| `novaterm_conpty_tests` 的 `injectedStartupStagesRollBack`、`repeatedLifecycleReturnsResourcesToBaseline` | **平台缺陷**：`CreatePseudoConsole`/`ClosePseudoConsole` 每个生命周期泄漏约 1 个句柄。排除性证据见 `tests/transport/conpty_handle_leak_repro.c`（单线程无子进程最小复现，实测 1.04/循环）。ConPtySession 自身 8 个句柄全部正确关闭 |
| `novaterm_conpty_tests` 的 `duplexLoadAndBackpressure`、`latestResizeWins` | 偶发；`duplex` 是 20s 超时，`latestResize` 偶尔拿到旧尺寸。未查明 |
| `novaterm_pty_tests`（Linux 本机） | 环境相关：PTY 子进程未按预期启动 —— `defaultWorkingDirectoryIsHome`、`workingDirectoryAndMergedEnvironmentReachChild` 拿不到子进程输出，`connected.wait(5000)` 超时，退出码收到 `0xFFFFFFFF`。在 `git stash` 掉全部 `src/` 改动后重建的未修改工作树上同样失败，非回归 |
| `novaterm_ui_dialog_layout_tests`（Linux 本机） | 环境相关：offscreen + 本机 Ela/字体度量下 `1100x760` 一档的滚动上限断言不符（`scrollMax=344` vs `expectedMax=250`），另两档尺寸通过。同样在未修改工作树上复现，非回归 |

## 不可违背的架构约束

摘自 `docs/ARCHITECTURE.md` §2，改动前必读全文：

1. Parser 单写，Renderer / Search 只读
2. **libvterm 类型只能存在于 VTAdapter 实现边界**（同理 libtelnet 只在 TelnetTransport 内）
3. **核心层 `src/core/` 不依赖任何 UI 框架**：Qt 类型（QObject、QString、QByteArray、
   QVector、QKeyEvent、QRegularExpression 等）只能出现在门面层及以上，核心用标准库
   等价物（`std::string`(UTF-8)、`std::vector`、`std::mutex`、`ByteView`、
   `NovaTerm::Key` 等）。**进行中，未收口** —— 详见下方"去 Qt 化现状"
4. Transport 只处理字节和连接状态，不理解终端 Cell
5. Renderer 不理解 ANSI、SSH 或配置格式
6. 所有跨线程队列必须有上限、统计和停止语义
7. 跨线程只传不可变快照或版本化数据
8. UI Theme / Terminal Scheme / Font Config 三者分离
9. Session 是运行期边界，Profile 是创建模板

`P6_Session_and_Transport.md` 的"实施禁止项"一节列了 11 条硬禁止，涉及
Session / Transport / Renderer 时先看那一节。

### 去 Qt 化现状（原则 3，进行中）

核心层去 Qt 已完成主体，**但未收口**，接手前先看清边界：

- **已去 Qt**：`VTAdapter`、`ScreenBuffer`、`ScrollbackBuffer`/`ChunkedScrollback`、
  `LineLayout`、`BoundedByteQueue`、`KeyMapper`，以及全部跨模块数据结构。整数用
  `CoreTypes.h` 的 `isize`/`u8`/`u32`/`u64`；字节用 `ByteView`；字符串用
  UTF-8 `std::string`；容器用 `std::vector`/`std::shared_ptr`；并发用 `std::mutex`/
  `condition_variable`/`thread`；键盘用 `NovaTerm::Key`/`KeyModifier`（Qt→核心的
  翻译在 `TerminalCore.cpp` 匿名命名空间的 `coreKeyFromQt`/`coreModsFromQt`）。
- **仍依赖 Qt（未做）**：`TerminalCore` 本身还是 `QObject` 门面（信号用
  QString/QByteArray，输入收 QKeyEvent）；`SearchEngine`/`ReflowEngine` 还是
  `QObject`，且 `SearchEngine` 内部仍用 `QRegularExpression`。故 `novaterm_core`
  **仍链接 `Qt::Core`/`Qt::Gui`**。
- **剩余工作（下个会话接手，阶段 5/7/8）**：
  - **阶段 7 — QObject 剥离 + 新建 `src/coreqt/` 门面**（最大一步）：
    `TerminalCore` 改名 `NovaTerm::TerminalEngine`（去 `QObject`/`Q_OBJECT`/signals），
    `Runtime::publishPendingSignals()` 改为组装一个 `PublishedBatch`（就是现在一次性
    投递的那组 damage/cursor/title/output/scrollback 数据）调 `observer.publish()`；
    `setBackpressure`/`reportOverload` 也改走 observer（注意二者会从 GUI 线程的
    enqueue 路径触发，门面侧入队要线程安全）。`SearchEngine`/`ReflowEngine` 去
    `QObject`，构造注入 sink `std::function<void(SearchBatch&&)>`（worker 线程调用），
    由 engine 转发到 observer。新建 `src/coreqt/TerminalCore.h/.cpp`：**QObject 门面，
    类名/方法名/信号签名与今天完全一致**，故 14 处 `connect` 与 8 个消费文件只需把
    `#include "core/terminal/TerminalCore.h"` 改成 `#include "coreqt/TerminalCore.h"`。
    9 个 `Q_DECLARE_METATYPE` 与 2 处 `qRegisterMetaType`（SearchEngine.cpp、
    LineLayout.cpp）迁到 `src/coreqt/CoreMetaTypes.h`。门面在 observer 回调里做
    std↔Qt 桥接（当前 TerminalCore.cpp 里那几处 `QString::fromUtf8`/QByteArray 桥接
    即其雏形）。`ScrollbackTests.cpp` 里 6 处 `QSignalSpy(&search,…)` 改成 sink 回调 +
    原子标志 + `QTRY_VERIFY`。
  - **阶段 5 — 搜索匹配器注入 `ITextMatcher`**（**用户已暂停，恢复前不要做**）：
    新增 `src/core/search/ITextMatcher.h`（`next(string_view,from)->optional<MatchRange>`
    + factory）；`SearchEngine` 构造接受可选 factory，未注入时用 core 内置
    `LiteralMatcher`（ASCII 折叠 + 词边界）；`runSearch` 的正则构造段与
    `potentiallyUnboundedRegex` 搬到 `src/coreqt/QtTextMatcher.cpp`，行为逐项保留。
    可与阶段 7 合并做（正则实现随 coreqt 一起落地，全程正则不断）。
  - **阶段 8 — CMake 收口 + 防回归**：`novaterm_core` 去掉 `Qt::Core`/`Qt::Gui`
    链接、加 `AUTOMOC OFF`（照 libtelnet 的 `CMakeLists.txt:94-95`；全局 AUTOMOC 由
    `qt_standard_project_setup()` 打开，必须显式关）；新建 `novaterm_core_qt` STATIC
    （`src/coreqt/*`）PUBLIC 链 core + Qt，`NovaTerm` 改链它；`GLOB_RECURSE` 的
    EXCLUDE 追加 `src/coreqt/`；**新增 `novaterm_core_no_qt_link_check` 可执行目标**
    （只链 `novaterm_core`、不链任何 Qt，main 里构造 TerminalEngine 喂几字节取一次
    snapshot）——静态库不做链接解析，只有这样一个不链 Qt 的可执行目标才能真正证明
    core 无 Qt 符号，是本项收尾的**验收判据**。**此步被 SearchEngine 的
    QRegularExpression 阻塞**，须先完成阶段 5 或把 SearchEngine 移入 coreqt。
  - 注意 `ChunkedScrollback` 的整块 CoW 已在阶段 0 消除，容器换 std::vector 无
    每分块深拷贝隐患（已完成）。
- **改核心层时**：新增 `.h`/`.cpp` 不要 `#include` 任何 `Q*` 头；需要 Qt 的功能
  放到门面层（当前即 `TerminalCore` 的门面部分，将来是 `src/coreqt/`）。
  `KeyMapper.h` 暴露 `VTermKey` 是 §2 边界的既有例外（仅映射用），因此
  `novaterm_core_tests` 显式加了 libvterm 头目录。

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
- 新建线程时**必须**在入口函数第一行调用
  `NovaTerm::setCurrentThreadName("nvterm-<模块>")`（`src/core/ThreadNaming.h`）。
  QThread 站点照常 `setObjectName()`，但两处共用同一个字符串常量而非各写一份
  字面量。模块内部的辅助线程取 `<模块>-<角色>`，如 `conpty-reader`。名字必须是
  ASCII 且不超过 15 字节（`MaxThreadNameBytes`，源自 Linux `TASK_COMM_LEN`）
- 新 transport 以 `SerialTransport` 为结构模板（单通道、事件驱动）；
  需要阻塞库时以 `SshTransport` 为模板（独立线程 + 非阻塞轮询）

## 第三方依赖约定

`third_party/` 全部是 **vendored 完整上游源码树、纳入版本控制**，无 submodule、
无 vcpkg / conan / FetchContent（机器上也没装 vcpkg 和 conan）。

**各依赖的改动政策不同，别一概而论**：

- `ElaWidgetTools` **允许并且已经有本地改动**，与上游有分歧（先例 `218afcb`
  修弹窗尺寸告警、`a2d6fb4` 改 tooltip 计时、`5ee1517` 加垂直选项卡、
  2026-09-14 修 `ElaAppBarPrivate::onCloseButtonClicked()` 的 use-after-free）。
  本地新增的组件：
  | 组件 | 用途 |
  | --- | --- |
  | `ElaTreeWidget` | `ElaTreeView` 的 item 版本。上游只有基于 QTreeView 的 `ElaTreeView`，需要 `QTreeWidget`/`QTreeWidgetItem` 便捷 API 的调用方无法同时获得 Fluent 外观 |
  | `ElaTreeWidgetStyle` | 上面那个的样式，派生自 `ElaTreeViewStyle`，只多一个 `IsFrameVisible` 开关用于关掉视口外框 |

  **新增这类适配组件必须放在库内，不能放 `src/`**：绘制依赖的
  `DeveloperComponents/Ela*Style.h` 一律**没有 `ELA_EXPORT`**，库外
  `new ElaTreeViewStyle(...)` 直接链接失败。
- `libtelnet` **必须保持零改动** —— 它是一份 git clone，本地改动会造成后续 pull
  冲突。所以它不走 `add_subdirectory`，而是在根 `CMakeLists.txt` 里直接声明
  target（见那一段注释）。

**Ela 子项目的 `FILE(GLOB ...)` 没有 `CONFIGURE_DEPENDS`**（根工程的
`GLOB_RECURSE src/*` 有）。往 `third_party/ElaWidgetTools/` 加文件后必须显式重跑
`cmake -S . -B build`，只 `cmake --build` 不会把新文件编进去。

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

P0 / P2 / P4 已完成，P1 架构边界完成（宽字符 continuation 与部分属性映射待补），
P3 与 P5 实施完成、部分平台或人工验收待做，P7 计划中。
**P6（Session/Transport）进行中**：Transport 层四种全部实现，但编排层未接入。

各阶段状态以**阶段文档自身的状态行**为准；`docs/architecture/README.md` 与
`Development_Roadmap.md` 的汇总表是同步过去的副本，若发现不一致以阶段文档为真。

最重要的一条：**`SessionManager` 与 `SessionFactory` 实现完整，但生产代码
零使用** —— `src/ui/` 和 `src/main.cpp` 中均无命中，实际是 `TerminalPage`
每个 Tab `new TerminalView`、由 View 自建并持有 Session，会话集合由
`TerminalPage::_terminalViews` 隐式代表。这是 P6 未落地的根因，多数其他
缺口（detach 语义、Challenge 发布层）都要等它才有落点。

其余缺口与按依赖排序的剩余工作见
`docs/architecture/stages/P6_Session_and_Transport.md` 的"实现进度"与
"剩余工作"两节，不在此重复。

## 容易写错的地方

**线程名必须显式设置，`QThread::setObjectName()` 在 Windows release 下对 OS 不可见**：
Qt 6.8 的 QThread 文档写的是 "you can call setObjectName() before starting the
thread ... Note that this is currently not available with release builds on
Windows" —— 也就是说该名字只在 Linux/Unix 上会写进内核线程名（`ps -L`、
`top -H`、`/proc/<pid>/task/<tid>/comm` 可见），Windows release 构建里调试器与
性能分析器只看得到进程名；`std::thread` 更是没有可移植的命名入口。因此统一走
`src/core/ThreadNaming.h`：**线程入口函数第一行**调用
`setCurrentThreadName()`（Windows 落到 `SetThreadDescription`，release 同样生效；
Linux/macOS 落到 `pthread_setname_np`），QThread 的 `objectName` 只作 Qt 层标识。
两个坑：(1) 名字超过 15 字节会被静默截断 —— glibc 对超长名字返回 ERANGE、内核名
保持不变，所以 `nvterm-*` 名字必须 ≤ `MaxThreadNameBytes`；(2) 直接编译生产源
文件、不链接 `novaterm_core` 的测试目标（`novaterm_renderer_p5_tests`、
`novaterm_conpty_tests`、`novaterm_ssh_transport_check`、
`novaterm_ssh_monitor_integration_check`）必须把 `src/core/ThreadNaming.cpp`
加进源列表，否则新增调用点后链接失败。

**启动 Transport 不要用 Core 旧尺寸覆盖 Renderer 的目标尺寸**：
`TerminalCore::resize()` 异步执行，布局激活后 `terminalSizeChanged` 已携带
新尺寸，但 `core->columns()/rows()` 可能仍是 80×24。`TerminalView`
启动及 attach 统一使用 `_latestResizeColumns/Rows`，否则去抖计时器也会
重发旧尺寸，PowerShell 按错误高度滚动。回归测试为
`novaterm_terminal_session_tests::terminalViewStartupPreservesPendingSize`。

**`CSI s` 在 DECLRMM 关闭时是保存光标，不是设置左右边距**：vendored
libvterm 曾无条件按 DECSLRM 处理并把光标归位，同时缺少 `CSI u` 恢复。
TUI 启动探测发送 `CSI s → CUP → CSI u` 后，进入备用屏就会保存错误位置，
退出时光标落在旧输出之前。解析器现按 DECLRMM 分流并支持 SCORC；主屏与
备用屏的光标/画笔保存槽分别保存，1049 进入前保存、退出后恢复主屏槽。
不能在 Renderer 或针对某个 TUI 修补。回归见 `TerminalCoreTests` 的
`cursorProbeBeforeAlternateScreenRestoresShellPosition`、
`alternateScreenPreservesSavedCursor`、`ansiCursorSaveRestoreDoesNotMoveOrDamage`。

**SCROLL 合并模式的 moverect 不能复制本地旧 Cell**：libvterm 回调延迟到
flush，内部屏幕已移动并可能继续改写。本地源区域未必同步，必须从 libvterm
读取当前目标区域。回归测试为
`novaterm_core_tests::batchedScreenEditsMatchIncrementalInput`。

**libvterm 的 screen resize 回调里不能读取 state lineinfo**：screen 回调发生时
新的 `VTermStateFields::lineinfos` 已建立，但 `VTermState::lineinfos[]` 和活动
`lineinfo` 指针要等回调返回后才更新。回调内调用
`vterm_state_get_lineinfo()` 会读已释放数组；Cell 可在回调内同步，lineinfo 必须
等 `vterm_set_size()` 返回后再同步。ASan 回归由
`novaterm_core_tests::resizesScreen` 覆盖。

**`ITransport` 两个错误信号有顺序约定**：实现若同时发 `transportError` 与
`errorOccurred`，必须**先发 `transportError`**且 message 一致 ——
`TerminalSession` 用前者补充错误分类，由后者统一上报一条 `sessionError`。
顺序颠倒会让分类回落到 `Io`。约定写在 `ITransport.h` 的信号注释里。

**`ssh_channel_read_nonblocking()` 的正常 EOF 也是负返回值**：libssh 0.12
会返回 `SSH_EOF`，不能用 `count < 0` 笼统判成读错误，更不能复用“输出超限”
状态。必须分别处理 `SSH_AGAIN`、`SSH_EOF`、`SSH_ERROR` 与实际正数字节数。
资源面板曾因此把只有 115 字节的正常 `df` 输出误报成超过 1 MiB。

**SSH 主动断开必须自行发布一次 `disconnected`**：`disconnect()` 会清空连接
标志并使旧输入/EOF 投递失效，不能再指望 worker 的 EOF 路径通知。清理完成后
同步通知，重复断开不重复发；否则 Session 停在 Running，标签隐藏的连接动作
不会恢复成重连按钮。Session 收到断开通知后还要恢复 Enter 重连入口。回归见
`novaterm_ssh_transport_check` 的主动断开用例与
`SessionTests::manualDisconnectKeepsTransportReconnectable`。

**远端资源采集时机分两段，不要退回"连上就全查"**：连接建立后只采面板需要的
量 —— 常驻通道（`startResourceMonitoring()`/`requestResourceSample()`）给
CPU/内存/交换/网络；面板已移除磁盘列表，`df`（`slowCommand(includeFrequency=false)`）
只在系统信息窗口可见且未最小化时采集，首次打开请求、完成后每 10 秒刷新，关闭
或隐藏时取消在途请求和重试。系统信息对话框的静态详情由 `ResourcePrefetch`（**绑定 transport、切标签
不重启**）按 `PrefetchSchedule` 分批铺开。三条不变量由
`tests/transport/SshTransportFailureCheck.cpp` 按常量推导断言，调参必须同步跑
`novaterm_ssh_transport_check`：

- **首批 ≤ 1.5s**：概览（os/kernel/host/arch/connection/ip）最先就绪，保证连接
  成功 2~3 秒后点 `_infoButton` 已有内容；还没到的卡片在对话框里显示"采集中"
  而不是"No data"（`populate(output, pending)`）；
- **批间隔 ≥ 500ms、总时长 ≤ 3.5s、批次数 ≤ 3**：实测（root@192.168.10.100，
  2 核 ARMv7）**每条远端命令约 15ms CPU，且与命令内容基本无关** —— 固定开销来自
  远端 shell/通道处理，因此**命令数比间隔更关键**；`lspci` 排最后一批，慢批次
  结束后还要吃满完成冷却，不立刻补发；
- **连接后前两个采样区间是预热**（`WarmupIntervals`）：登录 shell 启动、常驻通道
  建立与概览批落在其中。旧实现还有首帧 `df`，现已改为详情打开时按需采集；实测旧计划
  首个区间读数 4.0%、新计划 1.0%，因此预热期只更新基线、不发布 CPU/网络读数；
- 文件系统查询与预取错峰：`allowsSlowQuery()` 只允许首帧前先行，之后退避重试
  （`FileSystemDeferralMs`），不要绕过它硬发。

**面板显示的 CPU 占用率与网速都是本地按"相邻两帧差值"算出来的**，远端只回原始
文本（常驻脚本输出 `/proc/stat` 首行、`/proc/meminfo` 与 `/proc/net/dev` 原文、
`loadavg`、`uptime`，见 `SshTransport.cpp` 的 `resourceMonitorCommand()`）；
`LinuxResource::parseMetrics/cpuUsage` 与 `handleFastMetrics()` 负责解析与差分。
因此**采样区间里我方命令的开销会被计入读数**（2 核设备上单条命令约 1~2 个百分
点），排查"CPU 偏高"时先分辨是远端真实负载、我方采集开销，还是预热区间。

面板不再自己发起静态查询（旧的 `novaterm.staticResource*` 动态属性缓存已删）；
`ResourcePrefetch` 归 transport 所有，面板只借用指针并连 `destroyed` 置空。

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

**Win10 换 app style 必须用 `QT_STYLE_OVERRIDE` 而不是 `setStyle()`**：
Qt 6.8 在 Win10 上给的默认 style 是 `windowsvista`，它把 EDIT / HEADER /
SCROLLBAR / MENU 等交给 UxTheme 绘制，而 Win10 的 UxTheme 没有深色变体且
无视 QPalette —— 深色主题下这些原生 Qt 控件恒为白底（Win11 默认已是
`windows11`，纯 palette 驱动，所以无此问题）。修复在 `main.cpp` 里
`QApplication` 构造**之前** `qputenv("QT_STYLE_OVERRIDE", "windows11")`。
**不能改用 `QApplication::setStyle()`**：`MainWindow` 设了 stylesheet，其
子树改由 `QStyleSheetStyle` 绘制，而后者创建时就缓存了 `baseStyle`；
`setStyle()` 只换掉 `QApplication::style()`，`QStyleSheetStyle` 仍持有旧的
`QWindowsVistaStyle` 继续绘制。该失败模式极具误导性：诊断打印出的 app
style 已是 `QWindows11Style`、palette 也是深色，控件却照旧白底，唯一泄露
差异的是滚动条宽度从 17px 变成 12px。

顺带一条同源约束：**运行期不要换 app style**。ElaWidgetTools 给每个控件装
的是包住 app style 的 `QProxyStyle`（`setStyle(new ElaLineEditStyle(style()))`，
见 `ElaLineEdit.cpp:36`），构造时即捕获当时的 app style，换掉会留下悬垂
base 指针。主题切换只改 QPalette，不动 style。

**Ela 控件自带 QSS，调用方再 `setStyleSheet()` 会把它整体顶掉**。`ElaTreeView` /
`ElaTreeWidget` 等在构造里设了 `#ElaXxx{background-color:transparent;}`，调用方
若为了调行高、去边框再设一次 stylesheet，透明背景就没了。行高改用
`setItemHeight()` 表达（`SftpPanel.cpp` 的文件列表是范例）。同理，
别给这些控件改 `objectName` —— 那个 QSS 是 ID 选择器。

**`setFrameShape(QFrame::NoFrame)` 关不掉 item view 的外框**：Qt 无条件向 style
派发 `CE_ShapedFrame`，`NoFrame` 只让 `frameWidth` 归零，而 Ela 的树样式在该元素
里硬画圆角边线 + `BasicBaseAlpha` 底色（`ElaTreeViewStyle.cpp:127-139`）。控件已
嵌在卡片内时那圈边框是多余的，且底色会盖掉透明效果 —— 用
`ElaTreeWidget::setIsFrameVisible(false)`。实测：`_diskTree` 未关时边框 `#363636`
／内部 `#252525` 对面板 `#272727` 明显突出，关掉后三者一致。

**给自由函数加翻译要用 `Q_DECLARE_TR_FUNCTIONS`，不要 `QCoreApplication::translate()`
自拟上下文**。项目其余部分的译文都以类名作上下文，自拟一个 `.ts` 里查不到，运行时
静默回落到英文原文 —— 表现为界面中文、按钮英文。范例是
`MessagePrompts.h` 里那个只为提供上下文而存在的 `Prompts` 类。改动导致既有字符串
行号变化时，`lupdate` 会把它们当新条目并清空译文，记得回填（本次 `SessionPage`
的 `Telnet Session` 等两条就是这样丢的）。

**翻译 .ts 的例行工作流**：改代码后跑
`cmake --build build --target update_translations`（Qt 的 LinguistTools 在
`qt_add_translations` 里自动注册该 target，`CMakeLists.txt` 已带
`LUPDATE_OPTIONS -no-obsolete` 清掉源码里已删除/移走的旧条目），lupdate 会自动
扫描 `src/`、`ElaWidgetTools` 源码并刷新两个 `.ts` 的行号与新增/移除条目；然后
打开 `.ts` 回填新增条目的中文译文（遗漏的条目以 `<translation type="unfinished"/>`
标记，可 `lrelease` 后实跑验证，也可 grep `type="unfinished"` 检查）。**不要手工
维护 `.ts` 里的行号**——下一次 lupdate 会整体重排。

**Qt 官方控件文案（QFileDialog / QMessageBox / 标准右键菜单等）走 qtbase 翻译**：
`LanguageManager` 会从 exe 同级的 `translations/qtbase_<locale>.qm` 加载 Qt 自带
翻译（`applyLocale()` 里先装 qtbase、后装应用翻译器，因此工程译文优先）。该文件
由 CMake 从 Qt 安装目录的 `translations/` 复制到构建/安装产物；新增支持语言时，
除了往 `TS_FILES` 加 `novaterm_<lang>.ts`，还要在 CMake 里复制对应的
`qtbase_<lang>.qm`。

**`ElaContentDialog` 与 `ElaMessageBar` 的 parent 都不能为空，且应当传窗口一级**：
前者 `showEvent` 里直接 `parentWidget()->size()`，后者构造里直接
`parent->installEventFilter()`，传 `nullptr` 会崩或静默失效。更隐蔽的一条：
`ElaMessageBar` 的静态方法在 parent 为空时会去找顶层的 `ElaWindow`
（`ElaMessageBar.cpp:107-121`），**对模态对话框里的页面来说那是主窗口，浮层会落到
对话框背后看不见**。统一走 `NovaTerm::Ui::confirm()` / `warn()`
（`src/ui/widgets/MessagePrompts.h`），它们已经把 `->window()` 收在里面。

**`ElaContentDialog` 的按钮不需要接信号就能 accept / reject**：左键与右键的内建
处理器已经调用了 `_doCloseAnimation(false/true)`，其中直接 `reject()` / `accept()`
（`private/ElaContentDialogPrivate.cpp:19-29`），Esc 走同一条 reject 分支。但
**对话框不能栈分配** —— 那两个处理器随后用
`QTimer::singleShot(0, nullptr, ...)` 延迟发信号且捕获了 `this`，栈对象在
`exec()` 返回即析构，定时器会打在已释放的对象上。用 `new` + `deleteLater()`。

**`ElaScrollArea` 构造即把两个方向的 scrollbar policy 设成 `ScrollBarAlwaysOff`**
（`ElaScrollArea.cpp:18-19`，Ela 的设计是隐藏滚动条靠滚轮/手势）。需要可见滚动条
时必须在构造后显式恢复策略：
`setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded)`。`SystemInformationDialog` 是
现有范例，其回归测试还会断言该策略，防止替换控件后滚动条静默消失。

**`ElaScrollPageArea` 构造时会 `setFixedHeight(75)`**。设置页单行卡片可直接使用，
承载动态表格或多行内容时必须先解除限制：`setMinimumHeight(0)`、
`setMaximumHeight(QWIDGETSIZE_MAX)`，再交给布局和 size policy 决定高度。
`SystemInformationDialog::InformationCard` 是现有范例；对应 UI 回归测试断言完整
数据会生成 8 张 Ela 卡片，并继续检查滚动范围。

**`ElaDialog` 已自动预留标题栏空间**：`ElaAppBar` 构造时把窗口的
`contentsMargins.top` 设为 app bar 高度（默认 45 px），外层布局不要再加
同样用途的上边距。设置窗口曾额外加 30 px，再叠加页面 20 px 和内容 30 px，
导致第一组设置距离窗口顶部 125 px；现为自动标题栏 + 页面 8 px。

**`ElaThemeColor(mode, role)` 的 `role` 必须是枚举常量 token**：该接口是宏，展开时
会自动补 `ElaThemeType::`，传保存于成员变量的动态角色会被拼成不存在的枚举成员并
编译失败。动态主题角色统一直接调用
`eTheme->getThemeColor(mode, roleVariable)`；`MetricLegendText` 是现有范例。

**滚动区域里的内容高度不要交给布局的 `minimumSizeHint()`**：含
`QLabel::setWordWrap(true)` 的表单布局会被严重高估 —— 布局按"最窄可能宽度"
估算换行行数，实测卡片只需 886px，`minimumSizeHint().height()` 却给 989px，
滚动区照它定内容高度，于是滚到底多出约 100px 空白页。正解是让内容
控件按视口**实际宽度**算高度：`content->setMinimumHeight(max(hfw, 视口高))`，
`hfw = content->heightForWidth(viewport()->width())`，并在 `populate()`、
状态刷新与 `resizeEvent()` 里都重算一次（`SystemInformationDialog::
updateContentHeight()`）。附带的两个小坑：定时重建内容时旧控件要先
`hide()` 再 `deleteLater()`（否则当帧仍可见，看起来像状态文字重复）；
`deleteLater()` 期间重建还会把滚动位置顶回顶部，重建前后要自己存取
`verticalScrollBar()->value()`。回归测试 `novaterm_ui_dialog_layout_tests`
（offscreen，断言"内容高度 == max(视口, heightForWidth)"与"滚到底内容底边贴
视口底"）。

**软换行行的行尾空格不能裁**：`onScrollbackPush()` 里裁剪行尾空 Cell 只对
**硬换行**行安全。软换行行填满了整行才换行，其尾部空格是有效内容 —— 下一行
文本紧接其后，裁掉会让按新列宽重排后的内容整体左移。判据是 `softWrapped == 0`。

**`sb_popline` 取的是最新历史行，不是最旧**：libvterm 在屏幕**变高**时用它
反向取回紧邻屏幕顶部的那一行（`third_party/libvterm-0.3.3/src/screen.c:737-740`
是唯一调用点）。NovaTerm 存的是变长逻辑行，一条可能横跨多个屏幕行，因此只能
取走尾部一段并把剩余部分写回 —— 早期实现取最旧一行且只回填前 `cols` 格，
其余 Cell 被永久丢弃，纵向拉高窗口就会破坏历史内容。

**libvterm 不负责跨接缝的逻辑行拼接**：`screen.c:588-595` 明确写了列宽变化后
它只重排可见片段（"as its own prefix"），scrollback 里的前半段归应用层。所以
接缝处的短行不是 bug 而是必然结果，正解见
`docs/architecture/stages/P1_ScreenBuffer_and_VTAdapter.md` 的「剩余工作」。

**`QTest::mousePress(w, btn, mods, QPoint(0, 0))` 点的是控件中心**：`QPoint(0,0)`
满足 `isNull()`，QTest 会把它当作"未指定位置"并取控件中心。要点左上角必须用
非 null 坐标（如 `QPoint(1, 1)`）。写选区测试时这个坑会让起点落在屏幕中间。

**Windows 上 AltGr 报成 Ctrl+Alt**：处理键盘时不能见 Ctrl 就当控制字符。德语等
键盘 AltGr+Q=@、AltGr+8=[ 会被上报为 `Ctrl|Alt`，若走 Ctrl 分支会把 AltGr+Q
误当 Ctrl+Q 发出 0x11(XOFF) 冻结显示。判据：`Ctrl+Alt` 同时按下且 `event->text()`
是可打印字符时判为 AltGr，走文本路径。回归测试
`novaterm_core_tests::altGrProducesPrintableCharacterNotControlCode`。

**可打印文本输入不要把 Shift 转发给 libvterm**：文本已编码 Shift（"A"≠"a"），
再传 Shift 会让 libvterm 对 Shift+Space 特判发出 `CSI 32;2u` 而非 0x20
（`keyboard.c:33-35`）。`processTextInput` 只在"单独 Alt（真 meta，无 Ctrl）"时
转发 `VTERM_MOD_ALT`；AltGr(Ctrl+Alt) 不是 meta，原样发送。回归测试
`shiftSpaceSendsPlainSpace`、`altLetterSendsMetaEscapePrefix`。

**回看快照的锚点行可能已被淘汰**：`rendererSnapshot` 按 `anchorLine` 用
`lineById` 定位，但回看期间持续输出会把该行挤出 scrollback（默认仅 1000 行，
约一秒）。命不中时**必须回退到"历史末尾行 + scrollLine"**，否则 historyViewport
为空、整个回看区渲染成黑屏。回归测试 `rendererSnapshotFallsBackWhenAnchorEvicted`。

**关闭时不要排空解析队列**：`TerminalCore::Runtime` 析构曾 `waitForIdle(5000)`
把 8 MiB 积压全喂完 libvterm，大量输出中关标签页会阻塞 GUI 至多 5 秒 —— 那些
解析结果无人再看。正解：只 `stopping + bytes.stop() + join`，worker 完成当前
一批即退出。回归测试 `closingDoesNotDrainPendingInput`。

**Parser 的字节队列和命令队列必须共享唤醒链路**：Worker 空闲时在
`BoundedByteQueue::take(..., -1)` 无限等待；resize、键盘等命令成功入队或合并后
必须调用 `wakeConsumer()`。只通知字节入队会让空闲命令永久滞留；恢复 5 ms
超时轮询则会重新引入约 200 次/秒的空闲 futex 唤醒。回归测试
`boundedByteQueueWakesIdleConsumer`，命令路径由 `resizesScreen` 等 Core 测试覆盖。

**`scrollbackLines` 配置要显式接线**：`ConfigManager` 只校验存储该键，核心构造
默认 1000 行。必须在 `TerminalView` 创建 core 后 `setScrollbackLimit(配置值)`，
否则四种 Transport 的历史上限恒为 1000、用户设置形同虚设（`configuredScrollbackLines()`）。

**`std::vector::size()` 是无符号，与 `isize`/`int` 比较要显式转换**：去 Qt 后核心
容器从 `QVector`(有符号 `qsizetype`) 换成 `std::vector`(无符号 `size_t`)。诸如
`row >= vec.size()`、`vec.size() != rows` 直接写会触发有符号/无符号比较，`/W4`
下告警、边界判断也可能出错。统一写成 `isize(vec.size())` 或 `int(vec.size())`。
注意与恒正 `constexpr` 常量的比较（如 `vec.size() > MaxLines`）GCC 不告警，属同类
隐患。全部第一方 C++ 目标统一开启 `/W4`／`-Wall -Wextra -Wpedantic`
（vendored 第三方目标保持各自策略），新代码会在常规构建中暴露此类问题。
`RelWithDebInfo` 下的 GNU、Clang 与 AppleClang 另加
`-fno-omit-frame-pointer`，供性能分析保留完整调用栈。

**Qt 与标准库整数别名同宽也可能不同类型**：例如 `qsizetype` 与
`NovaTerm::isize`、`quint64` 与 `NovaTerm::u64`。混用于 `std::min/max` 时，
应显式指定目标接口的模板类型或转换参数，避免 Linux 上模板推导冲突。
修复范例见 `ScrollbackBenchmark.cpp` 的行数上限与
`TerminalRenderer::requestFullFrame()` 的 revision 合并。

异步 glyph 后，模型 revision 收敛不代表字形完成。GPU 基准必须同时等待
`glyphRasterQueueDepth == 0`；强制全帧场景还要检查实际重建行数，不能仅等待
任意新帧（可能是 overlay）。普通缺字形完成只重建 pending 行，随滚动旋转
pending 行标记，不能把每次完成都升级为全屏重建。

**凭据必须真的落到平台密钥链，别退回"内存实现"**：历史记录
（`session-history.json`）只持久化 `credentialRef`，凭据本体由
`CredentialStore` 保管。非 Windows 曾长期用 `MemoryCredentialStore`，于是重启后
引用还在、密码没了 —— 表现是**每次重连保存过密码的 SSH 历史会话都报"已保存的
SSH 凭据不可用"**（`SessionPanel::reconnectItem()` 里 `SshConfig::isValid()` 为假）。
现在非 Windows 走 freedesktop Secret Service，无密钥环时仍回退内存并
`qWarning()`，同时 `reconnectItem()` 区分"密码取不到"与"凭据整体失效"两种文案。
改这块时注意三条：

- **QtDBus 对数组参数不做内建映射**：`SearchItems` 返回的 `ao` 到手是
  `QDBusArgument` 而不是 `QList<QDBusObjectPath>`，`value<QList<...>>()` 会静默得到
  **空列表**（看起来像"密钥环里没有这条"）；必须 `beginArray()/atEnd()` 自己流出来。
  同理 `(oayays)` 结构有注册签名时才是结构体，否则也是 `QDBusArgument` ——
  `CredentialStore.cpp` 的 `objectPathList()`/`decodeSecret()` 就是干这个的。
- **调用前要注册 D-Bus 类型**：`QMap<QString,QString>`（`a{ss}`，搜索属性）漏了
  `qDBusRegisterMetaType` 会直接"Unregistered type"发不出去；`SecretValue` 还要
  额外 `QDBusMetaType::registerCustomType(type, "(oayays)")` 给出签名。
- **`SecretServiceCredentialStore::Impl` 是嵌套类，定义必须在全局作用域**：
  放进匿名命名空间会变成另一个类，构造函数与成员函数看到的类型对不上
  （报 incomplete type）；同理带 `Q_OBJECT` 的 `SecretPromptWatcher`（Prompt
  结果只能靠信号回报，而 `QDBusConnection::connect()` 需要 QObject 槽）只在
  Unix 编译，`#include "CredentialStore.moc"` 也要放在同一个条件块里。

验证入口：`novaterm_session_tests::secretServiceCredentialStoreSurvivesRestart`
（真实密钥环，无服务时 QSKIP）。

**`WA_DeleteOnClose` 的 Ela 窗口不要留默认 app bar 关闭分支（已在 Ela 内修好，
但别再引入同类写法）**：`ElaAppBarPrivate::onCloseButtonClicked()` 的默认路径是
`window = q->window()` → `window->close()` → `QApplication::processEvents()` →
`window->windowHandle()`。窗口若设了 `Qt::WA_DeleteOnClose`，`close()` 排入的
deleteLater **就被这次 processEvents() 执行掉**：窗口、它的 ElaAppBar 和正在执行
点击处理的那个按钮全部析构，函数却还在栈上，下一行的悬垂访问直接 SIGSEGV。
2026-09-14 的 core dump（系统信息窗口点关闭）崩溃帧就是
`QWidgetPrivate::windowHandle()` ← `ElaAppBarPrivate::onCloseButtonClicked`
（`ElaAppBarPrivate.cpp:57`）：`window` 指向的那块 0x80 字节内存
（`sizeof(SystemInformationDialog) == 120`）已被一个 `QMetaCallEvent` 复用，
`d_ptr` 槽位成了 `0x10001002b`，于是 `mov 0x78(%rdi)` 在 `0x1000100a3` 上取地址
失败。修法是 `QPointer<QWidget>` 重新判活后再取 `windowHandle()`。
同类风险点：`SshHostKeyDialog`（`TerminalView.cpp:459` 也设了
`WA_DeleteOnClose`）走的是同一条默认分支，修 Ela 之前同样会崩。
回归：`novaterm_ui_dialog_layout_tests` 的 `verifyAppBarCloseButtonClosesDialogSafely`，
它用 `mallopt(M_PERTURB, 0xAA)` 污染已释放内存——**不加这一步就复现不出来**，
因为悬垂窗口的内存内容往往还没变，读 `d_ptr` 会"碰巧"拿到旧值而不崩。

## 改动后必须同步文档

**终端深浅分类不跟随程序主题**：`terminal.appearance` 选择终端自身的
`dark/light` 分类，`terminal.colorScheme` 为名称或两个分类的名称对；不要用
`eTheme->themeModeChanged` 驱动终端换色。`TerminalSchemeStore` 负责命名方案、
Windows Terminal 字段解析和旧 `terminal.colors` 迁移，Renderer 不读 JSON。
设置页只在「保存并应用」后写配置并更新已打开的终端。相关回归分布在
`novaterm_renderer_tests`、`novaterm_ui_dialog_layout_tests` 和
`novaterm_terminal_session_tests::terminalColorsAreIndependentOfApplicationTheme`。
预置值集中在 `resources/terminal-color-schemes.json`，保存的 `schemes` 是完整方案库，
不再只有覆盖项；`materialize()` 只补缺失项。直接编译配色服务的目标必须调用
`novaterm_add_scheme_resources()`，否则缺少初始化/恢复模板。
跨进程读写和保存失败回滚由 UI 测试覆盖；GPU 实际换色由
`savedSchemeRepaintsExistingTerminalPixels` 覆盖，不能只断言配置对象已变化。

`Development_Roadmap.md` 的"统一完成定义"把**文档更新**列为"完成"的必要条件
之一。代码合了但文档没动，该项不算完成 —— 请在同一批提交里改掉，不要留到"以后"。

| 改动类型 | 必须同步的文档 |
| --- | --- |
| 阶段范围内的功能实现或缺口填补 | 对应 `docs/architecture/stages/P*.md` 的步骤条目 / 实现记录 / 剩余工作 |
| **阶段状态发生变化** | 三处，缺一即过期：阶段文档的状态行、`docs/architecture/README.md` 的阶段表、`Development_Roadmap.md` 的状态表 |
| 新增或移除第三方依赖 | `README.md` 特性说明、本文件"第三方依赖约定"、其中的许可证清单 |
| 构建要求、脚本、Qt 版本、测试目标变化 | `README.md` 环境要求与编译/测试步骤、本文件"构建与测试" |
| 修好一个已知测试失败，或发现新的 | 本文件"已知测试失败"表（修好的删掉，新的加上并写明原因与证据位置） |
| 踩到新的坑（API 语义、平台行为、构建陷阱） | 本文件"容易写错的地方"或"第三方依赖约定" |
| 架构边界、数据通路、所有权变化 | `docs/ARCHITECTURE.md` 相应小节。若与 §2 架构原则冲突，先讨论并改原则，不要默默绕过 |
| 换行 / reflow 策略变化 | `docs/ARCHITECTURE.md` §3.5、`docs/architecture/stages/P5_Glyph_and_GPU_Pipeline.md`。P5 里的 30 分钟实测表是历史数据，**不要改写实测值**，只追加策略变更说明 |
| 新增或移除目录 | `docs/ARCHITECTURE.md` §9 目录映射 |
| 新增 Transport / Session 相关接口约定 | 写进接口头文件的信号或函数注释，`ITransport.h` 的错误信号顺序约定是范例 |

写状态时的规矩（`docs/architecture/README.md`"文档权威性"一节的原文要求）：

- **只有代码、自动化测试和相应验收都完成后**才能写"已完成"
- 功能完成但性能或验收未达标，必须分别标注，不得写成全面验收完成
- 拿不准就写保守状态并附缺口，不要写乐观状态
- 缺口要带证据位置（`文件:行`、测试名、复现文件），便于下一个人接手

**阶段状态在三个文件里各有一份副本，两张汇总表都是从阶段文档同步过去的。**
本会话踩过这个坑：P4 在 2026-08-01 完成、P5 在 08-02 完成，但两张汇总表停在
07-31，状态错了一个多月；同期 P1 被反向高估为"已完成"。发现不一致时以阶段
文档为真，并把两张表一起改掉。

## 提交约定

- **每次创建 Git 提交都必须同步修改根 `CMakeLists.txt` 中
  `project(NovaTerm VERSION ...)` 的版本号。** 当前为 `0.2.x` 系列，每次提交
  将 PATCH 递增 1；MINOR 仅在版本规划明确调整时变更。不得创建只改代码、不改
  版本号的提交。
- 提交消息中文，`type: 摘要` 开头（`feat`/`fix`/`docs`/`test`/`chore`）
- 按主题拆分提交；vendored 第三方源码单独一个提交（先例 `47f2c4e`、`5ed630c`）
- 历史提交直接在 `master` 上，未走 PR 流程
- 提交前跑**与改动相关的测试目标**（按"跑测试"的「改哪测哪」表挑，不要跑全套），
  并对照上面"已知测试失败"确认没引入新的红灯。改动只碰 `src/ui/` 之类无覆盖测试
  的目录时，编译通过 + 实跑程序即可，不必为了走流程跑一遍无关测试
- 提交消息里写明跑了哪些测试目标。只跑了子集是正常的，但要让下一个人看得出
  哪些没跑 —— 别写成"ctest 全绿"
- 提交前对照上一节确认相关文档已同步；文档改动可以和代码同一个提交，也可以
  紧随其后单独一个 `docs:` 提交，但不要跨会话拖延
- 提交消息里如实写明与原计划不符之处（做不到的、改了方向的、发现是外部原因的），
  不要只写成功路径

## P0/P1 性能优化实施记录（2026-09-13 起）

计划与逐项步骤见 `docs/superpowers/plans/2026-09-13-perf-p0-p1-optimization.md`，
基线性能数据来自 `perf.data`（build-id `2c289bb5…`，本地 PTY 会话，
119 s / 2643 样本）。已完成 Task 1–5，证据与现状：

- **Task 1 行/块指纹合并（已完成）**：`RendererSnapshot::visibleRowBlockIdentities`
  与整行 identity 由 Core 同一次 Cell 遍历产出（`TerminalCore.cpp` 的
  `rowContentIdentity`，`RendererSnapshot::IdentityBlockColumns = 8`）；
  `RowBlockDamageTracker::reconcileRow()` 改为消费快照块指纹，渲染器里那份
  重复的 `blockIdentity()` 已删除。回归：`novaterm_core_tests` 的
  `rendererSnapshotPublishesBlockIdentities`、`rendererSnapshotHistoryRowsCarryStableIdentity`
  与 `novaterm_renderer_p5_tests::rowBlockDamageFindsOmittedStaleTail`
  （把 reconcileRow 改回整行兜底会让后者 FAIL，已用变异验证）。
  收益：基线中 `rowContentIdentity` 2.86% + `blockIdentity` 3.03%。
- **Task 2 GPU instance 暂存收窄（已完成）**：新增纯 CPU 可测的
  `TerminalRenderer::assembleSpanInstances()`（背景逐列直接覆盖、内容只清本次
  span、scratch 成员化复用）；`uploadCommands()` 每个 buffer 仍只做一次
  `updateDynamicBuffer`。回归：`novaterm_renderer_tests` 的三个 `spanAssembly*`
  用例。**注意 `contentUploadBytes` 的口径包含背景层字节**
  （`RendererP5GpuBenchmark.cpp` 的保留 stride 不变量按 5 实例/Cell 断言）。
- **Task 3 RenderCommand 容量与线性 merge（已完成）**：`RenderCommandBuffer`
  新增 `mutableRow()/finishRow()`（就地重建、复用行向量容量），
  `mergeRowCommandsIncremental()` 用一次按列线性扫描取代每行两次
  `stable_sort`；`rotateRowsUp()` 改为 clear 而非整行赋值以保留容量。
  回归：`incrementalRowMergeKeepsOrderAndColumns`（含"列宽之外旧命令不丢"）、
  `mutableRowRebuildKeepsMetadataAndDropsOutOfRange`。
- **Task 4 Glyph 稳态收窄（已完成）**：`FontManager::makeKeyAndSelection()`
  一次选择同时产出 GlyphKey + FontSelection，`ensureGlyph()` 不再第二次
  `select()`；单 ASCII 码点走 `_asciiCache`（按码点+样式直连寻址，
  generation 变化自动失效）；`ensureGlyph` 的 emoji 判定改为逐 QChar 扫描，
  去掉每 Cell 的 `toUcs4()` 堆分配。回归：
  `asciiSelectionUsesDirectCacheAndSingleQuery`（用
  `selectionQueryCount/selectionProbeCount` 断言命中不重复探测）。
- **Task 5 moverect 按行批量同步（已完成）**：libvterm 新增
  `vterm_screen_get_cells()`（整行一次解析行指针、宽字符宽度由同行右邻格回填、
  未用字符槽清零），`VTAdapter::Impl::syncRegion()` 按行读取 + 复用行缓冲，
  `ScreenBuffer::writableRowSpan()` 每行只做一次边界检查与行基址计算。
  回归：`wideCharacterWidthSurvivesBatchedSync` 与扩展后的
  `batchedScreenEditsMatchIncrementalInput`（随机字母表加入宽字符与 SGR，
  并比较属性/前景/背景）。A/B：RelWithDebInfo `novaterm_core_benchmark`
  20 MiB = 24.40 MiB/s（改造前记录 24.36 MiB/s，属噪声范围，因为该基准
  不滚动、不走 moverect）。
- **Task 6/7 亦已完成**：Task 6 的应用级事件过滤实现在下节，其窗口人工验收已由用户完成；
  RelWithDebInfo 全量构建、ASan/UBSan Core、perf 重录对比也都已补齐（见下）。唯一仍未
  复测的是 GPU 侧 A/B（`contentUploadBytes`、CPU frame P95）：本机 QRhi 在 offscreen/xcb
  下都拿不到设备。

**宽字符 `Cell::width` 可能在分批边界失真（既有缺陷，非本轮引入）**：
`width` 由"右邻格是否为延续标记"推导，只在脏矩形覆盖到该格自身时才刷新。
把随机差分测试的比较项加上 `width` 会在 `batch=14`（含 `中文` + `ESC[S`/`ESC[T`
+ `ESC[K`/`ESC[P`）稳定失败；把 `VTAdapter::syncRegion` 回退成逐格路径后
同样失败，故与本轮批量改造无关。修它需要在脏区传播时把宽字符左邻列一并向左
扩一列，尚未做；当前随机差分测试刻意不比较 `width`，另由
`wideCharacterWidthSurvivesBatchedSync` 覆盖确定路径。

### Task 6/7 补充记录（同上计划）

- **Task 6 移除 40 ms dock hover 轮询（已完成）**：删除常驻 `resizeHoverTimer`，
  改为 `MainWindow::installDockResizeHoverTracking()` 安装**应用级**事件过滤器
  （面板与 QMainWindow 内部分隔条会消费自己的鼠标事件，装到 MainWindow 或单个
  dock 上都收不到），只对属于本窗口子树的事件调度一次单次触发的
  `_dockResizeHoverTimer`（40 ms 合并抖动，仅在有事件或按住左键时活动）；
  `refreshDockResizeHighlight()` 保留原有可见性/最小化/激活/边框内判定与
  "无左键才复位 `_activeDockResizeKind`"语义。**A/B 实测**（offscreen 启动真实
  exe，隔离 `XDG_CONFIG_HOME/XDG_DATA_HOME`，8 秒空闲对比主线程
  `voluntary_ctxt_switches`）：旧实现 229 → 429（**+200，即 25.0/s**，utime +4
  ticks）；新实现 103 → 103（**0/s**）。左右 dock 高亮/移开隐藏/拖动/最小化恢复的**人工实跑验收**已由用户于 2026-09-13 在窗口环境完成；
  `pidstat -t -w` 的桌面级复测未做（空闲唤醒已用 /proc 计数做过等价对照）。
- **Task 7 回归状态**：RelWithDebInfo 全量构建通过；ASan+UBSan 构建
  （`-fsanitize=address,undefined`，Debug）跑 `novaterm_core_tests`
  **58/58 通过、无 ASan 报错、无 UBSan runtime error**；Debug 全套 ctest
  8/10（仅剩上表两项本机环境失败）；RelWithDebInfo `novaterm_core_benchmark`
  20 MiB = 24.40 MiB/s。**当时的未达标项**：perf 重录对比与 GPU 侧 A/B 在本机（无图形会话、QRhi 拿不到设备）
  无法执行。perf 重录后已由用户在桌面会话补齐（23:08 与 23:26 两次，见下两节）；
  GPU 侧 A/B（`contentUploadBytes`、CPU frame P95、memmove 占比）仍未复测。

### Task 5 同机 A/B（2026-09-13，RelWithDebInfo，各 3 次）

`novaterm_core_benchmark --bytes 20971520 --lines 100000`（该负载每行 CRLF，
120×40 视口，逐行触发滚动，正是 moverect 路径）同机对照"按行批量同步"与
"逐格 `vterm_screen_get_cell` + `setCell`"（后者临时 `git stash` 掉
`VTAdapter.cpp` 重建基准得到）：

| 实现 | 3 次结果（MiB/s） | 中位 |
| --- | --- | --- |
| 新：按行批量 | 23.53 / 24.59 / 23.74 | 23.74 |
| 旧：逐格 | 24.15 / 22.73 / 23.47 | 23.47 |

差值 +0.27 MiB/s（约 +1.2%），**落在运行间噪声内**（两组区间重叠），说明该
合成负载的 moverect 同步并不是吞吐瓶颈：`perf.data` 里 7.76% 的 per-cell 同步
开销来自真实会话的局部重写模式（光标定位改写 + 部分滚动），不是本基准的整屏
滚动。结论：Task 5 的收益必须用真实会话重录 perf 判定，本次未测得。

### Task 7 Step 5：优化后 profile 与基线对比（2026-09-13 23:08 重录）

`perf.data` 于 23:08 用**优化后**的 `build/RelWithDebInfo/bin/novaterm`
（build-id `6c606413…`，与当前 exe 一致，符号可直接解析）重录：102.8 s、
cpu_atom 510 + cpu_core 1486 = 1996 样本。与基线
（`2c289bb5…`，21:17，119.2 s，2643 样本）按同一套 bucket 口径对比：

| bucket | 基线 | 优化后 | Δ |
| --- | --- | --- | --- |
| 行/块指纹（Task 1） | 5.90% | **3.77%** | **−2.12pp** |
| RenderCommand 排序/扩容（Task 3） | 4.25% | **1.38%** | **−2.87pp** |
| Glyph 稳态（Task 4） | 4.58% | **2.89%** | **−1.69pp** |
| 分配器（malloc/free/realloc） | 6.14% | **3.41%** | **−2.73pp** |
| moverect 同步（Task 5） | 7.76% | 7.46% | −0.29pp |
| **GPU 上传/instance 装配（Task 2）** | 12.26% | **16.09%** | **+3.83pp** |
| GPU 驱动（libnvidia-glcore） | 22.71% | 20.37% | −2.34pp |
| Qt Widgets | 6.57% | 7.80% | +1.23pp |

总样本率 22.2/s → 19.4/s（**约 −12%**）。用户确认三次录制都是在 novaterm 中
跑同一条命令、窗口尺寸差异不大，故该下降与下面的 bucket 变化都视为可信的优化效果，
不是负载差异造成的。逐线程：主线程 2229 → 1672、`nvterm-parser` 344 → 299、
`nvterm-glyph` 24 → 25。

**结论与两条如实记录**：

1. Task 1/3/4 的收益在真实 profile 里可测且明显（合计约 −6.7pp，含分配器）。
2. **Task 2 未达标**：`assembleSpanInstances` 以 4.77% 成为 NovaTerm 侧最大单点，
   连同 `__memmove` 8.38%（基线 6.18%）、`makeInstance` 1.76% 使该 bucket 反而
   上升 3.83pp。推测原因是把原先内联在 `uploadCommands` 里的逐列写入抽成独立
   函数后（背景由 `QList::fill` 的 memset 变成逐列 64 B 结构体赋值）失去了内联
   与向量化，且内容仍按 4 槽/Cell 全量清零。下一步应在此处继续：背景改回
   批量 fill/紧凑装配、内容只清"上次写过而现在不用"的槽位，并复核
   `contentUploadBytes` 与 CPU frame P95。
3. **Task 5 收益未测出**：per-cell 调用开销（`cellAt/setCell/indexOf` ≈1.84pp）
   确实消失了，但 `populateCell` 从 2.19% 升到 2.73%、`syncRegion` 自身 1.42%，
   净 −0.29pp，落在噪声内。parser 线程剩余的主要成本是 `populateCell` 的逐字段
   转换与 libvterm 的按行读取本身。

### Task 2 第二轮收窄（2026-09-13，待重录验证）

针对上面"Task 2 未达标"的诊断做了三处修改（`TerminalRenderer.h/.cpp`，
`assembleSpanInstances()` 语义与接口不变，三个 `spanAssembly*` 用例保持全绿）：

1. **消除逐元素 detach**：装配前各做一次 `resize()`，随后热循环只走
   `QVector::data()` 裸指针。此前每次 `backgroundScratch[index] = ...` /
   `contentScratch[index*4+n] = ...` 在非 const 向量上都要做 detach 引用计数检查，
   是这段代码的主要开销之一。
2. **背景单次写入**：新增 `fillInstance(GpuInstance&, rect, uv, color)` 就地写字段
   （`makeInstance()` 改为它的返回值包装，供 `appendQuad` 等旧调用点使用），
   去掉"先零初始化局部 `GpuInstance` 再整块赋值"的第二次 64 字节写。
3. **内容单遍装配**：改为在同一轮里先写实际命令占用的槽位、再清零该格剩余槽位，
   删除原先对整段 `contentScratch` 的独立 `fill`（200 列 × 4 槽 × 64 B ≈ 51 KB，
   两遍扫描都超出 L1）。

**仍存在的体量与下一步**：23:08 profile 里 `__memmove` 8.38% 的调用点经
`addr2line` 定位在 `render():956`（`uploadCommands` 调用内部，137/149 样本）与
`render():805`（`rendererSnapshot`，11/149）；`QList<GpuInstance>::fill` 已降为
**0 样本**。因此剩余大头是 **QRhi staging 的上传体量本身**（背景 + 内容
共 5 实例/Cell × 64 B × 改动跨度），本轮改动不减少它。若重录后该 bucket 仍高，
下一步应做"只上传真正变化的槽位"（例如按行 slot 维护 instance 影子缓冲并跳过
未变区间），那需要新的状态与回归用例，属新工作项。

验证：`novaterm_renderer_tests` 34/34、`novaterm_renderer_p5_tests` 32/32，
`-R "novaterm_(core|scrollback|renderer|renderer_p5|session)_tests"` 5/5；
RelWithDebInfo 已重建（build-id `413f1e90…`）。

### Task 7 Step 5 完成：三份 profile 对比（2026-09-13 23:26 重录）

23:26 重录用的是含 Task 2 第二轮收窄的新 exe（build-id `413f1e90…`，与当前
exe 一致）：113.1 s，cpu_atom 485 + cpu_core 1315 = 1800 样本。三份同一口径：

| bucket | 基线 21:17 | 23:08 一轮后 | 23:26 二轮后 |
| --- | --- | --- | --- |
| 行/块指纹（Task 1） | 5.90% | 3.77% | **3.30%** |
| RenderCommand 排序/扩容（Task 3） | 4.25% | 1.38% | **1.24%** |
| Glyph 稳态（Task 4） | 4.58% | 2.89% | **2.48%** |
| **GPU 上传/instance（Task 2）** | 12.26% | 16.09% | **12.24%** |
| moverect 同步（Task 5） | 7.76% | 7.46% | 7.25% |
| 分配器 | 6.14% | 3.41% | 3.57% |
| GPU 驱动 nvidia | 22.71% | 20.37% | 23.11% |
| Qt Widgets | 6.57% | 7.80% | 7.73% |
| 样本率（样本/秒） | 22.2 | 19.4 | **15.9** |

**结论**：

1. **Task 2 的第二轮收窄生效**：该 bucket 从 16.09% 回到 **12.24%**（基线
   12.26%），`assembleSpanInstances` 已不在 Top 12、`QList<GpuInstance>::fill`
   保持 0 样本，取而代之的是 `__memset_avx2`（2.70%，零实例写入已走向量化
   memset）。"Task 2 未达标"的记录就此关闭。
2. **整体 CPU 显著下降**：样本率 22.2 → 19.4 → **15.9 /s（较基线约 −28%）**；
   用户确认三次录制跑同一条命令、窗口尺寸差异不大，故该下降可信（同时 `nvterm-parser` 344 → 275、主线程 2229 → 1505）。
3. 剩余最大单点仍是 `__memmove` 8.46%（QRhi staging 上传体量，Task 2 的
   scratch 改动不减少它）与 `populateCell` 3.09% / `vterm_screen_get_cells`
   2.79%（moverect 批量同步后的残余：逐字段转换 + libvterm 按行读取本身）。
   若要继续，方向分别是"只上传真正变化的槽位"与"把 Cell 转换合进 VTAdapter
   的按行读取"，都属新工作项，不在本计划范围内。
