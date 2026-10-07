# 撤回高频光标查询后的大文本验收（2026-10-07）

高频同步光标模式查询已撤回，只保留隐藏调度、恢复刷新和设置去重。
本机 D3D11 大文本吞吐/FPS 的三轮中位数恢复到同轮原基线范围，达到本轮约定的
95% 下限。按用户要求，仅保留最后一批统一计时条件下的三轮对照及最终通过的
功能测试记录，其他批次的报告与结果已清理。本报告不宣称所有单轮数据都优于基线。

## 1. 最终保留的改动

- Renderer 隐藏/最小化时暂停 Scheduler 和光标计时器，合并为一次恢复全屏帧，
  再次显示时同步视口。解析和历史维护继续运行。
- 字体归一化后比较，同字体不清空 atlas 或重排；相同绘制配色不重建；仅光标/
  选区颜色变化时走 overlay。首次配色仍同步核心默认色。
- 光标回到 530 ms 周期闪烁，只有 timeout 低频查询模式。
  删除 `syncCursorBlink`、模式缓存及 damage/cursorMoved/滚动/历史维护中的
  活动重置与新增模型锁查询；连续输出常亮行为一并撤回。

原有 RendererSnapshot 和 overlay-only 帧的核心读取未改动，核心与 Transport
接口没有变化，运行中光标模式切换仍从核心的实际状态读取并触发重绘。

## 2. 环境、输入与验收条件

| 项目 | 最终测量条件 |
| --- | --- |
| CPU / GPU | Intel Core i7-14700K，20 核 / 28 线程；NVIDIA RTX 4070 Ti |
| OS / 驱动 | Windows 11 专业版 build 26200；GPU 驱动 32.0.16.1714 |
| Qt / 编译器 | Qt 6.8.3；MSVC 19.51.36260.0；C++17 |
| 构建 | Ninja Release，`/O2 /Ob2 /DNDEBUG /MD` |
| 渲染 | D3D11，DPR 1.75，屏幕 59.997 Hz，目标 60 Hz |
| 窗口 / 网格 | 1152×760 逻辑像素；122×40 cells |

基线仍为 HEAD `cffba7710717204912923dcc6f104c1c8b615781` 的四个渲染文件；
当前版是撤回查询后的生产工作区代码，两版共用未修改的核心库和同一夹具。
64 KiB / 256 KiB 分块，每个可见/隐藏场景完整输入 64 MiB，预填 1,000 行，
历史保留 10,000 行。输入为同一个带编号的 128-byte 行数据集：126 字节 ASCII
正文加 CRLF，共 524,288 行，略宽于视口而发生自动换行。数据集 SHA256：

```text
b142380c51d9be58423f69568eb54cea2151ae93013f9a87c4c24db4154d2fbe
```

Producer 在 GUI 线程由 5 ms PreciseTimer 驱动，队列采样达到 4 MiB 就暂缓发送。
速率为完整 64 MiB 入队、解析并在可见场景最终渲染收敛的墙钟速率；FPS 来自
实际绘制计数。CPU 帧分位数只覆盖 render()，不覆盖函数外的 GUI 等待。

正式判据在本轮测量前写入 `rollback-results/manifest.json`：

1. 每种分块下，可见吞吐及 FPS 的三轮中位数均不得低于同轮基线的 95%。
   5% 是本轮同机测量的容差，不是跨平台性能标准。
2. 每场景入队/出队都是 67,108,864 bytes，结束队列为 0、Parser idle；
   所有可见场景最终渲染版本收敛，无 QRhi 失败或超时。
3. 当前版隐藏场景的调度请求与实际帧均为 0。
4. 两个渲染专项与 Windows 终端会话联通测试通过，主程序构建通过。

每组顺序：baseline-1 → current-1 → current-2 → baseline-2 → baseline-3 →
current-3。未删除较慢的单轮记录，以下表格按指标的三轮中位数汇总。

## 3. 最后一次统一计时对照：验收通过

两版临时夹具均调用 `SetProcessInformation`，将
`PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION` 的 StateMask 清零，
并通过 `timeBeginPeriod(1)` 请求精度；进程退出前配对 `timeEndPeriod(1)`。
每个结果均检查两个 API 成功，并新增实际 producer tick 间隔记录。
数据集、生产渲染代码、每场景字节量与其他参数不变。
该策略只作用于临时测量进程，不改变生产程序或系统电源计划。
API 行为见 [Microsoft 文档](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setprocessinformation)。

| 分块 | 指标 | 本轮基线 | 撤回后 |
| --- | --- | ---: | ---: |
| 64 KiB | 可见速率 | 12.43 MiB/s | 12.44 MiB/s |
| 64 KiB | FPS | 59.94 | 59.86 |
| 64 KiB | CPU 帧 P95 / P99 | 3.754 / 3.877 ms | 3.773 / 4.052 ms |
| 64 KiB | 帧数 / 重建行数 | 308 / 12,320 | 308 / 12,320 |
| 64 KiB | 隐藏速率 | 12.49 MiB/s | 12.49 MiB/s |
| 64 KiB | 隐藏调度 / 实际帧 | 307 / 0 | 0 / 0 |
| 256 KiB | 可见速率 | 19.39 MiB/s | 19.41 MiB/s |
| 256 KiB | FPS | 56.96 | 57.93 |
| 256 KiB | CPU 帧 P95 / P99 | 12.036 / 15.411 ms | 11.663 / 15.199 ms |
| 256 KiB | 历史 reflow 请求 | 7 | 9 |
| 256 KiB | 隐藏速率 | 20.14 MiB/s | 20.14 MiB/s |
| 256 KiB | 隐藏调度 / 实际帧 | 183 / 0 | 0 / 0 |

可见速率比率为 100.06% / 100.12%，FPS 比率为 99.87% / 101.70%。
64 KiB producer tick P50 约 5 ms；256 KiB 因 Parser/GUI 工作及夹具节流，
tick 间隔更长，不等于严格的 5 ms 发送节奏。64 KiB 仍受到约 12.5 MiB/s 的
名义发送上限限制，不能据此宣称最大解析吞吐。

256 KiB 基线/当前版均有 CPU 帧超过 16.667 ms（三轮计数中位数均为 2），
高压历史维护也仍可能 reflow；没有把它们写成零。这些现有压力场景问题不是
本轮判据要求解决的内容。

保留的最终批次共 12 个结果文件、24 个完整 64 MiB 场景，合计 1.5 GiB。
所有字节与收敛检查通过，当前版的 6 个隐藏场景均无调度/绘制请求。
GPU 执行时间、功耗、真实 RSS、其他后端、跨平台与长稳仍未测量。

## 4. 最终功能验证与审查

- `NovaTerm` 和 `novaterm_terminal_session_tests` Release 构建通过。
- CTest：`novaterm_renderer_tests`、`novaterm_renderer_p5_tests`、
  `novaterm_renderer_large_input_tests`，3/3 通过。
- Windows 实际会话联通：12 passed / 0 failed / 0 skipped。
- 验证周期闪烁不被内容通知推迟、光标模式变化请求重绘、历史回看不调度
  光标帧、隐藏/最小化恢复、相同字体及 overlay 配色更新，均通过。
- 独立只读复核确认高频模式查询/活动重置无残留，未发现新的高置信度缺陷。
- 正式夹具的两项审查问题已修复：未知可选参数返回 1，JSON 写入不完整或
  刷新失败返回 4。新增无 GPU 用例检查参数拒绝、输出文件名不误启计时、设备
  写入失败、短写以及完整 JSON 写出；旧错误路径先失败、修复后通过。

没有运行无关模块或全量 CTest，也没有连接用户服务器。

## 5. 数据、源码指纹与复现

目录：[`../../build/render-perf-20261006/rollback-results/`](../../build/render-perf-20261006/rollback-results/)。

- `manifest.json`：预先保存的验收条件及当前四个生产源码的 SHA256。
- `stable-{64,256}-{baseline,current}-{1,2,3}.json`：统一计时条件原始结果与 tick 分位数。
- `samples.csv` / `medians.csv`：最终 24 个场景及三轮中位数。
- `acceptance.json`：两种分块的两项比率判定，全部 pass。
- `renderer-ctest.log`、`terminal-session.txt`、`app-build.log`：最终通过的功能
  测试及主程序构建记录。
- 上级目录 `hardware.json`、`rollback-measured.patch`：环境及最终代码差异。
- 正式夹具为 [`../../tests/benchmarks/RendererLargeInputBenchmark.cpp`](../../tests/benchmarks/RendererLargeInputBenchmark.cpp)，
  在 `tests/CMakeLists.txt` 注册 `novaterm_renderer_large_input_benchmark`。
  Windows 且 `NOVATERM_BUILD_BENCHMARKS=ON` 时构建；硬件测量手工运行，不进入
  默认 CTest。它直接链接项目 `novaterm_core`，无需临时工程或硬编码的 `.lib` 路径。
- `migration-{64,256}-current.json`：迁移后正式目标的运行验证，各阶段完整解析
  64 MiB 并收敛，数据集 SHA256 与上述最终验收相同。原三轮验收数据保持不变。

在 MSVC x64 环境中构建正式目标，设置 Qt bin/plugins、windows 平台及 d3d11 后：

```powershell
cmake --build build/Release --target novaterm_renderer_large_input_benchmark
$env:QT_QPA_PLATFORM = 'windows'
$env:NOVATERM_RHI_API = 'd3d11'
build/Release/bin/novaterm_renderer_large_input_benchmark.exe build/Release/large-input-64.json 65536 --stable-timers
build/Release/bin/novaterm_renderer_large_input_benchmark.exe build/Release/large-input-256.json 262144 --stable-timers
```

以上命令测量当前源码；跨版本对照须使用同一夹具分别编译待比较版本。
夹具现已纳入项目源文件，原始结果和编译产物仍在 gitignore 的 build 目录。
计时控制不可用时退出码为 6，输入未完成/未收敛时退出码为 7，并保留已生成的
诊断 JSON。原始数据清理前需另行保留。
工作区改动尚未提交。
