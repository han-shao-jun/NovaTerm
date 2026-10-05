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
| `docs/architecture/Development_Roadmap.md` | P0–P8 依赖、状态表、**统一完成定义** |
| `docs/architecture/stages/P*.md` | 各阶段实施说明。P6 含逐步进度表与剩余工作 |
| `docs/architecture/stages/P9_File_Transfer_Protocols.md` | P9 独立 XMODEM/YMODEM/ZMODEM 协议库；Linux 六项专项与 124 项对端互通通过，两个上游缺陷用例跳过；Windows/macOS 与串口接线待验收 |
| `docs/architecture/stages/P8_AI_MCP_Interface.md` | P8 AI MCP：当前实现事实见 §14.2，修订设计与风险边界见 §15；七工具、2025 elicitation/2026 MRTR、低风险普通命令及 LocalShell/SSH 脚本能力已进入代码，真实桌面/跨平台/性能验收仍按阶段文档标记 |
| `docs/architecture/Rendering_Architecture.md` | Snapshot、调度、命令缓存、QRhi、Glyph |
| `docs/architecture/Configuration_Profile_Theme.md` | 配置分层、Profile、Session、主题职责。**描述目标设计**，开头有与当前源码的名称对照表 |

冲突时以 `docs/ARCHITECTURE.md` 和源码为准（`docs/architecture/README.md`
的"文档权威性"一节有明确规定）。旧概念设计文档不作依据。

## 构建与测试

环境要求以 `CMakeLists.txt` 为准：`cmake_minimum_required(VERSION 3.20)`、
`find_package(Qt6 6.8 REQUIRED ...)`、`set(CMAKE_CXX_STANDARD 17)`。
`README.md` 的「环境要求」一节已与这三处对齐（2026-09 复核）；它对
**测试**一节的描述仍不完整，见下方「跑测试」。

Qt 前缀**硬编码**在 `CMakeLists.txt:53-62`，按 `CMAKE_HOST_SYSTEM_NAME` 分支
写入 `CMAKE_PREFIX_PATH` 缓存变量（`set(... CACHE PATH ...)`，因此首次配置时
命令行 `-DCMAKE_PREFIX_PATH=...` 可覆盖）：

- Windows `C:\Programs\Qt\6.8.3\msvc2022_64`
- Linux `/home/super/Qt/6.8.3/gcc_64/`
- macOS `/opt/Qt/6.8.3/macos`

Windows 首次构建前需先跑 `scripts/build-OpenSSL-thirdparty.bat` 预编译
OpenSSL（需 PATH 有 `perl.exe` 与 `nasm.exe`）。产物在
`third_party/openssl-3.5.7/install/`，已 gitignore。libssh / libvterm /
libtelnet / ElaWidgetTools 都随项目从源码构建，无需预处理。

> `scripts/build-novaterm.bat` 写死了
> `C:\Programs\MicrosoftVisualStudio\18\Insiders` 的 vcvars 与
> `E:\code\Qt\NovaTerm\build\Release` 构建目录，**在 `D:\qt\NovaTerm` 工作区
> 不可用**（2026-10-05 复核：两条路径都不存在）。本机用下方 BuildTools 的
> vcvarsall 命令构建。该脚本也不做首次 CMake 配置或 OpenSSL 预编译。

### Windows 构建（Ninja + MSVC）

```bash
# 必须先进 MSVC 环境；Ninja 生成器需要 cl.exe 在 PATH 上
cmd /c "call \"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat\" x64 && cmake -S . -B build && cmake --build build"
```

### 跑测试

**默认只跑与改动相关的测试目标，不要跑全套。** 全套耗时**按平台不同**（见下方
「默认注册了哪些测试」）：**Linux 注册 21 项**（2026-10-05 实测 21/21 通过），
Windows 按条件推算 22 项、未实测。原 Windows 12 项全套实测约
**190 秒**，其中 `novaterm_conpty_tests` 单项 94s、`novaterm_terminal_session_tests`
46s、`novaterm_core_tests` 30s；而多数改动只需要其中一两项、几秒就跑完。

```bash
# 下面是 Windows（Git Bash / MSYS）的前提。Linux 见本节末尾的
# 「Linux 上的 ctest 没有这些 Windows 前提」
# 测试可执行文件需要 Qt bin 在 PATH（build/bin 只有 windeployqt 部署的
# NovaTerm 运行时，缺 Qt6Test.dll）
PATH="C:/Programs/Qt/6.8.3/msvc2022_64/bin:$PATH"
# renderer 测试的 CMake 属性设了 QT_QPA_PLATFORM=offscreen，但
# build/bin/platforms/ 只有 qwindows.dll，必须补插件路径
QT_PLUGIN_PATH="C:/Programs/Qt/6.8.3/msvc2022_64/plugins"

# 按名字挑（首选，最精确）
ctest --test-dir build -C Debug -R novaterm_scrollback_tests
# 按标签挑，label 见下表；注意 -L core 是 core + terminal_ops + scrollback 三项
ctest --test-dir build -C Debug -L core
# 一次改动跨了多个模块就挑多项
ctest --test-dir build -C Debug -R "novaterm_(renderer|renderer_p5)_tests"

# 全套 —— 只在下面「什么时候才跑全套」列出的场景用
ctest --test-dir build -C Debug
```

`-C Debug` 不能省：上面的 cmake 命令不传 `-G`，默认落到 Visual Studio 多配置
生成器，不带 `-C` 时每个测试都报 "Test not available without configuration"
并整体失败（count 数也见下方清单）。

#### 改哪测哪

| 改动位置 | 跑这个 | label | 平台 | 耗时 |
| --- | --- | --- | --- | --- |
| `src/core/terminal/`（TerminalCore、ScreenBuffer、VTAdapter、ScrollbackBuffer、BoundedByteQueue、KeyMapper）—— 后三者经 `TerminalCore.h` 传递覆盖；KeyMapper 有专项单测 | `novaterm_core_tests` | `core` | 全部 | ~30s |
| `third_party/libvterm-0.3.3/`（本地修正）及 VT 序列语义 | `novaterm_terminal_ops_tests` + `novaterm_core_tests`（`-L core`） | `conformance`／`core` | 全部 | <1s / ~30s |
| `src/core/scrollback/`、`src/core/search/` | `novaterm_scrollback_tests` | `scrollback` | 全部 | <1s |
| `src/filetransfer/`、`tests/filetransfer/` | `ctest -L filetransfer`（六项；也可独立配置 `src/filetransfer`，无需 Qt） | `filetransfer`／`p9` | 全部 | <1s |
| `src/session/`、`src/profile/`、`src/credential/` | `novaterm_session_tests` | `session`／`p6` | 全部 | <1s |
| `src/renderer/` 的 RenderCommandBuffer / RenderScheduler / TerminalRenderer | `novaterm_renderer_tests` | `renderer` | 全部 | ~2s |
| `src/renderer/` 的 RowBlockDamageTracker / ScrollDamageHandoff / TerminalHighlighting、`src/session/SerialHighlightRules` | `novaterm_renderer_p5_tests` | `p5` | 全部 | <1s |
| `src/transport/LocalShellTransport` 与 ConPty 路径 | `novaterm_conpty_tests`（Win）／`novaterm_pty_tests`（Linux） | `conpty`／`pty` | 互斥，见下注 | ~94s |
| `src/transport/SshTransport`、`SshMonitorProtocol` | `novaterm_ssh_transport_check`（失败路径 + 监控帧协议） | `ssh` | 全部 | <1s |
| `src/transport/TelnetTransport` | `novaterm_telnet_transport_tests` | `telnet` | 全部 | ~5s |
| `src/mcp/`、`tools/novaterm-mcp/`、`SessionDirectory`、`McpSettingsDialog`、Session ScriptProvider/SFTP | `novaterm_mcp_tests`（有界协议、分项授权、2025/2026 确认、取消/重放、交互命令、脚本写入及 UI） | `mcp`／`p8` | 全部 | ~10s |
| TerminalSession + TerminalRenderer + LocalShellTransport 的联通路径 | `novaterm_terminal_session_tests` | `terminal-session` | **仅 Win32** | ~46s |
| `TerminalTabWidget` 的连接动作/紧凑标题、`SystemInformationDialog` 的滚动范围/布局与 app bar 关闭按钮、`SshHostKeyDialog` 的 Ela 控件与端点标题 | `novaterm_ui_dialog_layout_tests` | `ui` | 全部 | <1s |
| `src/ui/widgets/SessionPanel.cpp` 的历史树多选与右键菜单 | `novaterm_session_panel_tests` | `ui` | 全部 | <1s |
| `src/ui/`、`src/platform/`、`src/service/` | **无覆盖测试** —— 编译通过 + 实跑程序看效果即可（`KeyMapper` 已移出此列，现由 `novaterm_core_tests` 覆盖） | — | — | — |

**平台列不是装饰**：`novaterm_pty_tests` 只在
`if(CMAKE_SYSTEM_NAME STREQUAL "Linux")` 里注册（macOS/BSD 无 PTY 集成测试），
而 `novaterm_conpty_tests` 与 `novaterm_terminal_session_tests` 都嵌套在
`tests/CMakeLists.txt:321` 起的 `if(WIN32)` 块内 —— **Linux 上
`novaterm_terminal_session_tests` 根本不构建**，那 46s 的联通路径覆盖在 Linux
工作树上无对应物，别把"跑过了"写进提交消息。

#### 默认注册了哪些测试

以 `tests/CMakeLists.txt` 为准（2026-09 复核）。两个开关的默认值都要注意：
`include(CTest)` 让 `BUILD_TESTING` 默认 **ON**，而
`option(NOVATERM_BUILD_BENCHMARKS ... ON)`（`tests/CMakeLists.txt:10`）默认
**也是 ON** —— 后者不是"可选的 benchmark 工具"，它底下**注册了一项 CTest 测试**。

`BUILD_TESTING=ON` 时的全量清单：

| 测试 | 注册条件 | LABELS | TIMEOUT |
| --- | --- | --- | --- |
| `novaterm_mcp_tests` | 无条件 | `mcp;p8` | 120 |
| `novaterm_filetransfer_checksum_tests` | 无条件 | `unit;filetransfer;p9` | 60 |
| `novaterm_filetransfer_support_tests` | 无条件 | `unit;filetransfer;p9` | 60 |
| `novaterm_xmodem_tests` | 无条件 | `unit;filetransfer;p9` | 60 |
| `novaterm_ymodem_tests` | 无条件 | `unit;filetransfer;p9` | 60 |
| `novaterm_zmodem_tests` | 无条件 | `unit;filetransfer;p9` | 60 |
| `novaterm_filetransfer_no_qt_link_check` | 无条件 | `unit;filetransfer;p9` | 60 |
| `novaterm_core_tests` | 无条件 | `unit;core` | 60 |
| `novaterm_terminal_ops_tests` | 无条件 | `unit;core;conformance` | 60 |
| `novaterm_scrollback_tests` | 无条件 | `unit;core;scrollback` | 60 |
| `novaterm_session_tests` | 无条件 | `unit;session;p6` | 60 |
| `novaterm_renderer_tests` | 无条件 | `unit;renderer` | 60 |
| `novaterm_renderer_p5_tests` | 无条件 | `unit;renderer;p5` | 60 |
| `novaterm_telnet_transport_tests` | 无条件 | `transport;telnet` | 60 |
| `novaterm_ssh_transport_check` | 无条件 | `transport;ssh` | 30 |
| `novaterm_ui_dialog_layout_tests` | 无条件 | `ui` | 30 |
| `novaterm_session_panel_tests` | 无条件 | `ui` | 30 |
| `novaterm_scrollback_tailfrom_scale` | `NOVATERM_BUILD_BENCHMARKS`（**默认 ON**） | `scrollback;perf` | — |
| `novaterm_pty_tests` | `CMAKE_SYSTEM_NAME STREQUAL "Linux"` | `integration;pty;linux` | 30 |
| `novaterm_conpty_tests` | `WIN32` | `integration;conpty` | 240 |
| `novaterm_terminal_session_tests` | `WIN32` | `integration;terminal-session` | 120 |

**Linux 注册 21 项，2026-10-05 实测 21/21 通过。** 构成：19 项无条件 +
1 项缩放护栏（benchmark 开关）+ 1 项 `pty`（Linux 专有）。计数以
`ctest -N` 为准，别用记忆里的数字。
**Windows 注册数是按注册条件推算的 22 项**（去掉 `pty`，加上 conpty 与
terminal-session 两项），**未在 Windows 实测**；`novaterm_session_panel_tests`
（2026-10-05 新增）、串口文件传输两项、`novaterm_terminal_ops_tests` 与 P9
协议目标都只在 Linux 上跑过。
`-L core` 命中三项（`unit;core`、`unit;core;conformance` 与
`unit;core;scrollback`），这是有意的。
`mcp`／`renderer`／`p5`／`ui` 五项设了
`ENVIRONMENT QT_QPA_PLATFORM=offscreen`（两个 `ui` 目标都在内）。

`novaterm_scrollback_tailfrom_scale` 不是普通单元测试，而是带 PASS/FAIL 判据的
缩放护栏（跑 `novaterm_scrollback_benchmark --tailfrom-scale`，比值判据、与机器
速度无关），几秒跑完、跑一次全量 ctest 时会顺带执行。

唯一**默认不注册**的 CTest 项是 `novaterm_renderer_p5_gpu_acceptance`
（需 `NOVATERM_BUILD_BENCHMARKS` **且** `-DNOVATERM_RUN_GPU_ACCEPTANCE_TESTS=ON`）。
数测试项时别用"九项"这种记忆里的数字，直接 `ctest --test-dir build -N` 数。

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
`novaterm_terminal_session_tests` 的 `TerminalSessionSmokeTests.cpp` 覆盖
（**Win32 独占**；Linux 上这条只能靠编译 + 实跑）；
终端标签动作、系统信息对话框的滚动范围/app bar 关闭按钮与 SSH 主机密钥对话框由
`novaterm_ui_dialog_layout_tests` 覆盖（offscreen 运行，全平台可用）；前者断言
连接动作/紧凑标题，系统信息断言"内容高度 == max(视口, heightForWidth)"与
"滚到底内容底边贴视口底"，防
"能滚进空白页"回归，后者断言 Ela 控件类型与变更主机端点标题。关闭按钮那条
用 `mallopt(M_PERTURB)` 污染已释放内存，让"关窗后仍访问已析构窗口"的
use-after-free 稳定复现（否则释放内存内容未变，可能碰巧不崩）。这类改动应跑
对应目标，普通面板外观改动仍按编译与实跑验证。

拿不准某个文件被哪个测试覆盖，就看测试源码的 include。`tests/core`、
`tests/renderer`、`tests/session`、`tests/transport`、`tests/ui` 五个目录，
**一个 `.cpp` 对一个测试目标**，翻一眼就能确认。
测试、人工检查与 benchmark 目标统一从 `tests/CMakeLists.txt` 引入；
P9 六个纯标准库测试声明在 `tests/filetransfer/CMakeLists.txt`，供根工程和
`cmake -S src/filetransfer` 的无 Qt 独立构建共用。根工程启用 CTest 后
通过 `add_subdirectory(tests)` 注册测试。

#### 什么时候才跑全套

只有这几种情况值得付那 190 秒（Windows 全套实测；Linux 上因缺 conpty 与
terminal-session 两项而短得多），此外一律按上表挑：

- 改了各模块共用的地基，且动到**接口或数据布局**：
  `core/terminal/TerminalTypes.h`、`ScreenBuffer`、
  `core/scrollback/ScrollbackTypes.h`、`transport/ITransport.h` 这类；
- 一次改动同时命中 3 个以上模块；
- 改了 `CMakeLists.txt` 的编译选项、Qt 版本或第三方依赖；
- 合并他人分支之后。

单纯的 UI 改动、注释与文档改动、单模块内的局部修复都不在其中 —— 那些情况下
跑全套只是在等那 190 秒（Linux 上短得多），不会多发现任何东西。

**测试可执行文件是 WIN32 子系统程序，stdout 不接管道（Windows 才有这个坑）** ——
从 Git Bash 直接跑它们会看到"零输出、退出码非零"，ctest 的 `LastTest.log` 里同样
是空的。要看断言详情用 QTest 自带的文件输出：

```bash
./build/bin/Debug/novaterm_renderer_tests.exe -o D:/qt/NovaTerm/build/rt.txt,txt
grep -E "FAIL!|Totals" build/rt.txt
```

**不要给全套 ctest 设 `QT_QPA_PLATFORM=offscreen`（仅 Windows 需注意）** ——
`novaterm_terminal_session_tests` 会初始化 D3D11，offscreen 下直接崩
（`0xc0000409`）。四项自带 `ENVIRONMENT QT_QPA_PLATFORM=offscreen` 的测试由
CMake 各自设置，不要在命令行再全局覆盖。

默认注册到 ctest 的测试数见上方「默认注册了哪些测试」。另有不注册的人工
`novaterm_ssh_monitor_integration_check`；
`novaterm_renderer_p5_gpu_acceptance` 也默认不注册，需
`-DNOVATERM_RUN_GPU_ACCEPTANCE_TESTS=ON`。

**Linux 上的 ctest 没有这些 Windows 前提**：单配置 Ninja/Makefile 生成器下
`-C Debug` 可省（带上也不报错），可执行文件不是 WIN32 子系统、stdout 直接可见，
Qt bin 与 plugins 从 `/home/super/Qt/6.8.3/gcc_64/` 取（用
`QT_PLUGIN_PATH=/home/super/Qt/6.8.3/gcc_64/plugins`），且 Linux 无
`novaterm_conpty_tests` 与 `novaterm_terminal_session_tests`。可直接
`ctest --test-dir build --output-on-failure`，或 `cmake --build build --target check`
（`tests/CMakeLists.txt:34` 那个 target 会自动带上 `--build-config`）。

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
| `novaterm_mcp_tests`、`novaterm_renderer_tests`、`novaterm_renderer_p5_tests`、`novaterm_ui_dialog_layout_tests`（liurui 的 Windows 工作机，2026-09-27 实测） | 全部以 `0xc0000409`（fail-fast）崩溃、零 stdout；conpty 整项 Failed（exit 4、92s）。在**改动前**与改动后各跑两轮、且 conpty 用原始代码 A/B 复测，失败集合与退出码完全一致——机器相关，非回归，未查明根因（刷新 Machine+User PATH 无效）。该机器上跑全套时以「除上述失败项外全部通过」为绿灯标准（这是新增 P9 前的历史失败集合；新增六项协议测试未在该机器运行，不计入历史通过项，以 `ctest -N` 实际注册和新一轮结果为准） |


### P9 独立协议验收（2026-10-04）

协议库可用 `cmake -S src/filetransfer -B build/filetransfer` 独立构建，
不需要 Qt；根工程测试用 `ctest --test-dir build -L filetransfer`。
Linux 六项专项及 ASan/UBSan 通过，独立 lrzsz 大文件矩阵为 124 PASS / 2 SKIP，
详情与命令见 P9 阶段文档。`novaterm_filetransfer_interop_driver` 只在 Linux
构建，外部对端验收不默认注册 CTest，不连接真实串口或用户服务器。

- 原始 lrzsz 0.12.20 的 `zm.c::zsdata` 用 do/while 处理 size_t length，
  空文件 CRC16 发送在 length=0 时下溢并越界；对应空文件/含空文件批次接收
  两项必须写 SKIP，不能算通过。库自身的空文件路径有确定性测试。
- raw PTY 对端的 lrzsz 恢复阶段调用 `tcflush(TCIOFLUSH)`，可能丢最后 ACK；
  夹具保留 driver 的 raw PTY，以 socketpair 承接 lrzsz，不伪造 ACK。
- LeakSanitizer 受本环境 ptrace 限制；本轮 ASan/UBSan 使用
  `ASAN_OPTIONS=detect_leaks=0`，不得写成泄漏检测通过。
- Windows/macOS 构建、独立对端和真实 UART 未验收。协议引擎未接入生产
  Session/Transport/UI；不得把独立协议测试写成串口功能已经可用。

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
src/session/    TerminalSession SessionFactory SessionInputPump SessionStore SftpSession SessionDirectory
src/mcp/        MCP 协议与授权（独立库，tools/novaterm-mcp 为 stdio 入口）
src/filetransfer/ XMODEM/YMODEM/ZMODEM 协议库（纯标准库，不链 Qt）
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
各有 `MaxPendingWriteBytes` = 1 MiB；LocalShell 不用该常量：会话层
`tryEnqueueInput()` 队列上限 1 MiB，`LocalShellTransport` 在其上维护积压
（256 KiB 分块、队列满时 5 ms 重试、总量 64 MiB 才报 Overload），大粘贴
不会被整段拒绝。

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
  2026-09-14 修 `ElaAppBarPrivate::onCloseButtonClicked()` 的 use-after-free、
  2026-09-29 修 `ElaProgressBarStyle::subElementRect()` 把进度条压成 0 宽）。
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
- `libssh` **允许本地改动**（既有先例：`misc.c` 的 `USERPROFILE` 环境回退、
  `vterm_screen_get_cells()` 那类新增 API）。2026-09-29 又改了
  `sftp.c` 的 `sftp_limits_use_default()`：把服务端未通告
  `limits@openssh.com` 时的兜底单包上限从 32 KiB 提到 256 KiB，理由见
  下「SFTP 传输吞吐被 libssh 的兜底 limits 卡死」一节。**注意
  `sftp_limits()` 返回的是 `memcpy` 出来的副本**（`sftp.c:2934`），
  从调用方改它对 `sftp_read`/`sftp_write` 无效，只能改 vendored 源码。

**Ela 子项目的 `FILE(GLOB ...)` 没有 `CONFIGURE_DEPENDS`**（根工程的
`GLOB_RECURSE src/*` 有）。往 `third_party/ElaWidgetTools/` 加文件后必须显式重跑
`cmake -S . -B build`，只 `cmake --build` 不会把新文件编进去。

新增纯 C 依赖的做法：源码拷进 `third_party/`，**自己写最小 CMake target**
（范本 `third_party/libvterm-0.3.3/CMakeLists.txt`），消费方 PRIVATE 链接。
若上游自带 CMakeLists，先确认它不会 `install()` / `include(CPack)` /
`add_subdirectory(examples)` 污染主工程 —— libtelnet 就因为这个改成在根
`CMakeLists.txt` 里直接声明 target（见那一段注释）。

两个已踩过的坑：

- 根 `project()` 现已声明 `LANGUAGES CXX C`（`CMakeLists.txt:2`）。若将来
  去掉 `C`，在根作用域建 C 目标（如 libtelnet）前必须 `enable_language(C)`，
  否则报 `CMAKE_C_COMPILE_OBJECT` 未设置
- **MSVC 上不要设 `C_STANDARD 11`**：`/std:c11` 会让 MSVC 定义
  `__STDC_VERSION__ >= 199901L`，命中某些 C 库 `INLINE` 宏的 GNU 分支
  展开成 `__inline__` 而编译失败（libtelnet 就是）

许可证：项目 GPLv2+；ElaWidgetTools MIT、libvterm MIT、libtelnet public
domain、libssh LGPL-2.1、OpenSSL Apache-2.0、Clink GPL-3（仅二进制随包分发）。
无 NOTICE / THIRD_PARTY_LICENSES 汇总文件。

## 当前进度与主要缺口

P0 / P2 / P4 已完成，P1 架构边界完成（宽字符 continuation 与部分属性映射待补），
P3 与 P5 实施完成、部分平台或人工验收待做，P7 计划中。
**P6（Session/Transport）进行中**：Transport 层四种全部实现；会话编排采用
「1 TerminalView 拥有 1 TerminalSession」并已在生产，剩少量自包含项。

各阶段状态以**阶段文档自身的状态行**为准；`docs/architecture/README.md` 与
`Development_Roadmap.md` 的汇总表是同步过去的副本，若发现不一致以阶段文档为真。

最重要的一条：**`SessionManager` 已删除，不要再接入**。原设想的
「SessionManager 拥有 Session、View 非 owning attach、Session 后台存活」已放弃
（见 `docs/ARCHITECTURE.md` 会话一节与 `src/ui/terminal/TerminalView.h:52`）。
实际是 `TerminalPage` 每个 Tab `new TerminalView`、由 View 自建并持有 Session，
会话集合由 `TerminalPage::_terminalViews`（`TerminalPage.h:110`）代表；MCP 通过
非 owning 的 `SessionDirectory` 访问。`SessionFactory` 仍在 `src/session/`，
但 `src/` 生产代码中无调用方（2026-10-05 grep 复核）。

其余缺口与按依赖排序的剩余工作见
`docs/architecture/stages/P6_Session_and_Transport.md` 的"实现进度"与
"剩余工作"两节，不在此重复。

## 容易写错的地方

**串口 LF 自动回车只属于解析状态**：`SerialConfig::lfImpliesCr` 默认 false，
旧历史字段缺失也按 false；SSH/PTTY 等非串口传输默认 false。不要在
`SerialTransport` 或 `SessionInputPump` 改写接收字节，也不要借用 ANSI LNM
（会改变 Enter 编码）。独立状态位在 vendored libvterm 的 LF 控制字符分支，
经 `VTAdapter`、`TerminalCore` 的 Parser 命令设置。解析 Worker 的单次 take
可能跨越命令字节屏障，必须在屏障处切分后执行模式命令；模式命令队列拒绝
时不可继续附着传输。串口编辑与历史恢复要同步 `transport.lfImpliesCr`；
回归见 `TerminalCoreTests` 的 `lfImpliesCrChangesOnlyLfCursorBehavior`、
`lfModeSwitchHonorsByteBarrierInsideBatch`、`lfModeCommandReportsFullQueue`。


**Ninja/MSVC 必须识别头文件依赖**：中文 `/showIncludes` 的代码页可能与 Ninja
规则里的 UTF-8 前缀不同，导致 `ninja -t deps <obj>` 显示 `#deps 0`，头文件改动
后继续复用旧对象，产生 ABI 混用和假回归。根 CMake 对 Ninja/MSVC 已固定
`scripts/msvc-deps-launcher.cmake.in` 编译输出兼容层及 ASCII 前缀；不要移除。
它先保存编译器原始字节，由 `scripts/normalize-msvc-output.ps1` 优先按 UTF-8、
失败时按系统代码页解码，再规范化依赖行。MSVC 路径选项统一用正斜杠，防止
`/Fd` 目录末尾反斜杠吞掉 CMake 列表中的下一个参数；`/D` 宏定义转义保持原样。
仅设置 `VSLANG=1033` 不够，本机只有 2052 中文编译器资源，仍会输出中文前缀。
已有错误依赖记录的构建目录必须干净重建一次，仅重新配置不够。

**`ssh_loopback_check.py` 的 SFTP 用例在本机 Linux 失败（既有，非回归）**：
2026-09-29 实测报 `SCRIPT_WRITE_FAILED … SFTP status 8 (Operation unsupported)`，
四个 loopback 命令用例与交互式 `pwd` 均 PASS，只有 SFTP 写入一项 FAIL。已用
`git stash` 回到改动前的 HEAD 复测，结果完全相同，故与本轮改动无关。根因未查明，
疑为 Paramiko `SFTPServer` 夹具未实现 NovaTerm 写入路径用到的某个操作，
**修夹具前不要把它当成功项**。

**P8 测试不连接用户服务器**：`novaterm_mcp_tests` 使用内存凭据和临时目录；
可选 `tests/mcp/interop_check.py` 使用官方 2025 SDK 并检查 2026 MRTR wire，
`ssh_loopback_check.py` 的测试端只监听回环，返回固定命令结果、模拟交互 `pwd` 的
回显/输出/结束标记，并仅将测试脚本写到夹具目录；不执行真实命令或上传的脚本。
客户端关闭环境 SSH 配置加载。`performance_check.py`
同时报告成功读取和 Busy，不能把被拒绝请求的低延迟当作捕获性能达标。

**Windows 状态目录的属主校验不能只比对 TokenUser**：`ownerOnly()`
（`src/mcp/McpProtocol.cpp`）在收紧 MCP 状态目录/实例目录前要求对象属于本进程。
提权运行（以及 UAC 提权令牌、部分沙箱宿主）创建的对象属主是令牌的**默认属主**，
即 `BUILTIN\Administrators`，而不是 TokenUser，所以“属主 == TokenUser”这条判据
在提权运行里恒为假：`AccessStore::save()` 直接失败，`setEnabled(true)` 回滚，
设置界面的“启用本机 MCP 接入”表现为点不动（只弹一句通用失败提示）。
判据必须覆盖整个对象属主身份集合：`TokenUser`、`TokenOwner` 与令牌组里带
`SE_GROUP_OWNER` 的组（Windows 用它标记“本令牌创建对象的默认属主”，普通用户的
非提权令牌上就是 Administrators）。不要改成硬编码某个 SID，也不要退回只看
TokenUser。回归：`novaterm_mcp_tests::stateDirectoryOwnedByTokenDefaultOwnerIsSecurable`
（提权/有 ACL 权限的环境下，改回单比对 TokenUser 会让 MCP 套件里 12 个用例报
`fixture.enable(true) returned FALSE`）。

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

**鼠标上报的坐标是屏幕行，不是 `widgetToCell` 的文档行**：终端鼠标协议
（X10/SGR 1006）上报的是可见屏幕坐标；`widgetToCell()` 会随回看滚动偏移
减去 `_scrollLine`，回看中点击会把坐标报成历史文档行。鼠标事件一律用
`widgetToScreenCell()`。另外 libvterm 的 `vterm_mouse_button()` **不携带
坐标**，位置只能通过 `vterm_mouse_move()` 设置 —— `TerminalCore` 的
MouseButton 命令因此自带 row/col，执行时先更新位置再发按键。滚轮路由
（Ctrl 缩放 → Shift 强制本地回看 → 鼠标跟踪发按键 4/5 → 备用屏
Alternate Scroll 发 ↑/↓ → 本地 scrollback）在 `TerminalRenderer::wheelEvent`，
判定所需的 `VTERM_PROP_MOUSE` 与备用屏状态由 VTAdapter 经 observer 缓存到
Runtime 原子变量（`mouseTrackingMode()`/`isAlternateScreen()`），GUI 线程
同步读取、不拿模型锁。**滚轮没有配对 release**：`processWheel` 只发一次
press（按键 4/5），SGR 模式下多发 `'m'` 结尾的 release 会让部分 TUI 误判
为点击释放。**左键手势在 VT 鼠标跟踪开启时归应用**（开启鼠标模式的 TUI
自带选中语义），本地选区让位，Shift+左键是本地选区的逃生口；手势归属在
**按下时刻**判定并贯穿始终（`_activeGestureIsVtMouse`），中途应用开关跟踪
模式不能切换半途手势的归属 —— Qt 把双击的第二次按下投递为 doubleClick 事件
而非 press，跟踪开启时须在 `mouseDoubleClickEvent` 里转发，否则应用收不到
双击。回归：`novaterm_core_tests` 的
`mouseTrackingModeFollowsParserState`、`wheelReportsMouseButtonsWithCellCoordinates`、
`mousePressCarriesCellCoordinates`、`mouseMoveReportsPositionInMoveMode`、
`alternateScrollSendsCursorKeys`，以及
`novaterm_renderer_tests::vtMouseTrackingClaimsLeftButtonGesture`。

**OSC 52 剪贴板与 tmux passthrough（通用机制，勿按具体应用适配）**：
TUI 应用（vim/nvim 的 osc52 provider、opencode、claude code、sshclip……）
把选中写入系统剪贴板的唯一通路是发 `ESC]52;c;<base64>`，**解码与落地是
终端的义务**——不支持的终端表现为"应用提示已复制、剪贴板却是空的"。
NovaTerm 的实现全在协议层：libvterm 已内置 OSC 52 解析（`state.c` 的
`on_osc` case 52 + `vterm_state_set_selection_callbacks`，base64 解码、
分片、`?` 查询），VTAdapter 构造时注册 `VTermSelectionCallbacks`
（回调与 512 KiB 解码缓冲与 Impl 同寿命，libvterm 只存指针），
`selectionSet` 经 observer 由 TerminalCore 切 GUI 线程写 `QClipboard`
（c/s/cut buffer → 系统剪贴板，p → X11 选区）并发
`clipboardWriteRequested` 信号供测试观察。**OSC 52 读取查询默认不应答**
（`terminal.osc52ClipboardRead` 配置，默认 false）：应答等于允许远端程序
读走本机剪贴板，主流终端同样默认拒绝（Windows Terminal 不实现读取、
kitty 需确认）；粘贴进远端应用走终端自身的 Ctrl+Shift+V / 括号粘贴。
**tmux DCS passthrough 必须在 `VTAdapter::feedWithPassthrough` 预扫描解开**
（`ESC P tmux ; <ESC 加倍载荷> ESC \`）：应用在 tmux 内检测 `$TMUX` 后会
自包裹，而 libvterm 的 parser 在字符串态遇到 `ESC ESC` + 非 `\` 字节会
**中止 DCS 并把后续字节当正文打印**（parser.c 的 abort 分支），不能指望
libvterm 自己解开。扫描状态跨 `writeInput` 分片保持，内层解开点就地回喂
保证与后续字节有序，支持嵌套（递归深度上限 8）。回归：
`osc52WriteDecodesAndEmitsClipboardSignal`、`osc52QueryIsNotAnsweredByDefault`、
`tmuxPassthroughOsc52ReachesClipboard`、`tmuxPassthroughSurvivesFragmentedInput`、
`tmuxPassthroughProbeMismatchPassesBytesThrough`、
`tmuxPassthroughSplitInsidePayloadReachesClipboard`、
`tmuxPassthroughSplitAtEveryBoundaryIsByteExact`、
`tmuxPassthroughSplitAtEscapeBoundaryDoesNotDuplicateBytes`。

**预扫描器的 `runStart` 不变式：状态机消费掉第 i 字节就必须推进到 i+1**。
2026-10-05 修掉一个真实的跨分片缺陷：`feedWithPassthrough` 把扫描状态
（`passthroughScan`/`scanProbe`/`passthroughBody`）放在 `Impl` 上跨调用保持，
但"扣住不发"的游标 `runStart` 原是**每次调用的局部变量**，只在部分分支推进。
后果是分片末尾的 `flushRun(data.size())` 会把已扣在探针/载荷里的字节重复喂给
libvterm，或把已写过的前缀再写一遍。两个症状：
分片落在 Payload 态（`ESC P tmux;` 与载荷之间）时，magic 被喂给 libvterm 使其
停在 DCS 字符串态，内层 OSC 52 **静默失效**；输入以 `ESC` 结尾时前缀被写两遍
（`"A\x1b"` → 屏幕出现 `AA`）。正确不变式：`runStart` 恒等于 data 中"尚未被状态机
消费"的第一个字节下标——`Normal` 态的非 ESC 字节不被消费故原地不动，其余每个
分支（转入 `Esc`、转入 `Probe`、`Probe` push、载荷 push、ST 终止、mismatch）
都必须推进。

**该缺陷之所以长期未被测出：`tmuxPassthroughSurvivesFragmentedInput` 是假覆盖**。
它在 `TerminalCore::writeInput` 层逐字节喂，但 parser worker 用一次 `take()` 取走
队列里全部待处理字节，再按命令 `byteBarrier` 分段调 `adapter->writeInput()`——
27 次单字节写会被合并成一个 64 KiB 批次，**分片扫描器从未被真正触发**。
要测跨分片行为必须在 `VTAdapter` 层直接驱动（`novaterm_core_tests` 直连
`novaterm_core`，可构造 `VTAdapter` + `ScreenBuffer` + `ScrollbackBuffer`）。
三个新用例都这么做，其中 `tmuxPassthroughSplitAtEveryBoundaryIsByteExact` 用
**穷举两段切分点 + 逐字节**并断言"分片与整块输入的观察结果完全相同"——
这是比逐条断言屏幕文本更强、也更贴近本质（预扫描器对非 tmux 输入必须**字节透明**）
的性质。变异验证：把 `runStart` 的推进改回旧代码，三个新用例全部失败，
旧三个仍全绿。

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

**Windows 沙箱中的 libssh home 查询需要环境回退**：libssh 0.12 的
`SHGetSpecialFolderPathA(CSIDL_PROFILE)` 在部分无桌面/受限宿主里会失败；默认 SSH
identity path 在 `ssh_options_apply()` 展开时因此报 `Cannot expand homedir`。当前
`third_party/libssh-0.12.2/src/misc.c` 在 API 失败后回退到 `USERPROFILE`，再回退到
`HOMEDRIVE+HOMEPATH`。回环 SSH/SFTP 测试依赖该路径可用，勿把回退移除；它不启用用户
SSH 配置加载，也不读取私钥内容。
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

**不要把 `QWidget::style()` 强转成 Ela 样式**：父窗口有 QSS 时，该接口返回
`QStyleSheetStyle` 包装对象，不是控件创建的 `Ela*Style`。2026-10-03 的 core
显示 `ElaScrollBar::setScrollBarExtent(16)` 强转后，把滑块宽度 `3.84` 写进包装
对象的指针字段，随后 `QTabWidget::addTab()` 换父控件触发样式刷新并 SIGSEGV。
库内须保存创建时的真实样式指针（`ElaScrollBarPrivate::_scrollBarStyle` 为范例），
不要通过 `style()` 的返回值访问派生类字段。回归在 UI 测试的
`verifyTerminalScrollBarWithParentStyleSheet()`；可加 `--terminal-scrollbar-only`
单独运行，覆盖父级 QSS、换父控件、主题样式变更及反复析构。

**`setFrameShape(QFrame::NoFrame)` 关不掉 item view 的外框**：Qt 无条件向 style
派发 `CE_ShapedFrame`，`NoFrame` 只让 `frameWidth` 归零，而 Ela 的树样式在该元素
里硬画圆角边线 + `BasicBaseAlpha` 底色（`ElaTreeViewStyle.cpp:127-139`）。控件已
嵌在卡片内时那圈边框是多余的，且底色会盖掉透明效果 —— 用
`ElaTreeWidget::setIsFrameVisible(false)`。实测：`_diskTree` 未关时边框 `#363636`
／内部 `#252525` 对面板 `#272727` 明显突出，关掉后三者一致。

**`QAbstractItemView` 多选的三处隐式行为，动选择模型前必读**（`SftpPanel` 的文件
列表已用 `ExtendedSelection` 踩过一遍，Ctrl/Shift 组合完全由 Qt 提供、不要自绘）：

- **`takeTopLevelItem()` 往返会清空选区**：`QItemSelectionModel` 经
  `rowsAboutToBeRemoved` 把自己摘掉，`insertTopLevelItems()` 插回来也不会恢复。
  `SftpPanel::sortFileTree()` 因此要在取出前按 `RemotePathRole` 记下选区、
  插回后逐条 `setSelected(true)` —— 否则"选好几项再点表头排序"会把选区清空。
- **`setCurrentItem()` 在 `ExtendedSelection` 下等于 `ClearAndSelect`**：无修饰键
  的**右键**同样如此。所以右键菜单里必须先判 `item->isSelected()`，点在已选条目上
  就别调 `setCurrentItem`，否则多选会被右键吃掉。
- **焦点落在 view 本身而不是 viewport**：装在 `_fileTree` 上的事件过滤器能收到
  按键（装在 viewport 上也能，但 view 已经够了）。`QAbstractItemView::state()` 是
  protected，取不到行内编辑态，只能用焦点判定：编辑器是视口子控件且持有焦点，
  且 `isAncestorOf` 对自身也返回 true，必须先排除 `focus == view`。
- **补充一条事实**：`QTreeWidgetItem` 的默认 flags **不含 `ItemIsEditable`**，
  `QStyledItemDelegate::createEditor()` 因此返回空、根本进不了行内编辑。
  `SftpPanel` 的重命名走的是 `requestRemoteName()` 对话框，不是内联编辑。
  同理**密码字段不 `trim()` 是正确的**（`SessionPage.cpp` 里只有它不 trim）——
  密码可以有前后空格，trim 反而会改坏正确密码，不要当 bug"修"掉。

**带默认参数的成员函数不能直接接 Qt 新式 connect**：`void f(T* = nullptr)`
取成员指针后签名是 `void (C::*)(T*)`，与 `QAbstractButton::clicked(bool)` 推导
不出合法 `QObject::connect`（报 `makeCallableObject` 的 `enable_if` 失败）。这种
签名要么在接线处包一层 lambda，要么干脆别给槽函数加默认参数 —— 后者更省事。

**SFTP 传输层没有可调的吞吐旋钮，别去"优化"它**：块大小 `TransferChunkBytes`
是 256 KiB；libssh 的 `limits@openssh.com` 扩展在 `sftp_init()` 里**自动**协商
（`sftp.c:565`），协商不到才回落到默认的 32 KiB `max_read_length`
（`sftp.c:2895`）—— 所以"手动调大块大小"在服务端支持时就毫无作用。
通道窗口 2 MiB / maxpacket 32 KiB 是 libssh 的 `WINDOW_DEFAULT`，与 OpenSSH
客户端默认值一致，NovaTerm 未覆盖。密码套件也没钉，libssh 默认首选项是
`chacha20-poly1305@openssh.com`，OpenSSH 服务端按自己的顺序会选中它。2 核 ARMv7
设备上实测 6~8 MB/s 时，**先分清是链路（WiFi/百兆）还是设备 CPU/存储上限**：
用 `sftp` 或 `scp` 官方客户端对同一设备测一次，若量级相同就不是 NovaTerm 的问题。

**进度条是上传/下载共用的，不要再加第二条**：成员名一律 `transfer*` 前缀
（`_transferProgressBar` / `_activeTransferSize` / `startTransferProgress`），
延迟显示的判据走 `isTransferActive()`，它同时看两个方向的活动条目名 —— 批次收尾时
`_uploadBatchActive` / `_downloadBatchActive` 还没清，只看标志位会让空档期误显示。
`SizeRole` 存的是原始字节数（Size 列是 `QLocale` 格式化后的文本，进度条不能
拿它算比例）；服务端 `fstat` 返回 0 时比例算不出来，`updateTransferProgress()`
退化为 `setRange(0, 0)` 的忙碌指示器，而不是把条钉死在 0%。

**`QObject::disconnect` 的两种"全断"写法都是静默无效的**（2026-09-30 踩坑）：
成员函数里不加限定写 `disconnect(view, nullptr, this, nullptr)`，成员重载隐含
"发送者是 this"，语义与"断开 view 的一切"不同；而想图省事写
`QObject::disconnect(nullptr, nullptr, this, nullptr)`（"断开所有以 this 为接收者
的连接"），本 Qt 版本对 **functor 连接返回 false，一条都不断**（打印返回值确认）。
要断 functor 连接只能**逐个显式列出信号**。两种错误写法症状与"完全没写"一模一样。

**`TerminalPage` 析构必须按"发送者"逐个断开，不是只断视图**（2026-09-30 崩溃
修复，core 栈顶 `QStackedWidget::indexOf`）：控件销毁顺序是 `_tabWidget` 先拆
（连带其 TerminalView），而 `~TerminalView` 会调 `detachTransport()` →
`session->close()`，途中发出两处信号回到本页：

- `TerminalSession::stateChanged/connected/disconnected` → `updateTerminalTabConnectionAction()`
- `ITransport::disconnected`（`SshTransport::disconnect()` 直接发，GUI 线程非队列）
  → `&TerminalPage::emitCurrentSessionContext`

此刻 `_tabWidget` 已进析构，`indexOf()` 直接段错误；第二条则让 Qt 的
`assertObjectType` 发现"类析构已跑完"而 abort。**两者同一个病：连接活得比它引用的
东西久**（AGENTS.md 的内存审查一节已把 `SessionPanel` 右侧菜单的同类 UAF 修过一次，
这里是同一家族的第二处）。所以 `~TerminalPage` 要对每个存活视图断**视图 + 会话 +
传输层**三个来源，并保留 `_shuttingDown` 标志兜底 —— 逐个列举必然有遗漏，而拆控件
期间回到本页的任何路径都应当无事发生。`registerTerminalView` 里的
`workingDirectoryReported/RequestFailed` 只发 MainWindow 侧信号，不碰 `_tabWidget`。

**退出期崩溃的复现/验收命令**（不需要 SSH —— `TerminalPage` 会自动起一个本地
shell 就足以触发，offscreen 平台可用）：

```bash
# 修复前：Thread 1 received signal SIGSEGV，栈顶 QStackedWidget::indexOf
# 修复后：[Inferior 1 (process …) exited normally]
gdb -q -batch -ex "set pagination off" -ex "set confirm off" \
    -ex "break QApplication::exec()" -ex "run" \
    -ex "call (void)_ZN16QCoreApplication4quitEv()" -ex "continue" \
    -ex "info program" --args ./build/bin/novaterm
```

`QCoreApplication::quit()` 必须按 mangled 符号调用：Qt 库是 stripped，`call` 一个
C++ 名字报 `No symbol`；按文件偏移 `call 0x185720` 也不行，那是链接期偏移而非
运行时地址（会跳到无关地址再 SIGSEGV）。该用例**未进 CTest**：`TerminalPage` 依赖
`novaterm_core`/renderer/transport/session 整条闭包，而现有 `ui` 测试目标只链
Ela+Qt；且回归用例要等本地 shell 真正起来，依赖 PTY 时序，Linux 上已知易抖。

**SFTP 面板的三种批次（上传/删除/下载）共用一套骨架，但有一处不对称**：上传和
删除都会改动远端目录，所以摘要塞进 `_statusAfterNextDirectoryList`，等
`directoryListed` 到达时再显示（否则被 "N items" 覆盖）；**下载不改动远端目录**，
摘要必须由 `finishDownloadBatch()` 就地 `setBusy(false, ...)` 显示，走那条旁路会
一直挂着不出现。因此下载的计数由 `resetDownloadBatch()` 自己清，不在
`directoryListed` 里复位。

**本地文件是否被覆盖由 `SftpSession` 决定，不是面板**：`downloadFile` 用
`QSaveFile`（`SftpSession.cpp:1312`）写临时文件后 `commit()`，**提交时静默覆盖**
同名文件，面板拿不到任何"已存在"的反馈。多选下载无法逐条询问，所以
`queueDownloads()` 自己在下发前用 `QFileInfo::exists()` 统计并只问一次 ——
与上传批次"覆盖远端文件"的确认对称。不要指望传输层弹框。

**别用 `_activeDownloadName` 拼下载摘要**：`operationFinished` 分支会先把它清空
（正是为了让随后的 `errorOccurred` 不会误记到已完成的条目上），所以
`finishDownloadBatch()` 只能用计数拼摘要。这个"清空以防误归属"的模式与
`_activeUploadRemotePath`、`_activeDeleteName` 一致，三处都别为了好看而省掉。

**`ElaProgressBar` 曾把轨道和填充压成 0 宽（已修，别改回去）**：
`ElaProgressBarStyle::subElementRect()` 原先用"内容宽 − 标签宽"给条内文字
让位，但 QCommonStyle 系（Windows 的 `QWindowsStyle`、Linux 的 Fusion）对
`SE_ProgressBarLabel` 返回的是**整个控件矩形**，两者等宽，于是
`setWidth(0)` 让**任何百分比下都不绘制轨道与填充**，只剩那行居中文字 ——
表现和"只有百分比没有进度条"完全一样，很容易误判成调用方没启用。
诊断手法：给 `CE_ProgressBarGroove` 临时打日志看 `option->rect.width()`，
为 0 即命中此坑；注意 `QStyleOptionProgressBar` 必须手工填
`minimum/maximum/progress`，`QStyleOption::initFrom()` 不含这些字段，
否则量出来的矩形是假的。

**SFTP 传输吞吐被 libssh 的兜底 limits 卡死（已修，勿回退）**：SFTP 是
请求/响应式的，吞吐塌成 `单包上限 / RTT`。服务端**不**通告
`limits@openssh.com` 时，`sftp_init()` 落到 `sftp_limits_use_default()`，
而上游把 `max_read_length`/`max_write_length` 保守地设成 **32 KiB**。实测
192.168.10.100（buildroot，多半是 dropbear，不提供该扩展）：32 KiB ÷
4.822 ms = 6.79 MB/s，与实测 6.48 MiB/s 吻合；而同机 OpenSSH 客户端
（OpenSSH_9.6，scp 默认也走 SFTP）达 14 MB/s，因为它请求超过 32 KiB、
服务端就照发那么多 —— 差距**完全**来自这个兜底猜测，与调用方缓冲区无关
（`TransferChunkBytes` 本来就是 256 KiB，调它毫无作用）。修法是改 vendored
`libssh` 的兜底值到 256 KiB（= OpenSSH sftp-server 通告的同值，也等于
NovaTerm 自身的块大小，请求不会超过调用方缓冲区）。服务端若回包更短只是
产生短读，协议与所有客户端都能处理。

修后实测（256 MiB 下载，两次复现）：`avgBytes/call` 32764 → **261888**、
吞吐 6.48 → **9.03 / 9.00 MiB/s**（+39%）。**没到 14 MB/s，且不该再调**：
`avgMs/call` 同时从 4.822 涨到 27.7 ms，说明包一大就脱离 RTT 瓶颈、转为设备
侧按字节开销（2 核 ARMv7 的 CPU/磁盘/网络）主导 —— 剩下的差距是设备上限，
客户端侧没有旋钮。**判别是不是这个原因，看协商到的 `maxRead`**：等于 32768
即命中兜底默认值。

**排查吞吐用"块大小 ÷ RTT"先算，别猜**：SFTP 是请求/响应式的，拿到实测的
`avgBytes/call` 与 `avgMs/call` 就能定位。若两者之积接近实测吞吐，说明完全受
该式约束；此时只有两条路 —— 要么把单包上限放开（受服务端是否通告
`limits@openssh.com` 约束，见上一条），要么确认瓶颈已转移到设备侧的按字节
开销，此时客户端再调参数只会让单次调用更慢、吞吐不变。**不要再因为"看起来
还能再调"就去动块大小或窗口**：它们要么无效（被服务端 limits 覆盖），要么把
时间从别处挪过来。

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
`row >= vec.size()`、`vec.size() != rows` 直接写会触发有符号/无符号比较，
告警即构建失败、边界判断也可能出错。统一写成 `isize(vec.size())` 或 `int(vec.size())`。
注意与恒正 `constexpr` 常量的比较（如 `vec.size() > MaxLines`）GCC 不告警，属同类
隐患。第一方 C++ 目标的警告策略在根 `CMakeLists.txt:224-264`：MSVC 为
`/Wall /WX` 加一组带注释的 `/wd` 噪音类；GCC/Clang 为 `-Wall -Wextra -Wpedantic`，
GCC 另加 `-Werror`。P9 协议测试目标自用 `/W4`（`tests/filetransfer/CMakeLists.txt`）。
vendored 第三方目标保持各自策略。
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

## 全项目 review 修复轮（2026-10-05）

一次覆盖 244 个 first-party 文件（63k 行）的分类审查共产出 19 条确认缺陷 +
10 条待人工确认项。本轮修完，另有一处**改变方向的决策**需要记录在性能章节。

### 已修且带回归测试

- **tmux DCS 预扫描跨分片重复/泄漏字节**（见上节，最高优先级）
- **`SftpSession` 单文件上传改为临时文件 + rename 就位**（原先直接以 `O_TRUNC`
  打开最终路径，中途失败/被取消会把远端文件留成截断文件，源文件在 offset 0
  不可读时还会把远端**清零**）。同一纪律 `uploadRegularFile()` 与
  `uploadScriptBytes()` 早已有，只有这条路径漏了。
- **`SerialTransport` 不再丢弃 `QSerialPort` 五个线路设置的结果**。注意一个反直觉
  事实：Qt 里 `setBaudRate()` 等在**端口未打开时恒返回 true**，真正的驱动协商发生在
  `open()` 内部（Windows `SetCommState`、Linux `setTermios`+`setBaudRate`），
  因此只在 `open()` 之前检查是装饰性的。正确做法是**开端口后重设一次**，
  失败即关端口并报 `Configuration`，否则"已连接但速率被驱动悄悄改掉"
  这个静默故障仍在。
- **SFTP 多文件下载的远端文件名路径穿越**已由 `SftpSession::queueDownloads` 的
  同级校验覆盖（`collectRemoteDirectory` 早已在用）。

### 生命周期与并发

- **停止标志移出对象**：`SftpSession` 的 `WorkerControl`（`running`+`generation`）
  与 `SshTransport` 的 `_wakeup` 都改成由 worker lambda 按值捕获的 shared_ptr。
  原因同类：GUI 侧的 `wait()` 必须有上界（阻塞式 libssh 的数据阶段不受
  `SSH_OPTIONS_TIMEOUT` 约束——那只管 `ssh_connect`），一旦放弃线程，停止标志
  绝不能是对象成员，否则僵尸线程读的是已释放内存。`SshTransport` 的
  `_wakeup` 是**最后声明**的成员，也就是析构时**最先**销毁的，而僵尸 worker 仍
  把它当 libssh `ssh_event_add_fd` 的回调上下文——改成 shared_ptr 才真正消除那个 UAF。
- **主机密钥判定 `_keyDecision` 的复位必须在 `_keyMutex` 内**：锁外写会与
  "取锁前的窗口"竞争，accept/reject 可能被 worker 自己的 `-1` 覆盖 →
  对话框已关而连接在永不为假的谓词上空等；反向时序则让连接凭一个用户
  **并未为它做过的**判定继续，等于跳过主机密钥校验。
- **SSH 待定 PTY 尺寸改为单个打包原子字**（高 16 位列、低 16 位行）。此前是两个
  独立 `std::atomic<int>`，撕裂读会让远端收到"80 列配 50 行"这种既非旧尺寸
  也非新尺寸的几何，并把它记入 `appliedCols/appliedRows` 当作已应用，从而不
  自愈；启动路径还把 `_pendingCols` 连 load 两次（守卫与取值可能不同源）。
- **`TerminalCore` 的屏幕尺寸改为无锁读，且成对读取必须走 `screenSize()`**。
  起因：`columns()`/`rows()` 原先每次都取 `modelMutex`，而 worker 跨
  `adapter->writeInput()` 持有该锁最长 64 KiB（实测 24 MiB/s 下约 2.7 ms），
  渲染器每帧要问很多次 → GUI 线程每帧排若干次无界停顿。
  **关键教训：打包成单个原子字只保证「发布」原子，不保证「成对读取」原子。**
  `columns()` 与 `rows()` 各自仍是独立 load，消费者分两次调用照样会读到
  「列来自本次、行来自下次」。因此新增 `screenSize() -> std::pair<int,int>`
  作为**单次 load** 的成对访问器，并要求"列与行必须同源"的消费者用它：
  渲染器的 viewport 设置、选区端点、`widgetToCell`/`widgetToScreenCell`
  的钳位上下界、坐标合法性判定、光标绘制、选区逐行填充。选区逐行那处原先
  是每个选中行各问一次两个字段，已提到循环外。
  回归 `screenSizeStaysAConsistentPairUnderConcurrentResize`：持续输出 + 反复
  resize，断言 `screenSize()` 只返回**请求过的**组合。**注意该用例必须用
  `flushDamage()` 把每个 Resize 隔开** —— 同类型 Resize 在队列里会被合并，
  连发几千次 resize 实际只产生几十次执行，对百万次读取的命中概率约 0.006，
  变异验证会「假通过」。变异验证（退回两个独立 atomic）确认可抓住。
- **`LocalMcpServer::stop()` 遗弃已连接 `QLocalSocket`**：`nextPendingConnection()`
  返回**无 parent** 的 socket（Qt 契约要求调用方 delete），原代码只依赖
  `disconnected` 回收，于是每次"禁用再启用 MCP 接入"都泄漏 socket + 定时器 + fd。
  改为 `setParent(this)` 并在 `~IoWorker` 里 `abort()` 收尾。

### 性能

- **P0/P1 性能优化实施记录一节的三个数值需要重新解读**：见下节追加说明。

### 追加：滚动快路径此前在生产中是死代码（2026-10-05）

`RowSlotMap::update()` 那套 identity→slot 复用**没有生产调用者**：
`_conservativeLiveScrollRendering` 默认为 `true`，而 `TerminalView::startLocalShell()`
只对 `wsl.exe` 把它置 false —— 于是除 wsl.exe 外的每个 shell（bash/zsh/fish、
PowerShell/cmd）走的都是整帧重建 + `uploadAllRows` 分支。
用 xcb + opengl 实测 `novaterm_renderer_p5_gpu_benchmark`（6 秒 `steady_scroll`）：

| 路径 | rows rebuilt | contentUploadBytes | 平均 CPU 帧 |
| --- | --- | --- | --- |
| 基准脚本按原样（**它主动 opt-in 了快路径**） | 648/653/496 | 23.7/23.8/17.8 MB | ~1.0 ms p50 |
| 保守模式（= 生产实际） | 13483/12084/13004 | 512/459/494 MB | 5.52/5.39/5.08 ms |

**所以本节此前记录的三次 profile 与 P95 判据测的是一个除 wsl.exe 外没有 shell
走过的路径**；生产稳态滚动的 CPU 成本约为那些数字的 6 倍。
本轮选择了 review 给出的另一条路：**删掉死代码**（`RowSlotMap::update()`/
`RowSlotUpdate`/`VisibleRowIdentity`/`mappingOnlyUpdates`/`rowSlotsReused` 等），
保留 `rotateRowsUp()` 以便日后恢复。行为上对除 wsl.exe 外的 shell **无变化**
（它们本就走保守分支），wsl.exe 由整帧改为整帧（快路径已删）。
要恢复快路径需要三处约 15 行的改动：
`LocalShellProfile` 加 `emitsCursorPositionedRewrites` 标志、四个 profile 各自置值、
`TerminalView` 改传该标志而非比较可执行文件名。

同轮把 P5 基准里 `require(rowsP95 <= 2, …)` 换成
`require(scroll.rowsRebuilt <= 2 * framesRendered * visibleRows)`：前者守的是
已删除快路径的不变式。`rowsP95` 仍作为诊断量输出。
**这不是把验收放宽**：新判据守的是"每帧最多重建两个可见网格"，与保守模式一致。

D-018 的实测收益（`LD_PRELOAD` 计 malloc，2000ms vs 6000ms 两次运行取斜率、
除以 `steady_scroll` 帧数差，同一保守路径）：**1311.5 → 1140.8 次分配/帧（−13.0%）**、
**2091 → 1669 KiB/帧（−20.2%）**、平均 CPU 帧 5660 → 5320 µs（−6%，多次运行
区间 5261–5980 与 4447–5687，属弱信号）。剩下的 1140 次/帧大头是
`appendCellCommands` → `cellCharsToString` **每个非空白 Cell 一个 `QString`**
（约 1000 次/帧），因为 `GlyphKey::cluster` 是 `QString` —— 那是下一个收益点，
本轮未动。

## P0/P1 性能优化实施记录（2026-09-13 起）

计划文档已随实施完成删除，逐项步骤与结论以本节为准；
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
  复测的是 GPU 侧 A/B（`contentUploadBytes`、CPU frame P95）。
  **2026-09-28 更正**：下文「本机 QRhi 拿不到设备」的结论已过期。当前机器有
  Xorg + kwin + NVIDIA 卡，`novaterm --novaterm-internal-graphics-probe` 返回 0。
  但**只有 xcb 平台能拿到 RHI**：`QT_QPA_PLATFORM=offscreen` 会报
  `QRhiWidget: QRhi is not supported on this platform`，即使加
  `NOVATERM_RHI_API=opengl` 也不行。xcb + `QT_WIDGETS_RHI=1` +
  `NOVATERM_RHI_API=opengl` 正常（同机 60.03 fps，CPU 帧 P95 2.96 ms）。

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
  8/10（**当时的** Linux 注册集，仅剩上表两项本机环境失败；此后
  `novaterm_scrollback_tailfrom_scale` 被加入默认注册，见「默认注册了哪些测试」，
  今天的 Linux 全集是 11 项）；RelWithDebInfo `novaterm_core_benchmark`
  20 MiB = 24.40 MiB/s。**当时的未达标项**：perf 重录对比与 GPU 侧 A/B 在本机
  （当时无图形会话、QRhi 拿不到设备）无法执行。perf 重录后已由用户在桌面会话补齐
  （23:08 与 23:26 两次，见下两节）；图形会话自 2026-09-28 起在本机可用，
  但 offscreen 平台仍拿不到 RHI，须用 xcb（见上）；
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

## Effective C++ 全项目审查与 IRON 修复（2026-09-27）

7 个并行审查代理按 Effective C++ 铁律读完 130 个文件（~49k 行），共 58 项发现：
6 IRON / 15 STRONG / 37 GOOD。完整报告（带筛选与修复建议）在
`.claude/novaterm-cpp-review.html`（未跟踪，浏览器直接打开）。

### 已修复（本次改动，全部通过测试验证零回归）

1. **SshTransport**：`disconnect()` 忽略 `wait()` 超时后 `delete` 运行中线程（UAF）。
   现在超时改 `finished→deleteLater` 自毁 + `_workerAbandoned` 拒绝新连接；
   析构路径等待上限提到 60s（认证等阻塞调用以 ConnectTimeoutSec=10s 单步上限串联）。
2. **ConPtySession**：析构仅靠 `Q_ASSERT` 保证线程已 join（release 下 joinable
   thread 析构 = `std::terminate`）。现在防御性收尾：置停止标志 +
   `TerminateJobObject` 打断阻塞的 ReadFile/ClosePseudoConsole + join。
   同时 reader 条件变量谓词补 `_readerStopping`（原 rescue 链断裂即永久阻塞）。
3. **serialport_info**：`HDEVNOTIFY` 从不注销（每次构造泄漏）→ 成员保存 +
   析构注销；空语句 `GetLastError()` → qWarning；顺带修 Qt5 分支笔误与
   nativeEvent 判空。
4. **SessionPanel**：非模态右键菜单 lambda 捕获裸 `QTreeWidgetItem*`（树重建后
   UAF）→ 改捕获稳定 `SessionId`，`editItem`/`deleteItem` 按 ID 查找。
5. **TerminalView/TerminalPage**：`attachTransport()` 失败泄漏无 parent 的
   transport 且留野指针成员 → 返回 bool 明确"失败即未被 adopt"，本地路径
   delete 回收、三个远程路径回收并跳过 `start()`。
6. **TerminalCore**：`Runtime::adapter` 锁外 reset + 锁内解引用（启动/关闭窗口期
   空指针或 UAF）→ adapter 在构造函数（线程启动前）创建、不再提前 reset，
   生命周期与 Runtime 成员对齐。

### 顺带修复的预存构建损坏

a399ec6（2026-09-27 12:21）引入的 MSVC `/Wall /WX` 在本机从未成功构建过。
处理原则：保留 `/Wall /WX` 对真实警告严格，压掉 `/Wall` 独有噪音类
（CMakeLists.txt `/wd` 清单，每类有注释说明；C4365/C4061 等与 GCC
`-Wall -Wextra` 实际检查范围对齐）。5 处真实小警告修在源码：SshTransport
未用 `this` 捕获、SftpPanel/SshTransportFailureCheck 的 `std::array` 双大括号、
printf 非字面量格式串、SystemInformationDialog 局部变量遮蔽成员、
SettingsPage 未用参数。

**教训**：`cmake --build --target X --clean-first` 的 clean 会清掉**全部**目标
产物再只重建 X，误用一次就全量重编（本机约 10 分钟）。只想强制重编单目标，
touch 源文件即可。

### 未修（报告中有细节，按优先级）

STRONG 前几名：McpProtocol secureFile 的 lstat→chmod TOCTOU、
TerminalSession::start/beginReconnect 缺 QPointer 复查（重入崩溃）、
CredentialStore Windows remove() 语义与契约相反、SearchEngine 逐码点
QString 堆分配（热路径）、TerminalRenderer `_fm` 裸指针 delete/new ×4。

## 内存占用审查与第一档优化（2026-09-27）

6 个并行代理按模块做完内存审计（对账实测：空闲启动 RSS 168 MB，大头是
框架 + atlas GPU 纹理，不在业务代码）。完整发现：2 个真·无界容器、
1 条 ~400 MB 级未入账路径、一批虚高的容量常数与拷贝链。

### 已实施（第一档：零/低风险，全部验证）

1. **回看碎块化 ~400 MB 未入账路径**（最重要）：回看 + 持续输出时每帧
   sealActive 封存几行的小块，每块白占 reserve(1024) 的 ~40 KB 且不计入
   maxBytes。修复：sealChunk 对 size<capacity 的块 shrink_to_fit（正常
   写满路径零拷贝），estimateChunkBytes 把 lines 容量入账。
   scrollback 基准 Budget respected 由新账目验证通过。
2. **ChunkedScrollback::_retired 无界慢漏**：collectRetired 原本只有
   statistics() 调而生产无人调。retireChunk（块粒度冷路径）末尾顺手回收。
3. **GlyphCache 条目无界**：加 MaxEntries=32768 硬顶（~8 MB），超限先
   清扫死条目、仍超整表清空（FontManager _selectionCache 同款策略）。
   顺带修了 insert 复用记账 find 导致的命中率统计失真（拆出
   findEntry 不记账路径）。
4. **容量常数对齐业务**：Serial 读缓冲 8 MiB→256 KiB（串口 12 分钟才
   攒满原值）；Telnet 读缓冲 8 MiB→1 MiB；ConPty 4+8 MiB→1+4 MiB；
   PtySession 4 MiB→1 MiB。吞吐由消费端决定，不受影响（见基准）。
5. **QByteArray 容量粘滞**：SshTransport 断开时 _inbound/_writeQueue
   整体赋值归还 ~2 MiB/标签（热路径 clear 保持不动）；
   McpProtocol FrameReader 读空帧后归还 ~2 MiB/连接。
6. **classifyScript 拷贝链**：QStringDecoder 直接从 view 解码 +
   hasError() 检测，删掉 QByteArray 深拷贝与 toUtf8() 往返校验
   （单轮瞬态 ~14→~10 MiB）。
7. **RenderCommandBuffer 内容层 reserve** 4×columns→1.5×columns：
   密集装饰行经 QVector 倍增自然长到位，容量跨帧保留不形成每帧 realloc；
   典型视口省 ~4-7 MB。

### 验证记录

- 构建 OK（/Wall /WX 严格警告全过）；全套 ctest 两轮：同样 5 个本机
  既有失败，其余项全部通过，零回归（失败项与通过项数见「已知测试失败」表，
  那张表是权威来源；本条原先写的"其余 7 项"与当时的注册数也对不上）。
- P2 吞吐基准 **20.46 MiB/s ≥ 20 目标**（本机历史基线 ~19，无下降）。
- scrollback 基准：100000 行摄入 49.5 万行/s；Budget respected PASS。
- 空闲 RSS 改前 168.1 / 改后 168.4 MB——第一档不动空闲基线（诚实记录：
  空闲大头是 Qt 框架 + atlas GPU 纹理，属第二档）。

### 第二档候选（未做，按收益排序）

- atlas 纹理数组按实际页数增长（一启动顶格 64 MiB 显存 → 典型省 48 MiB）
- 灰度字形页改 R8 单通道（现 Alpha8 也按 RGBA8888 存，atlas 省 75%）
- BoundedByteQueue 8 MiB/标签 分段懒分配（每标签省 ~6 MiB，须重跑 P2）
- MCP executions 失败输出双份拷贝 + 600s 保留（病理 33 MiB）
- ResourcePrefetch 懒启动（每 SSH 标签省 10-150 KB + 3 条远端命令）
- 专项：SFTP 递归删除/下载流式化（百万文件 120-250 MB）、
  CellAttributes 位标志（Cell 52→36 B，回滚省 ~30%）
