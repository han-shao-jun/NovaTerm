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
