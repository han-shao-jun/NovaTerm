# Linux LZ4 历史后端性能复测（2026-10-20）

> 归档清理：可重跑生成的 JSON、日志及重复表格已移除；本文保留历史结论。
> 文中输出文件名用于标识重跑产物，已不在仓库中；重跑生成的是新样本。

测试源码与生产后端基于干净提交 `06bae544df106ef73364a6c89f64cce170d9d1e3`（0.2.76），
本轮只运行基准并记录结果，未修改生产实现、参数或默认开关。**内存收益达到本轮合成门槛，追加与扫描性能未达标，默认压缩继续关闭。**

## 环境与方法

Ubuntu 24.04.5 LTS / Linux 6.8.0-147-generic x86_64，Intel Core i7-14700K，
约 31.03 GiB 物理内存，GPU 为 NVIDIA GeForce RTX 4070 Ti（本基准不使用 GPU）。
GCC 13.3.0、Ninja、独立 Release `-O3 -DNDEBUG`，链接完整生产 `novaterm_history`；
动态依赖检查无 Qt。CPU 使用系统默认调度，未固定核心或隔离系统其他负载。
本任务采样串行进行，期间不并行编译或运行其他测试。

本机未安装 PowerShell，因此使用随报告保存的 [run-matrix.py](run-matrix.py)，
逐项复现现有 `run-matrix.ps1` 的参数、OFF/ON 轮换、校验、指标汇总和门槛判定。
每个样本独立进程，每项三次；使用同一 schema 2 C++ 可执行文件。构建后 smoke
仅检查工具可用，不计入以下 84 个样本。采样时的 JSON 包含每轮结果和中位数/min/max（已清理），
包含全部 Cell 字段及行索引校验、输入/保留哈希、P95/P99、RSS/峰值 RSS、进程 CPU、
队列、冷读样本数、构建/可执行文件/源文件/运行脚本哈希。

## 同内容：1 GiB live budget，36 样本

五类普通语料各 10000 行 × 80 Cell；long 为 100 行 × 30000 Cell。全部样本完整保留。
charge 包含实际保留历史和共享服务工作区，非 RSS；比值为各模式中位数之比。

| 语料 | OFF/ON charge MiB | 节省 | 追加调用吞吐 ON/OFF | 扫描吞吐 ON/OFF | 压缩冷读 P99 各轮最大 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| repeat | 45.05 / 11.83 | 73.7% | 0.471 | 0.202 | 0.361 |
| variable | 45.05 / 11.92 | 73.5% | 0.586 | 0.194 | 0.397 |
| mixed | 45.05 / 13.31 | 70.5% | 0.451 | 0.177 | 0.478 |
| entropy | 45.05 / 37.57 | 16.6% | 0.405 | 0.160 | 0.380 |
| long | 152.79 / 155.80 | -2.0% | 0.170 | 0.993 | 未测 |
| rewrite | 45.08 / 11.92 | 73.6% | 0.912 | 0.184 | 0.349 |

重复/变量日志 charge 均下降超过 40%。追加吞吐仅 rewrite 达到 90% 门槛；
扫描吞吐仅 long 达到 80% 门槛，普通日志扫描约为 OFF 的 16%–20%。
long 没有压缩块，不能用其扫描通过证明压缩扫描达标。`synthetic_gates_passed=false`。
追加调用中位时间约为亚毫秒至 1 ms，容易受调度/计时影响；完整 ingest 时间和 CPU 另列。
不能把追加调用比值外推为 Parser/网络接收吞吐，也不能以 ingest wall time 较短掩盖扫描代价。

| 语料 | OFF/ON RSS MiB | OFF/ON peak RSS MiB | OFF/ON ingest CPU ms | OFF/ON ingest wall ms |
| --- | ---: | ---: | ---: | ---: |
| repeat | 44.04 / 15.45 | 43.75 / 19.07 | 26.82 / 33.36 | 26.79 / 19.16 |
| variable | 44.05 / 15.14 | 43.53 / 18.41 | 28.60 / 33.41 | 28.59 / 18.99 |
| mixed | 44.02 / 19.51 | 43.53 / 22.95 | 22.73 / 44.82 | 22.71 / 27.58 |
| entropy | 44.02 / 40.19 | 43.75 / 43.22 | 40.04 / 72.08 | 40.05 / 41.10 |
| long | 152.54 / 152.59 | 152.03 / 151.81 | 78.08 / 77.78 | 78.13 / 77.83 |
| rewrite | 44.17 / 14.58 | 43.75 / 18.16 | 29.16 / 37.33 | 29.15 / 20.80 |

CPU 为整个进程 user+kernel，含语料生成、全字段哈希、追加、worker 和排空；
并非固定频率接收 CPU。RSS 是追加完成后值，peak 含生成/扫描/校验等全程峰值，
不等于纯历史净分配。详见 `same-content.json`（重跑生成）。

Linux 当前 RSS 来自 `/proc/self/statm`，peak 来自 `getrusage(RUSAGE_SELF).ru_maxrss`。
本轮部分样本出现 peak 小于当前 RSS；上表保留工具原值，没有人为调整。
这两种采样的记账一致性尚未校准，因此不能将本轮 peak 当作严格内存峰值上界。
charge 节省与内容校验不依赖这两个 OS 采样值。

## 等预算：32 MiB live budget，36 样本

普通语料输入 25000 行，long 仍为 100 行；不加节拍。范围为三轮 min–max。

| 语料 | OFF 保留行数 | ON 保留行数（范围） | 保留量 ON/OFF | 扫描 Cell 吞吐 ON/OFF |
| --- | ---: | ---: | ---: | ---: |
| repeat | 7592 | 25000（25000–25000） | 3.293 | 0.169 |
| variable | 7592 | 25000（25000–25000） | 3.293 | 0.184 |
| mixed | 7592 | 25000（25000–25000） | 3.293 | 0.171 |
| entropy | 7592 | 10084（9858–10310） | 1.328 | 0.177 |
| long | 12 | 21（21–21） | 1.750 | 0.972 |
| rewrite | 7609 | 25000（25000–25000） | 3.286 | 0.164 |

本机普通日志 ON 全部保留，OFF 约保留 7592 行。该结果与此前 Windows 突发输入
仅小幅提升的结果不同，不能据此承诺跨平台或更高速输入的保留量；基准生成/哈希期间
后台也可工作，实际压缩与追加竞速会影响预算淘汰。entropy 的 ON 保留量存在波动。
long 无压缩块，其差异包含 ON/OFF 分块粒度影响，不归功于 LZ4。
不同保留量只比较归一化 Cell 吞吐，不能直接比较总扫描时间。等预算总 charge 也不是
同内容内存收益；live budget 不包含共享工作区，total charge 可超过 32 MiB。

本组 `synthetic_gates_passed=true` **仅表示实测压缩冷读门槛通过**；
等预算组未评定同内容追加/扫描/节省门槛，不代表发布性能全部达标。
详见 `equal-budget.json`（重跑生成）。

## 节拍补充：32 MiB、每 64 行请求 sleep 2 ms，12 样本

| 语料 | OFF/ON 保留行数 | 保留量 ON/OFF | OFF/ON ingest wall ms |
| --- | ---: | ---: | ---: |
| repeat | 7592 / 25000 | 3.293 | 1017.38 / 1004.56 |
| variable | 7592 / 25000 | 3.293 | 1023.62 / 1026.56 |

两种模式完整 ingest 约 1 秒；请求 sleep 2 ms 不等于真实调度精度或 UART 速率。
本组作为额外实验单列，未替代前述尽快追加数据。
详见 `equal-budget-paced.json`（重跑生成）。

## 校验、冷读与关闭范围

- 84/84 样本逐字段和保留内容校验通过，42/42 ON 申请成功；无基准报告的存储/解码/压缩错误。
- 所有实际采到压缩块的组，各轮后端冷读 P95 ≤ 50 ms、P99 ≤ 100 ms；同内容组 P99 最大值小于 0.48 ms。
  long 的压缩冷读样本为 0，标记未测；这些指标不是 GUI frame。
- 每轮 10000 次首尾 ID/contains/rowForLineId 检查未改变解码缓存计数。
- 任务队列高水位最大 2；ON 任务拒绝计数范围 0–418，反映调度准入，不能当成丢历史行数。
- 独立后端 CTest 1/1（0.11 秒），Release `novaterm_scrollback_tailfrom_scale` 1/1（1.66 秒）。
- 保存的 idle store close / service stop 为已排空任务和释放读者后的耗时，不评定在途关闭门槛。
- 未运行真实 UART、固定速率接收 CPU、端到端吞吐、GUI 帧、Search/Reflow/取消性能、
  模型锁 P99、在途关闭、多会话 30 分钟长稳、消毒器或其他平台。未重跑无关全套。

## 复现与证据

```sh
cmake -S tests/history-noqt -B build/history-perf-linux-2026-10-20 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/history-perf-linux-2026-10-20 --target novaterm_history_benchmark novaterm_history_no_qt_link_check -j4
ctest --test-dir build/history-perf-linux-2026-10-20 --output-on-failure
python3 tests/benchmarks/archive/lz4/history-linux-2026-10-20/run-matrix.py . \
  build/history-perf-linux-2026-10-20/checks/novaterm_history_benchmark build/history-perf-linux-rerun
ctest --test-dir build/Release -C Release -R '^novaterm_scrollback_tailfrom_scale$' --output-on-failure
```

复现命令应在仓库根运行；runner 需要 Linux `/proc`、Python 3、Git 和 `lspci`。
本轮 `validation.json`（重跑生成） 汇总样本/校验/哈希检查。编译、依赖与测试
控制台日志已按[归档规则](../../../README.md)清理，验证结论和复现命令保留。原始 Windows 成绩继续保留，
本报告不将不同编译器/硬件的数值当作生产代码前后优化比较。
