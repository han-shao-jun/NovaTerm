# GUI 历史滚动审查修复（2026-10-21）

> 归档清理：可重跑生成的 JSON、日志及重复表格已移除；本文保留历史结论。
> 文中输出文件名用于标识重跑产物，已不在仓库中；重跑生成的是新样本。

修复 `da29c2c` 审查发现的三项问题。默认参数仍为 512 KiB 块 / 2 MiB 热区，
压缩开关仍默认 false；没有扩大缓存、队列或工作区预算。

## 实现与回归边界

1. `RendererSnapshot.historyAnchor` 携带实际绘制的首行 ID，包含锚点淘汰后的回退。
   Renderer 只在完整重建后记录该内容锚点；后续字形补帧须匹配它。同一映射发生
   淘汰回退时，稀疏帧升为完整重建；二次取快照的淘汰竞争也不能认领完成标记。
2. GUI 提前准备通过模型锁内 `prepareColdRange` 直接二分查找封存块，最多检查
   可见行数个块，只查询 encoded 块。复杂度 O(log(历史块数)+可见行数)，不复制
   全量 ChunkView、不封存活动尾块、不读 Raw Cell、不保留额外 pin 或快照。
   GUI 事件合并、原 worker/缓存预算与所有权边界保持原实现。
3. 基准跳转目标使用与区间互质的步长，并避开初始位置；常规 50000 行负载的目标
   序列不变。退化模数仍覆盖同一冷区范围，不用重复/no-op 请求冒充新内容完成。
   helper 由基准与 Renderer 单测共用，并纳入 runner 来源哈希。

新增存储回归覆盖跨块请求上限、Pending 去重、范围边界及 Raw/活动尾块保持。
GPU self-test 保留 Pending 和“增量帧先执行”检查，新增活动尾块不封存，以及
布局更新尚未投递时锚点淘汰的显式完整帧/稀疏帧两条路径。只有该身份专项预热
回退内容以隔离 Pending；正式采样没有额外预热。

RED：旧目标生成在 `benchmarkJumpTargetsAlwaysChangeViewport` 失败；旧预取在
真实 GPU 上触发 `cold preparation must not seal active history`。恢复旧完成锚点
赋值时，自测在已确认完整重建后仍触发实际回退锚点断言失败。
编译、测试及 lint 控制台日志已按[归档规则](../../../README.md)清理，结论与复现命令保留。

## 可复现命令

构建与正式采样分开；在仓库根目录运行。Qt 路径沿用本机 CMake 配置。

```sh
cmake -S . -B build/Release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake -S . -B build/Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake -S tests/history-noqt -B build/history-noqt -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/Release --parallel 4
cmake --build build/Debug --target novaterm_core_tests novaterm_scrollback_tests \
  novaterm_renderer_tests novaterm_renderer_p5_tests novaterm_history_no_qt_link_check \
  novaterm_history_gui_benchmark --parallel 4
cmake --build build/history-noqt --parallel 4
ctest --test-dir build/Release --output-on-failure
ctest --test-dir build/Debug --output-on-failure \
  -R '^novaterm_(core|scrollback|renderer|renderer_p5|history_no_qt_link_check)(_tests)?$'
ctest --test-dir build/history-noqt --output-on-failure
NOVATERM_RHI_API=opengl build/Release/bin/novaterm_history_gui_benchmark --self-test
NOVATERM_RHI_API=opengl build/Debug/bin/novaterm_history_gui_benchmark --self-test

# 原先退化为相同目标的合法参数，单独检查，不混入性能矩阵
NOVATERM_RHI_API=opengl build/Release/bin/novaterm_history_gui_benchmark \
  --profile off --rows 12960 --samples 16 --output build/history-edge-new.json

# 需已解锁的可见 X11 桌面；每次输出到新目录
NOVATERM_RHI_API=opengl /usr/bin/python3 tests/benchmarks/history/lz4/run-gui-matrix.py \
  --keep-display-on --output build/history-review-fixes-new
```

完整矩阵保持两类语料、四组策略、三次独立进程，共 24 进程；每进程 50000 行，
四场景各 128 次请求，60 Hz。没有降低负载或缩小扫描范围。默认调度，不绑定 CPU。
所有延迟截止目标内容及字形就绪后的 frameSubmitted，不含显示器呈现。
历史参照使用 `../history-gui-tuning-linux-2026-10-21/final-unpinned/matrix.json`（重跑生成），
只比较相同的 OFF 和 on-default 参数；这些非同时交错的历史对照不能证明因果收益。
本轮 on-small/on-hot 的实际参数在每进程 JSON 内单列，不与旧版本同名组混比。

## 验证与测量结果

- Release 完整构建及全套 **37/37**（160.88 s）。
- Debug Core/Scrollback/Renderer/P5/纯历史 **5/5**（22.06 s）；独立无 Qt **1/1**。
- Release/Debug 真实 OpenGL GPU self-test 均通过。
- 12960 行 / 16 请求专项通过：四场景共 64 次请求完整记账，闭环跳转 16/16 完成、4 次像素抽检通过，见 `edge-12960-off.json`（重跑生成）。
- 完整矩阵 **24/24**，12288 次请求完整记账、96 次像素抽检通过，见 `matrix/matrix.json`（重跑生成）。
  构建、CTest、GPU 自测均在正式采样前完成；采样期间没有构建或修改测量源码。
  临时显示租约已释放，无状态异常或持久设置变更，见 `matrix/display-guard.json`（重跑生成）。
- lint 新增行命中一条 PAT-12：`auto& spans` 有意修改每行脏区，不能改 const；未据此改动合法逻辑。

默认组两类冷区各轮 P95 最大 **36.03 ms**，P99 最大 **51.27 ms**，仍满足 50/100 ms。
热区 CPU/OFF 为 variable **1.038**、mixed **1.018**；对历史默认组为 **0.994/0.925**，
均未超过 10% 增幅。初始 charge 保持 **13.16/13.14 MiB**，相对 OFF 节省 **93.0%/93.2%**。
RSS 中位数为 229.73/244.93 MiB；它包含 Qt/GPU 等进程成本，不等于历史 charge。
这里只确认没有观察到门槛回退，不把历史对照中的 CPU/RSS 波动归因于这次修复。

并非所有尾延迟下降：mixed 连续最新请求 P99 中位数 **35.38→39.68 ms**，最早合并请求
**51.09→52.76 ms**，正确内容间隔 **50.85→51.37 ms**；三轮范围在下表完整保留。
普通 GPU 帧间隔与正确内容更新间隔分列，不能互相替代。默认参数保留 512 KiB/2 MiB，
本轮是审查修复验证，不重新宣称参数全局最优。

表格均为每进程分位数的三轮 **中位数 [最小值, 最大值]**，没有合并原始分位数。
下列表格保留历史汇总。归档分析脚本需要先重跑并按其输入路径准备新旧两份矩阵，
再执行以下命令生成新表格和门槛结果；它不再附带原始输入：

```sh
python3 tests/benchmarks/archive/lz4/history-gui-review-fixes-linux-2026-10-21/compare.py
```

### 默认策略前后延迟（ms）

| 语料 | 场景 | 之前 P95 | 之后 P95 | 之前 P99 | 之后 P99 |
| --- | --- | --- | --- | --- | --- |
| variable | hot_wheel | 34.30 [34.20, 34.69] | 34.15 [34.06, 34.31] | 34.86 [34.42, 34.88] | 34.45 [34.39, 34.55] |
| variable | cold_pages | 35.14 [35.13, 35.39] | 35.50 [35.37, 36.03] | 49.93 [49.87, 51.37] | 50.45 [50.34, 50.47] |
| variable | cold_scrollbar_jumps | 34.99 [34.21, 35.50] | 34.16 [34.11, 34.32] | 49.86 [49.02, 50.71] | 49.99 [49.85, 50.04] |
| variable | live_input_wheel | 33.75 [32.68, 34.22] | 33.38 [31.59, 33.70] | 35.65 [34.18, 44.40] | 35.63 [31.80, 50.09] |
| mixed | hot_wheel | 34.21 [34.12, 34.54] | 34.12 [33.84, 34.23] | 34.43 [34.43, 34.77] | 34.41 [34.07, 34.49] |
| mixed | cold_pages | 35.72 [35.10, 36.02] | 35.17 [35.17, 35.47] | 50.18 [50.05, 50.64] | 50.25 [49.97, 51.27] |
| mixed | cold_scrollbar_jumps | 33.95 [33.73, 34.90] | 34.04 [33.99, 34.08] | 34.78 [34.15, 35.40] | 34.39 [34.32, 35.06] |
| mixed | live_input_wheel | 33.65 [33.04, 34.07] | 33.79 [33.34, 33.96] | 35.38 [34.65, 37.70] | 39.68 [34.27, 48.69] |

### 完整四策略冷区复测（ms）

| 语料 | 策略 | 场景 | P95 | P99 |
| --- | --- | --- | --- | --- |
| variable | off | cold_pages | 35.22 [34.94, 35.23] | 36.35 [35.94, 36.36] |
| variable | off | cold_scrollbar_jumps | 34.07 [33.90, 34.24] | 35.03 [34.31, 36.05] |
| variable | on-default | cold_pages | 35.50 [35.37, 36.03] | 50.45 [50.34, 50.47] |
| variable | on-default | cold_scrollbar_jumps | 34.16 [34.11, 34.32] | 49.99 [49.85, 50.04] |
| variable | on-small | cold_pages | 35.45 [35.18, 35.52] | 49.92 [49.73, 49.97] |
| variable | on-small | cold_scrollbar_jumps | 34.28 [34.14, 34.31] | 49.75 [49.57, 49.78] |
| variable | on-hot | cold_pages | 35.34 [35.23, 35.74] | 50.18 [49.84, 50.76] |
| variable | on-hot | cold_scrollbar_jumps | 34.29 [34.26, 34.33] | 34.86 [34.53, 35.74] |
| mixed | off | cold_pages | 35.20 [34.58, 35.49] | 35.47 [34.98, 36.12] |
| mixed | off | cold_scrollbar_jumps | 34.03 [34.02, 34.24] | 34.25 [34.18, 34.68] |
| mixed | on-default | cold_pages | 35.17 [35.17, 35.47] | 50.25 [49.97, 51.27] |
| mixed | on-default | cold_scrollbar_jumps | 34.04 [33.99, 34.08] | 34.39 [34.32, 35.06] |
| mixed | on-small | cold_pages | 35.54 [35.16, 35.70] | 51.51 [50.86, 52.23] |
| mixed | on-small | cold_scrollbar_jumps | 34.09 [33.99, 34.24] | 34.39 [34.27, 34.58] |
| mixed | on-hot | cold_pages | 35.77 [35.47, 36.02] | 51.04 [50.44, 51.53] |
| mixed | on-hot | cold_scrollbar_jumps | 34.20 [34.16, 34.30] | 50.14 [49.71, 50.16] |

### 默认策略 CPU/内存前后对照

| 语料 | 之前热区 CPU P95 ms | 之后热区 CPU P95 ms | 之前 charge MiB | 之后 charge MiB | 之前 RSS MiB | 之后 RSS MiB |
| --- | --- | --- | --- | --- | --- | --- |
| variable | 6.07 [4.93, 6.33] | 6.03 [6.01, 6.17] | 13.16 [13.16, 13.16] | 13.16 [13.16, 13.16] | 233.41 [229.58, 233.43] | 229.73 [217.96, 238.44] |
| mixed | 5.97 [5.95, 6.01] | 5.52 [5.05, 5.56] | 13.14 [13.14, 13.14] | 13.14 [13.14, 13.14] | 254.86 [250.00, 256.76] | 244.93 [238.45, 257.04] |

### CPU 与内存

| 语料 | 策略 | 热区 Renderer CPU P95 ms | 初始 charge MiB | 进程 RSS MiB |
| --- | --- | --- | --- | --- |
| variable | off | 5.81 [5.77, 6.29] | 189.28 [189.28, 189.28] | 405.62 [405.31, 405.65] |
| variable | on-default | 6.03 [6.01, 6.17] | 13.16 [13.16, 13.16] | 229.73 [217.96, 238.44] |
| variable | on-small | 6.02 [5.95, 6.19] | 13.45 [13.45, 13.45] | 231.06 [221.45, 233.43] |
| variable | on-hot | 5.89 [5.67, 6.08] | 19.06 [19.06, 19.06] | 235.35 [219.73, 235.45] |
| mixed | off | 5.42 [4.73, 5.90] | 194.61 [194.61, 194.61] | 435.30 [430.54, 440.97] |
| mixed | on-default | 5.52 [5.05, 5.56] | 13.14 [13.14, 13.14] | 244.93 [238.45, 257.04] |
| mixed | on-small | 5.69 [5.37, 6.03] | 13.47 [13.47, 13.47] | 257.68 [257.19, 261.07] |
| mixed | on-hot | 6.01 [5.95, 6.28] | 19.07 [19.07, 19.07] | 263.03 [254.85, 263.20] |

### 连续输入滚动四种口径（P99，ms）

| 语料 | 前后 | 策略 | 最新请求 | 最早合并请求 | 正确内容帧间隔 | 普通 GPU 帧间隔 |
| --- | --- | --- | --- | --- | --- | --- |
| variable | before | off | 34.01 [33.65, 34.49] | 50.68 [49.81, 52.04] | 50.80 [34.34, 52.45] | 38.78 [34.34, 39.11] |
| variable | before | on-default | 35.65 [34.18, 44.40] | 51.67 [35.65, 61.02] | 51.37 [33.59, 51.64] | 36.08 [33.59, 44.86] |
| variable | after | off | 35.80 [34.63, 36.99] | 52.37 [51.30, 56.13] | 51.12 [50.11, 52.93] | 36.30 [35.27, 40.87] |
| variable | after | on-default | 35.63 [31.80, 50.09] | 51.26 [47.61, 52.29] | 49.96 [34.38, 51.06] | 36.14 [34.38, 37.06] |
| mixed | before | off | 37.11 [35.37, 37.74] | 53.05 [52.39, 53.77] | 51.71 [51.55, 54.92] | 38.38 [36.30, 38.38] |
| mixed | before | on-default | 35.38 [34.65, 37.70] | 51.09 [49.17, 51.59] | 50.85 [34.31, 52.19] | 35.16 [33.84, 35.49] |
| mixed | after | off | 34.21 [33.75, 38.18] | 49.51 [49.09, 54.86] | 50.89 [35.19, 51.00] | 34.64 [34.45, 39.34] |
| mixed | after | on-default | 39.68 [34.27, 48.69] | 52.76 [49.84, 56.35] | 51.37 [50.07, 51.54] | 36.49 [34.46, 40.38] |

### 默认策略门槛

| 语料 | 冷区单轮 P95 最大 | P99 最大 | 热区 CPU/OFF | 热区 CPU/之前默认 | charge 节省 |
| --- | --- | --- | --- | --- | --- |
| variable | 36.028 | 50.468 | 1.038 | 0.994 | 93.0% |
| mixed | 35.468 | 51.272 | 1.018 | 0.925 | 93.2% |

## 剩余范围

本轮仅 Linux X11/OpenGL、单会话合成日志。显示器呈现、其他平台、真实 UART、
多会话长稳、完整主窗口/Ela 动画，以及既有后端追加/扫描性能缺口仍未验收。
没有新增工作抢占取消或扩大预算；已入 worker 的过期单块任务仍可能执行至结束。
步骤 7 与压缩默认关闭状态不变。修复与归档清理纳入 0.2.78 本地提交，未推送。
