# f662302 历史压缩性能补充（2026-10-19）

测试对象为提交 `f662302aa964e81c4ea5b9aefda1bbe3b2f2189b` 引入的生产历史后端；
工作树基于 `22d2f5866a623ad269c4579b1104045a948a8186`，只改 benchmark/文档及
benchmark 目标的构建元数据。未修改生产压缩策略或实现。

Windows 10 x64（10.0.19045）、Intel Core i7-13700H、Intel Iris Xe Graphics
（系统同时枚举 OrayIddDriver）、MSVC 19.51.36256、Ninja Release；不链接 Qt，
不使用 GPU。两组各 36 样本：六类语料 × OFF/ON × 三次独立进程，按模式轮换顺序。
最终采样串行运行，无并行构建或其他本任务测试；所有 72 样本字段/行索引校验通过。

## 相同内容 / 1 GiB live budget

除 long 为 100 行 × 30000 Cell，其余为 10000 行 × 80 Cell；全部保留。
以下为各模式中位数的比值；charge 包含共享服务工作区，非 RSS。

| 语料 | OFF charge MiB | ON charge MiB | charge 节省 | 追加调用吞吐 ON/OFF | 扫描吞吐 ON/OFF |
| --- | ---: | ---: | ---: | ---: | ---: |
| repeat | 45.05 | 11.83 | 73.7% | 0.431 | 0.126 |
| variable | 45.05 | 11.92 | 73.5% | 0.448 | 0.132 |
| mixed | 45.05 | 13.31 | 70.5% | 0.368 | 0.137 |
| entropy | 45.05 | 37.57 | 16.6% | 0.506 | 0.126 |
| long | 152.79 | 155.80 | -2.0% | 0.160 | 0.955 |
| rewrite | 45.08 | 11.92 | 73.6% | 0.623 | 0.094 |

追加只计 append/continuation、processHistory 和 publish 调用，未包含异步任务的
全部 CPU/等待；不是 Parser/网络吞吐。`ingest_wall_ms`、`ingest_cpu_ms` 另列
生成/哈希、节拍与压缩排空在内的完整进程成本。扫描在冷缓存采样前运行，使用
生产 cursor 并累计 chars[0]，全字段验证另计。完整参数、各轮分位数、中位数及
范围见 [same-content.json](same-content.json)。

重复/变量日志的 charge 节省超过 40%，但追加调用和扫描吞吐均低于 90%/80%
合成门槛，`synthetic_gates_passed=false`。本轮普通日志扫描仅约 OFF 的 13%。
这揭示后端代价，不代表真实接收路径已测或已退化同样比例。

五类普通语料每轮采到 57 个压缩块；其冷读 P99 各轮最大值均小于 4.5 ms，
通过本后端请求到 Found 的 50/100 ms 门槛。这不是 GUI frame 验收。long 超过
普通压缩块上限，压缩冷读样本为 0，标记未测，不能将 0 分位数记为通过。

## 相同 32 MiB live budget

普通语料追加 25000 行；long 仍为 100 行，预算不含独立共享工作区。

| 语料 | OFF 保留行数中位数 | ON 保留行数中位数 | ON/OFF |
| --- | ---: | ---: | ---: |
| repeat | 7592 | 7711 | 1.016 |
| variable | 7592 | 7711 | 1.016 |
| mixed | 7592 | 7711 | 1.016 |
| entropy | 7592 | 7711 | 1.016 |
| long | 12 | 21 | 1.750 |
| rewrite | 7609 | 7711 | 1.013 |

尽快批量追加会先淘汰尚未压缩的内容，最终排空压缩任务不会恢复已淘汰行；
本轮普通日志保留量提升约 1.6%，不能从最终 charge 节省推导额外可保留行数。
long 没有压缩块，其保留量差异含 ON/OFF 分块粒度的影响，不归功于 LZ4。
不同保留量使用 Cell MiB/s 对比扫描，不能比较总扫描耗时后宣称算法变快。
详见 [equal-budget.json](equal-budget.json)；其中 gates 仅覆盖实测压缩冷读，
`synthetic_gates_passed=true` 不代表完整性能门槛通过。

## 工具与验证

- Release 独立后端构建通过；`novaterm_history_no_qt_link_check` 1/1（0.17 s）。
- 七组非法参数均非零退出且 stdout 无 JSON；OFF/ON 预算淘汰后逐字段哈希通过。
- 刻意使用不足预算的 SameContent + `-EnforceGates`：先保存报告，再非零退出；
  无压缩冷读样本没有被列为已测。该负向检查不属于性能采样。
- 未增加默认 CTest 项；没有修改共用编译选项，未重复运行与此工具无关的全套测试。

复测命令和各字段口径见 [基准说明](../../../../tests/benchmarks/history/README.md)。
JSON 保存可执行文件及三个基准源文件的 SHA256，标注工作树改动；本轮汇总不使用
历史 schema 1 的计时/哈希做前后优化比值。默认开关仍为 false。

尚未验证真实 UART、固定速率接收 CPU、Parser/端到端吞吐、GUI frame、
Search/Reflow/取消、模型锁 P99、在途任务关闭延迟、多会话 30 分钟长稳及其他平台。
