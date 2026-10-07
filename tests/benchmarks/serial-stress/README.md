# Windows 串口终端压力验收

显式打开一对已连接的串口，通过当前生产代码的
`SerialTransport → SessionInputPump → InteractiveStreamFramer → TerminalCore`
接收、解析数据，同时运行真实 `TerminalRenderer` / D3D11 窗口。
夹具不运行完整 MainWindow，不覆盖 Session 重连或文件传输协议。
不注册默认 CTest，不修改串口历史、凭据或主程序配置。

## Case 矩阵

端口默认 COM1 ↔ COM2，8N1，无流控。每个方向单独运行，不是同时双向。

| 发送方式 | 波特率 | 数据量 | 窗口 | Case 数 |
| --- | --- | --- | --- | --- |
| 按 8N1 字节率定速 | 1M、3M、5M、8M | 64 KiB、1 MiB、8 MiB | 可见 | 24 |
| 按 8N1 字节率定速 | 1M、3M、5M、8M | 8 MiB | 隐藏 | 8 |
| 按 8N1 字节率定速 | 8M | 64 MiB | 可见 | 2 |
| 不限速突发 | 配置 8M，实际软件速率单独记录 | 64 MiB | 可见 | 2 |

总计 36 个正式 case。定速发送由独立线程执行，每毫秒根据累计时间计算
应发送的字节数，每次最多写 16 KiB；GUI 线程不会主动降速发送者。
虚拟驱动可能不模拟波特率，因此不能只设置波特率而不做发送节流。
64 MiB 突发刻意取消节流，结果不应描述成真实 8M UART 吞吐。

## 完整性判定

- 每帧 128 字节：`NV:`、十位序号、确定性 ASCII 载荷、CRC32/IEEE 和 CRLF。
- 接收端按字节流重组，不能把 `readyRead` 的一次回调当成一个数据帧。
- 验证全部帧、缺失／重复／乱序／损坏计数，以及完整流 SHA-256。
- 发送完成后等待接收、泵与解析队列排空，并调用 `waitForIdle()`。
- 输入泵接收／接受字节数、Core 出队字节数须与发送总量一致。
- 再从解析后的历史和活动屏幕检查最后最多 512 帧。历史限制为 10000 行，
  正常历史淘汰不计为串口丢帧；没有宣称逐帧验证全部已淘汰的 Cell。
- 任何发送／接收错误、超时、过载、剩余半帧、文本不一致或可见窗口没有渲染帧，
  都会令该 case 失败。

`--self-test` 注入分片、损坏、缺帧、重复和乱序，验证校验器能区分这些情况。
JSON 同时记录事件循环延迟、CPU 帧 P95、进程 CPU、RSS、解析队列水位与暂停次数。
进程 CPU 包含发送线程、校验器、Parser 和 Renderer，不等于 NovaTerm 主程序的
纯接收 CPU。CPU 百分比按一个逻辑核为 100% 计算。
短数据量 case 的吞吐受线程启动和 10 ms 收敛轮询影响；持续吞吐应看 8 MiB case。

## 构建与运行

先按主项目说明构建 Release 的 `novaterm_core` 和 `libvterm`。
此独立工程默认从 `build/Release/bin/` 导入这两个静态库；必须与当前源码、
Qt 和编译配置匹配。不会重新编译或修改主工程。

在 x64 MSVC 开发者命令提示符、仓库根目录下运行（需要原生 Windows Ninja）：

```bat
cmake -S tests/benchmarks/serial-stress -B build/serial-stress-native -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Programs/Qt/6.8.3/msvc2022_64
cmake --build build/serial-stress-native
powershell -NoProfile -ExecutionPolicy Bypass -File tests/benchmarks/serial-stress/run.ps1 -Smoke
powershell -NoProfile -ExecutionPolicy Bypass -File tests/benchmarks/serial-stress/run.ps1
```

本机默认 PATH 上的 CMake/Ninja 位于 MSYS 安装目录。本轮在沙箱内的编译器 ABI
探测停住，改用系统环境中的 Visual Studio 自带 CMake/Ninja 后构建通过；
使用其他受限执行环境时需先确认编译工具可以启动子进程。Visual Studio 的
Ninja 位于 `Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe`，
可通过 `-DCMAKE_MAKE_PROGRAM=<绝对路径>` 指定。

`-Smoke` 仅运行校验器自检及可见／隐藏各一次 1 秒预检。
完整矩阵默认结果写到 `build/serial-stress-results/matrix/`。
完整矩阵包含刻意超出串口速率的突发 case；本机这部分及 8M、64 MiB 定速长测
都发生丢帧，所以脚本返回非零属真实验收结果，不能掩盖失败或否定其他 case。
`-LongCases` 单独运行两个 8M、64 MiB 定速 case。
`-Diagnostics` 对比延后校验的完整终端和仅接收模式，四项 64 MiB 突发；
`-ChunkControls` 查询驱动队列并运行四项 4 KiB 分块、8 MiB 突发对照。
两种对照默认分别输出到 `controls/` 和 `block-controls/`，避免混入正式矩阵。
`-TimingControls` 运行 COM1 → COM2 的四项 8M、64 MiB 时序对照：默认计时的
仅接收、稳定计时的完整终端、稳定计时的仅接收、稳定计时且延后校验的完整终端。
`-StableRawCase` / `-StableTerminalCase` 分别单独运行后两项。
时序对照默认输出到 `timing-controls/`。
`-DriverQueueProbe` 请求并查询确认驱动 RX 队列扩大到 256 KiB，做 1 MiB 预检。
`-LargeQueueCases` 在每次打开后核对实际 RX 队列，再运行两个方向的可见／隐藏／
仅接收模式，六项 8M、64 MiB 定速、稳定计时、延后校验的长测。
两者默认输出到 `large-queue/`。如果驱动拒绝扩容或实际值小于请求，退出 7，
不发送测试数据，也不会宣称已排除驱动小队列影响。
脚本支持 `-PortA`、`-PortB`、`-QtPrefix`、`-ExecutablePath`、`-OutputDirectory`。
运行前释放两端串口；输出目录相同且文件名相同时复测会覆盖上一轮结果。

单个 case 示例（需要 Qt bin 在 PATH、Qt plugins 可用）：

```bat
build\serial-stress-native\serial_stress.exe COM1 COM2 8000000 20 visible paced build\serial-8m.json --bytes 8388608
```

定位模式：窗口参数 `raw` 只保留 SerialTransport，关闭输入泵和渲染，并在接收
结束后验证全部捕获字节；`--deferred-verify` 将完整终端的校验也延后，排除在线
CRC／序号验证对接收热路径的影响。`--tx-chunk 4096` 可改变发送分块大小，
允许 128 字节至 64 KiB，要求是 128 的整数倍。
`--probe COM1 <output.json>` 按生产串口参数打开端口，仅查询驱动队列容量，
不调用 SetupComm 改变驱动配置。
`--driver-rx-queue 262144` 则在当前接收端调用 SetupComm，并通过
GetCommProperties 再次确认实际容量；范围为 4 KiB 至 4 MiB。这是实验控制，
不修改主程序默认参数。为访问同一个端口，夹具目标定义
`NOVATERM_SERIAL_STRESS`，启用 SerialTransport 中的测试专用 friend；
生产目标不定义该宏，不增加公开 API 或改变运行行为。
`--stable-timers` 只修改测量进程的计时策略，避免 Windows 忽略计时精度请求；
会记录最大实际发送间隔、单次发送块及超过本机 4 KiB 驱动队列的写入次数。
平均限速可能在调度延迟后补发成瞬时突发，不能当作精确 UART 线速模拟。

不传 `--bytes` 时按波特率和秒数生成数据；传入时以总字节数为准，要求
128 字节整数倍，范围为 128 字节至 256 MiB。
退出码 0 表示完整性验收通过，1 为参数／自检失败，2 为渲染启动失败，
3 为接收串口打开失败，4 为报告写入失败，5 为完整性或运行错误，
6 为请求的进程计时策略设置失败。
7 表示无法核实请求的驱动 RX 队列容量。

虚拟串口结果只证明本机驱动与软件通路在该负载下的表现，不能替代真实 UART 的
电气误码、设备 FIFO、拔线、线路流控和设备端发送时序验收。

## Windows 函数热点采样

需要管理员令牌及 WPR / xperf（Windows Performance Toolkit）。独立符号构建
保留 Release 优化，重建第一方 Core 与 libvterm，提供完整的第一方 PDB：

```bat
cmake -S tests/benchmarks/serial-stress -B build/serial-stress-symbols -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Programs/Qt/6.8.3/msvc2022_64 -DNOVATERM_STRESS_PROFILE_SYMBOLS=ON
cmake --build build/serial-stress-symbols
pwsh -NoProfile -File tests/benchmarks/serial-stress/profile-windows.ps1
```

采样握手排除语料构造和离线校验；record/stop 控制和 rundown 等空闲阶段仍可能
出现在 ETL 中。采集默认两项 COM1 → COM2、8M、64 MiB，可见／隐藏，RX 队列
确认 256 KiB。记录进程、GUI、Parser 与发送线程 ID，便于按线程剔除夹具成本。
原始 ETL 可能为 GiB 级，保留在 gitignore 的 build 目录；包含系统级事件，
发布报告时只导出目标进程的统计。

xperf 的 `-o` 必须放在 `-a` 之前。函数统计示例（替换实际 ETL 名称）：

```powershell
$env:_NT_SYMBOL_PATH = "$PWD/build/serial-stress-symbols"
$env:_NT_SYMCACHE_PATH = "$PWD/build/serial-stress-profile/symcache"
xperf -i capture.etl -symbols -o visible-profile.csv -a profile -detail
xperf -i capture.etl -symbols -o visible-parser-stack.html -a stack -butterfly 1 -tid 12345 -event '.*Profile.*'
```

按 metadata 中的 PID／TID 导出可见和隐藏的 GUI／Parser／发送线程报告，使用
标准文件名 `<mode>-profile.csv`、`<mode>-<role>-stack.html` 后可运行：

```bat
python tests/benchmarks/serial-stress/analyze-windows-profile.py --capture-dir build/serial-stress-profile/captures --output build/serial-stress-profile/hotspots.json
```

本机结果与估算边界见
[Windows 热点报告](../../../docs/architecture/Performance_Serial_Hotspots_2026-10-07.md)。
