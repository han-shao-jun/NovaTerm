# Windows 历史热区调用栈与 pin 复用（2026-10-22）

针对上一轮热区 Renderer 墙钟 P95/OFF 超过 1.10 的现象，先采样，后做单点修复，再独立交错 A/B。
没有修改压缩开关、512 KiB/2 MiB 默认参数、预算、GPU 完成判据或基准负载。此前 Windows 48 进程结果仍保留在本机 build/history-gui-windows-2026-10-22/。

## 采样范围与判断

Windows 26200 / i7-14700K / RTX 4070 Ti / Qt 6.8.3 / MSVC 19.51 / OpenGL。
独立 Release `/O2 /Ob2 /DNDEBUG /Z7`、链接 `/DEBUG:FULL`；先保存修复前 EXE/PDB，再用相同编译参数生成修复后文件。
WPR CPU 同时记录 SampledProfile、CSwitch、ReadyThread；默认调度与 P 核 0–15 对照各四进程。八项内容校验均通过，ETL Lost Events/Lost Buffers 均为 0。
WPR marker 按热区起止限定分析，不把预填充、冷区与持续输入混入；该窗口仍包含热区起始约 100 ms 的预定位及最终 readback，不能等同于逐个计时 render 的 CPU 精确分解。

| 亲和性/语料/策略 | Renderer 样本 | 重建命令子树样本 | rendererSnapshot 样本 | GUI CPU ms |
| --- | ---: | ---: | ---: | ---: |
| default-mixed-off | 152 | 78 | 19 | 671.40 |
| default-mixed-on-default | 160 | 93 | 10 | 637.64 |
| default-variable-off | 135 | 80 | 11 | 592.65 |
| default-variable-on-default | 145 | 70 | 13 | 602.16 |
| pcore-mixed-off | 159 | 90 | 19 | 678.79 |
| pcore-mixed-on-default | 174 | 85 | 20 | 658.48 |
| pcore-variable-off | 158 | 90 | 17 | 698.30 |
| pcore-variable-on-default | 203 | 88 | 21 | 763.19 |

inclusive 子树相互包含，不能相加；样本是 CPU 命中，不是函数调用次数。热区共享解码缓存 hits/misses 八项全部为 0，不能把热区差异归因于 LZ4 解压或共享解码缓存锁。
默认调度 variable 的 GUI CPU 为 592.65→602.16 ms（约 +1.6%），Renderer 样本 135→145；对应录制中的墙钟 P95 为 1.59→2.15 ms。两者计量边界不同，不支持把 P95 全部增幅视为真实计算增幅。
Ready 等待合计 25.89→33.69 ms，单段 P99 0.071→0.076 ms；两者 GUI 都在 P 核运行，未观察到 GUI 在 E 核执行。不能把这次差异简单归咎于 P/E 核迁移，也不能从全阶段 Ready 时间精确推断每帧延迟。
P 核对照仍有 CPU/上传子树波动；Qt、Windows 和驱动缺少完整私有符号，外部 Unknown 不归给项目。此前 21%–31% 墙钟比值不是稳定的第一方计算退化定量结论，调度/等待与采样扰动尚不能完全分摊。

## 有证据的小修复

`LineLayout::viewport()` 已为每条可见逻辑行取得 pin；原 `rendererSnapshot()` 又对每个折行切片调用 `lineById()`，重复索引搜索、引用计数及压缩块缓存查找。
现在把这些 pin 移入 RendererSnapshot 的既有所有权容器，按递增行 ID 遍历复用。没有新增缓存、全局状态或锁，也没有改变 Pending 早退和实际完成锚点。
回归检查先复现四个显示行/两条逻辑行的 6 次 cache hit，再验证降为 2 次；检查不同逻辑行/折行切片、短行补白、全脏/全净/混合脏行，以及 clear 后 pin 仍可读。

## 未录制的三轮交错 A/B

默认调度 0–27，两语料 × 修复前/后 × OFF/ON × 三独立进程，共 24 进程；配置顺序轮换。每进程预填 50000 行、四场景各 128 请求、60 Hz。采样期间没有编译或运行其他测试。
24/24 内容及请求记账通过，共 12288 请求、96 次最终像素检查。每组指标为三个进程分位数的中位数，未混池。

| 语料/策略 | 热区 P95 前→后 ms | 后/前 | 热区进程 CPU 前→后 ms | 冷翻页 cache hit 前→后 |
| --- | ---: | ---: | ---: | ---: |
| variable/off | 3.747→3.221 | 0.860 | 1203.1→1031.2 | 0→0 |
| variable/on-default | 3.717→2.961 | 0.796 | 953.1→1187.5 | 11372→5807 |
| mixed/off | 4.025→3.258 | 0.809 | 1156.2→875.0 | 0→0 |
| mixed/on-default | 3.871→3.749 | 0.968 | 1031.2→1109.4 | 11364→5784 |

所有热区 P95 的三轮范围：

| 语料/策略 | 修复前 min–max ms | 修复后 min–max ms |
| --- | ---: | ---: |
| variable/off | 3.683–3.867 | 2.725–3.531 |
| variable/on-default | 3.224–3.807 | 2.949–3.867 |
| mixed/off | 3.173–4.037 | 2.911–3.885 |
| mixed/on-default | 3.439–4.064 | 3.739–4.078 |

修复后沿用原门槛（不是修复前/后比值）：

| 语料 | 热区 ON/OFF | ≤1.10 | 冷区 P95/P99 中位数 ms | 单轮最大 P95/P99 ms | charge 节省 |
| --- | ---: | --- | ---: | ---: | ---: |
| variable | 0.919 | 通过 | 33.66/50.08 | 33.96/50.19 | 93.0% |
| mixed | 1.151 | 未通过 | 33.72/50.01 | 33.72/50.06 | 93.2% |

重复读取次数的减少可确定验证；三轮墙钟变化和进程 CPU 波动仍需结合范围判断，不能把全部中位数变化都归给这二十行修改，也不能删除此前失败样本。frameSubmitted 不等于显示器呈现，最终 readback 不证明每个计时帧的像素。

## 复现与产物

本机原始 ETL、marker、函数/线程 HTML、CSwitch/Ready 统计、EXE/PDB、三轮 JSON 与日志在 `build/history-gui-hotpath-2026-10-22/`，未纳入 Git。
本机采集脚本 `profile.ps1`/`capture.py`、解析 `export.py`/`scheduling.py`、A/B `ab.py` 及 `validation.json` 同目录保留；脚本只停止自己启动的 WPR，临时显示保护退出释放。
独立符号构建命令见本机 `build/configure-gui-profile.cmd`，配置关键参数如上。现有基准单进程命令保持不变：
```powershell
$env:NOVATERM_RHI_API="opengl"
build/Release/bin/novaterm_history_gui_benchmark.exe --corpus variable --profile on-default --rows 50000 --samples 128 --output build/new-sample.json
```

## 验证范围

Release Core/Scrollback/Renderer/RendererP5/History 五项专项 5/5（14.63 秒）及真实 GPU 自测通过。新回归用例全脏/全净/混合三行均通过。
相同独立优化构建目录随后加 `/fsanitize=address` 重建 Core/History/libvterm 与 Core 测试，AddressSanitizer 92/92（63.12 秒）通过；Qt DLL 不在本轮插桩范围。不含 LeakSanitizer/泄漏检测，未运行 UBSan。该 ASan 二进制未用于性能矩阵。
未跑无关全套、Linux/macOS、真实 UART、多会话长稳。未验证的范围不据本机专项推断通过。
未修改生产压缩默认启用状态。性能采样基于 0.2.78 工作树；按用户后续要求，
交付提交版本递增为 0.2.79，版本号修改后只复验构建和相关测试，不冒称重新采样。
0.2.79 主程序及相关目标构建通过，提交前五项专项再次 5/5（13.77 秒）通过。
未推送远端。
