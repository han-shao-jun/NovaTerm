# LZ4 历史后端性能基准

针对 `f662302aa964e81c4ea5b9aefda1bbe3b2f2189b` 引入的可选历史压缩，
复用生产 `novaterm_history`，不链接 Qt，不连接串口或服务器。
默认策略不变。此基准不自动注册 CTest，不使用绝对耗时作为日常单测断言。

Windows 先进入 MSVC x64 环境；Linux 可直接运行：

```sh
cmake -S tests/history-noqt -B build/history-standalone -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/history-standalone --target novaterm_history_benchmark novaterm_history_no_qt_link_check --parallel 4
ctest --test-dir build/history-standalone --output-on-failure
```

单次运行输出 schema 2 JSON，失败输出 stderr 并返回非零：

```sh
build/history-standalone/checks/novaterm_history_benchmark variable 10000 1073741824 on 0 64
```

参数顺序：`corpus rows budget off|on pace_ms read_samples`。
Windows 可执行文件加 `.exe`；多配置生成器需 `--config Release` 并按实际目录定位。
语料包括 `repeat`、`variable`、`mixed`、`entropy`、`long`、`rewrite`。
`long` 为 30000 Cell 单行，其余 80 Cell；`rewrite` 用两次追加拼成同一逻辑行，
覆盖尾行 continuation 路径，不模拟 ANSI 解析或真实光标改写。
固定种子，字段哈希包含全部 Cell 语义及行 ID、hardBreak、Cell 数；预算淘汰后
也逐行核对仍保留的内容。拒绝无效参数、ON 申请失败、冷读错误/超时、存储错误。

PowerShell 5.1/7 矩阵（每个样本独立进程，每项至少三次，轮换 OFF/ON 顺序）：

```powershell
./tests/benchmarks/history/run-matrix.ps1 -Executable build/history-standalone/checks/novaterm_history_benchmark.exe -Output build/history-same-content.json
./tests/benchmarks/history/run-matrix.ps1 -Executable build/history-standalone/checks/novaterm_history_benchmark.exe -Scenario EqualBudget -Rows 25000 -Budget 33554432 -Output build/history-equal-budget.json
```

可用 `-Corpora repeat,variable` 缩小矩阵；`-LongRows`、`-Runs`、`-ReadSamples`
调整规模，`-PaceMilliseconds 2` 追加节拍实验。保留不加节拍的数据。
脚本拒绝非 Release/旧 schema，自动校验输入一致、逐样本 retained 哈希、
同内容组的 OFF/ON 哈希；JSON 保存原始样本、环境、各指标中位数/范围、比值及
后端合成门槛结果。`-EnforceGates` 在保存报告后因门槛失败返回非零，默认只报告。
等预算场景仅展示保留量及归一化扫描吞吐，不用不同内容的总扫描时间判断退化。
CPU/GPU 元数据查询不可用时保留空值，发布报告前需补齐。

计量口径：

| 字段 | 范围 |
| --- | --- |
| `append_ms` / `batch_p95/p99_us` | 64 行一批的 append/continuation + processHistory；append_ms 另含最终 publish。生成、字段哈希、节拍不计入 |
| `append_wall_ms` / `ingest_wall_ms` | 前者含生成/哈希/节拍，后者再含 publish 和所有压缩任务完成等待 |
| `ingest_cpu_ms` | 进程 user+kernel，含 worker、生成/哈希和 settle；不是接收 CPU。Windows 短样本受 CPU 计时粒度影响 |
| `scan_ms` / `scan_cell_mib_per_s` | 冷读采样前，用 cursor 顺序读取全部保留内容并累计 chars[0]；全字段校验另计 verify_ms。吞吐按 sizeof(Cell)，不是输入字节 MiB/s |
| `cold_*` / `cold_encoded_*` | 每个被选块首次 CacheOnly 请求到 Found；后者只统计真正压缩块。样本数为 0 时分位数 0 表示未测，脚本不判通过 |
| `hot_*` | 上述同一行随后的 CacheOnly 命中，非 GUI frame CPU |
| `read_p95/p99_us` | 顺序扫描逐行取 pin 延迟，不含 Cell 遍历；冷块解压稀疏，不能代替 cold_encoded 分位数 |
| `index_ms` | 10000 次索引/contains/rowForLineId，校验 cache hit/miss 均不变 |
| `resident_charge` / `rss` | 读前服务总 charge 与追加完成后的 RSS；peak_rss 为全程峰值，包含生成批次/校验开销，不等于纯历史净分配 |
| `idle_store_close_ms` / `idle_service_stop_ms` | 读者释放且任务排空后的关闭；不证明在途任务关闭延迟 |

每个进程的分位数按最近秩 `ceil(p*N)` 计算，汇总给出这些分位数的
中位数/min/max；不把各轮分位数当成可合并的原始样本。
内存/追加/扫描比值门槛只对完整保留相同内容的组适用。
冷读门槛只对每轮都采到压缩块的组适用，并要求每轮分位数均通过。

schema 2 改变了哈希与计时口径，不能直接与 schema 1 的 `append_ms`/
`scan_ms` 做前后优化比较。本工具测存储后端；Parser、固定速率接收 CPU、
GUI 帧、Search/Reflow/取消、模型锁、在途关闭、多会话长稳及真实 UART 另验。

## 可见 GUI 历史滚动基准

`novaterm_history_gui_benchmark` 使用生产 TerminalCore/TerminalRenderer 和可见 QRhi
窗口，模拟滚轮、逐页回看、滚动条位置跳转及持续输入下回看。不连接串口或服务器。
需可用桌面/GPU；不注册默认 CTest，不能以 offscreen/Null 后端代替 GUI 成绩。

```sh
cmake --build build/Release --target novaterm_history_gui_benchmark -j4
NOVATERM_RHI_API=opengl build/Release/bin/novaterm_history_gui_benchmark --self-test
NOVATERM_RHI_API=opengl python3 tests/benchmarks/history/run-gui-matrix.py \
  --executable build/Release/bin/novaterm_history_gui_benchmark \
  --output build/history-gui-performance
```

先完成构建，再串行采样。矩阵默认为 variable/mixed 两种语料、每个参数组合三次
独立进程，每次预填 50000 行、每场景 128 次请求，四种策略依次轮换：

- `off`：关闭压缩。
- `on-default`：当前生产策略为 512 KiB 块、2 MiB 热区（2026-10-21 调参后）。
- `on-small`：256 KiB 块、2 MiB 热区，仅改变封块目标。
- `on-hot`：512 KiB 块、8 MiB 热区，仅扩大热区。

策略仅用于基准对象，不修改应用配置。单进程可传 `--profile`、`--corpus`、`--rows`、
`--samples`、`--output`，参数与实际窗口网格/刷新率/DPR 写入 JSON。
Windows 加 `.exe` 并按生成器定位目录；其他操作系统须单独运行，不引用 Linux 成绩。

计量边界与判读：

| 指标 | 含义及限制 |
| --- | --- |
| `event_to_frame` | 合成滚动请求投递至完整目标内容和字形的 `frameSubmitted`；不等于合成器/显示器呈现 |
| `event_to_handler` | GUI 队列等待；连续场景由独立线程按 16.667 ms 投递，不依赖 GUI 定时器产生输入 |
| `oldest_coalesced_batch_to_frame` | 本次更新所合并的最早请求到帧提交；与最新请求延迟一起报告 |
| `coalesced` | 中间请求被更新的目标取代；不算作已完成，不能删掉后只报告最快存活请求 |
| `correct_content_frame_interval` / `correct_content_fps` | 实际目标内容更新间隔/速率；与重复提交旧画面的 GPU 帧分开 |
| `frame_interval` / `frame_cpu` | 所有 GPU 帧提交间隔/Renderer CPU；含等待冷数据时提交旧画面的帧 |
| `*_gaps_over_16_7_ms` / `*_gaps_over_33_3_ms` | 两套间隔各自超预算的次数，分母在对应分布的 samples；不是合成器掉帧率 |
| `heartbeat_lag` | 5 ms GUI 定时器的超额等待，辅助识别事件循环阻塞 |
| `pixel_checks` / `pixel_readback` | 每阶段结束后抽查最终目标颜色标记；读回会重新绘制，单独计时且不混入阶段结果，不声称捕获了每个计时帧 |
| `process_cpu_ms` / cache/charge | 整个场景进程 CPU、缓存命中/未命中、解码 Pending、内存记账；用于评估策略成本 |

前三个场景逐请求等待目标完成并按不快于 60 Hz 的节拍发下一次，属于闭环响应实验；
它们的帧间隔受输入节拍影响，不能直接解释成持续滚动掉帧。第四场景为 60 Hz 开环
滚轮并同时注入约 960 行/秒合成日志，报告实际请求/合并/输入量、正确内容速率及尾延迟。
所有分位数按每进程最近秩计算，跨进程汇总中位数和范围，不合并分位数。

`--self-test` 在真实 GPU 上检查冷数据等待时保留旧完成标记，以及“先排增量帧、
再滚动、完整重绘尚未交付”的交错，防止统计提前认领新视口。该探针不作绝对耗时断言。

2026-10-21 增量：矩阵脚本拒绝写入非空输出目录，防止覆盖历史样本；每轮必须指定
新目录。来源哈希还包含 LineLayout、Core、历史读取/服务与 RenderScheduler 实现。
单进程可加 `--trace-pipeline`，在每场景 `pipeline_trace` 内记录请求处理、
historyDataReady、调度交付、render 起止和 frameSubmitted（每场景最多 16384 条，
超限失败）。这是 GUI 信号到达时间，不是 worker 解码完成时间；有跟踪开销，
仅用于定位顺序，不能混入不带跟踪的正式矩阵。

GPU self-test 暂缓冷数据交付以确定性触发 Pending，同时检查冷读首次提交先于
本次滚动的 GPU 帧交付。调度次数在首次冷读回调内取样，不能在事件循环返回后
读取，否则正常到期的帧定时器会产生误报。原有增量帧先执行检查仍保留。

参数调整可显式覆盖 `--block-kib`（4–1024）与 `--hot-mib`（0–8），仅限 ON
策略；OFF 拒绝这些无效覆盖。实际字节参数写入每进程 JSON，runner 会核对。
`--configurations` 可选择原四组，也可选择以下固定参数组：

- `on-reference`：512 KiB / 4 MiB，不随生产默认值变化。
- `on-block384`：384 KiB / 4 MiB。
- `on-hot2`：512 KiB / 2 MiB。
- `on-block384-hot2`：384 KiB / 2 MiB。

```sh
NOVATERM_RHI_API=opengl taskset -c 0-15 python3 tests/benchmarks/history/run-gui-matrix.py \
  --output build/history-tuning-new \
  --configurations off on-reference on-block384 on-hot2 on-block384-hot2
```

该 CPU 集合仅适用于报告中的 i7-14700K；runner 记录实际继承的亲和性，未配置时
仍由系统调度。统计按配置标签分组，底层 profile 字段仍保留原模式名。三轮对照
与最终默认调度复验见 [调参报告](../../../docs/architecture/benchmarks/history-gui-tuning-linux-2026-10-21/README.md)。

X11 长时间采样建议加 `--keep-display-on`（本机需要系统 Python 的 dbus 模块及
`xset`）。它要求桌面已解锁，通过临时 ScreenSaver/PowerManagement D-Bus 租约
抑制采样期间息屏，每秒检查桌面和 Monitor On；状态变化即中止当前子进程，
正常或异常退出都释放租约，额外状态写入 `display-guard.json`。不修改持久电源
配置，也不尝试解锁桌面。锁屏/息屏异常样本不能混入可见 GUI 成绩。
