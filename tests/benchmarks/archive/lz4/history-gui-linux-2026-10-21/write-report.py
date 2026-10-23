#!/usr/bin/env python3
"""Render the recorded matrices and comparison without pooling percentiles."""
import json
from pathlib import Path

root = Path(__file__).resolve().parent
before = json.loads((root / 'before-desktop/matrix.json').read_text())
after = json.loads((root / 'after/matrix.json').read_text())
comparison = json.loads((root / 'comparison.json').read_text())
cpu_control = json.loads((root / 'cpu-pcore-control/matrix.json').read_text())


def group(matrix, corpus, profile):
    return next(g for g in matrix['summaries'] if g['corpus'] == corpus and g['profile'] == profile)


def fmt(d, scale=1):
    return f"{d['median'] / scale:.2f} [{d['min'] / scale:.2f}, {d['max'] / scale:.2f}]"


def metric(g, scene, key):
    return g['scenarios'][scene][key]


names = {'hot_wheel': '热区滚轮', 'cold_pages': '冷翻页',
         'cold_scrollbar_jumps': '冷跳转', 'live_input_wheel': '持续输入滚轮'}
lines = ['# Linux LZ4 GUI 滚动优化（2026-10-21）', '',
         '本轮在现有未提交基准/统计改动上实施，不重置工作树，不修改历史样本，不创建提交或推送。',
         '生产改动为提前、合并的 CacheOnly 视口准备，以及首个缺块时的有界跨块请求。',
         '完成标记仍绑定实际全量视口重建及字形就绪；测量截止 `frameSubmitted`，不包含合成器/显示器呈现。', '',
         '## 同机完整矩阵与门槛', '',
         '前后各两语料 × 四策略 × 三独立进程，共 48 个进程。每进程 50000 行、四场景各 128 次请求，',
         '窗口 1200×800，原始 GUI/Parser/GPU 负载不减。策略、种子、保留内容与原基准相同。',
         '环境：Ubuntu 24.04 / Linux 6.8.0-147-generic，i7-14700K、约 31 GiB RAM、RTX 4070 Ti，',
         'Qt 6.8.3 / GCC 13.3.0 / Release / X11 xcb / OpenGL；123×42 Cell，DPR 1.82292，屏幕约 59.997 Hz，目标 60 Hz。',
         'OS 默认 CPU 调度，未固定核心或隔离整机其他负载；不将桌面噪声或进程间波动隐藏。',
         '所有表格为三个进程各自分位数的 **中位数 [最小值, 最大值]**，单位 ms；不合并三轮分位数。',
         f"采样墙钟时间：优化前 {before['elapsed_seconds']:.1f} 秒，优化后 {after['elapsed_seconds']:.1f} 秒。构建与采样分开，串行运行。", '',
         '| 语料 | 默认压缩冷区全部三轮 50/100 ms | 热区 CPU 新/旧 | 热区 CPU ON/OFF | 初始 charge 节省 |',
         '| --- | --- | ---: | ---: | ---: |']
for corpus, gate in comparison['gates'].items():
    lines.append(f"| {corpus} | {'通过' if gate['cold_all_runs_pass'] else '未通过'} | "
                 f"{gate['hot_renderer_cpu_after_over_before']:.3f} | {gate['hot_renderer_cpu_on_over_off']:.3f} | "
                 f"{100 * gate['initial_charge_saving']:.1f}% |")
lines += ['', 'CPU 比值以每进程热区 `frame_cpu.p95_ms` 的三轮中位数计算；同时给出相对本轮 OFF 和相对优化前默认组。',
          '**不绑核完整矩阵的 mixed ON/OFF 为 1.413，超过 10% 门槛；此项不能写成默认环境已通过。**',
          '两语料的新默认组相对旧默认组均未增加，但这不替代相对 OFF 的门槛。补充同类核心控制见下文。',
          '这是合成 GUI 工作负载的验收，不能外推固定速率真实接收 CPU、Search/Reflow 吞吐或整个压缩功能的发布状态。', '',
          '## 定位与实施', '',
          '链路为 CacheOnly → HistoryReadContext 去重/后台 worker → Parser wake → GUI historyDataReady →',
          'RenderScheduler → QRhi 完整重建。原实现首次冷读直到 render 才发生，LineLayout 遇首个缺块立即返回；',
          '跨块视口因此可能重复等待旧帧提交及 GUI 通知。阶段跟踪中的 history_ready 是 GUI 信号到达，',
          '不是 worker 解码完成时刻，不能把它前面的整个等待归因于 codec。', '',
          '单独启用跨块请求的探索样本，variable 冷跳转 P95/P99 为 51.65/66.64 ms，仍未达到 P95 门槛。',
          '原时间线该场景 128 次请求中 46 次发生多轮 Pending；仅跨块后为 3 次，最终索引预取版为 0 次；',
          '最终版 128 次冷跳转均在首次 render 前收到数据就绪。见 [pipeline-analysis.json](pipeline-analysis.json)。',
          '这些跟踪进程带有诊断记录开销，用于解释顺序，不混入最终三轮矩阵。', '',
          '- `scrollToLine` 每事件轮次至多投递一个准备回调，执行时读取最新目标，合并尚未开始的过期准备工作。',
          '- 准备回调沿用当前 HistoryLayout，用可见首尾 LineId 定位快照中的相交块，每块仅查一次 CacheOnly。',
          '  不重新折行，不保留第二份缓存或长期快照；隐藏、布局宽度不符等情况由正常绘制路径兜底。',
          '- LineLayout 首个 Pending 后仅查询剩余可见行数能够覆盖的候选块；不等待、不交付部分视口，原任务/缓存额度照常拒绝。',
          '- historyDataReady 与 RenderScheduler 仍沿用原有通知及节流，完成标记写入条件保持原样。', '',
          '没有扩大块、热区、缓存、队列或临时工作区预算。已提交 worker 的过期单块任务仍可能执行至结束；',
          '本轮只合并尚未启动的 GUI 准备，未实现按视口取消 worker。cache_misses 含重试，不等于实际解码任务数。', '',
          '审查指出早期原型在每次输入事件调用完整 viewport 折行，会放大超长 Raw 行成本；最终版改用索引定位。',
          '通知过滤原型曾一次完整负载超时、随后诊断复跑通过，原因未证实，因此已撤回过滤，保留失败日志及原型 patch。',
          '不得把该原型的单轮收益当作最终结果。', '',
          '## 完整响应矩阵', '',
          '| 语料/策略 | 场景 | 前 P95 | 后 P95 | 前 P99 | 后 P99 |',
          '| --- | --- | ---: | ---: | ---: | ---: |']
for row in comparison['rows']:
    a, b = row['before'], row['after']
    lines.append(f"| {row['corpus']}/{row['profile']} | {names[row['scene']]} | "
                 f"{fmt(a['event_to_frame.p95_ms'])} | {fmt(b['event_to_frame.p95_ms'])} | "
                 f"{fmt(a['event_to_frame.p99_ms'])} | {fmt(b['event_to_frame.p99_ms'])} |")
lines += ['', '前三项为闭环响应实验，不能把其更新速率当作连续滚动帧率上限。', '',
          '## 连续滚动：四种口径分开', '',
          '下表为持续输入滚轮场景，独立线程仍每 16.667 ms 投递并注入 16 行日志。',
          '最新请求、最早合并请求等待、正确内容更新间隔和所有 GPU 提交间隔均单列。',
          'GPU 旧画面提交不能冒充正确内容更新；合并请求不能冒充完成。', '',
          '| 语料/策略/版本 | 最新请求 P99 | 最早合并请求 P99 | 正确内容间隔 P99 | GPU 帧间隔 P99 | 合并请求数 |',
          '| --- | ---: | ---: | ---: | ---: | ---: |']
for corpus in ('variable', 'mixed'):
    for profile in ('off', 'on-default', 'on-small', 'on-hot'):
        for version, matrix in (('前', before), ('后', after)):
            g = group(matrix, corpus, profile)
            m = g['scenarios']['live_input_wheel']
            lines.append(f"| {corpus}/{profile}/{version} | " + ' | '.join(fmt(m[k]) for k in (
                'event_to_frame.p99_ms', 'oldest_coalesced_batch_to_frame.p99_ms',
                'correct_content_frame_interval.p99_ms', 'frame_interval.p99_ms', 'coalesced')) + ' |')
lines += ['', '## CPU 与内存成本', '',
          '整场景进程 CPU 包含 Parser、GUI、worker、事件循环与基准记账，不等于纯 codec CPU。',
          '冷跳转每轮都是 128 次请求，因此总 CPU 可对照；闭环墙钟变短并不代表工作负载减少。', '',
          '| 语料/策略 | 热区 Renderer CPU P95 前 | 后 | 冷跳转进程 CPU 前 | 后 |',
          '| --- | ---: | ---: | ---: | ---: |']
for corpus in ('variable', 'mixed'):
    for profile in ('off', 'on-default', 'on-small', 'on-hot'):
        a, b = group(before, corpus, profile), group(after, corpus, profile)
        lines.append(f"| {corpus}/{profile} | {fmt(metric(a, 'hot_wheel', 'frame_cpu.p95_ms'))} | "
                     f"{fmt(metric(b, 'hot_wheel', 'frame_cpu.p95_ms'))} | "
                     f"{fmt(metric(a, 'cold_scrollbar_jumps', 'process_cpu_ms'))} | "
                     f"{fmt(metric(b, 'cold_scrollbar_jumps', 'process_cpu_ms'))} |")
lines += ['', '| 语料/策略 | 初始 charge 前 MiB | 后 MiB | RSS 前 MiB | 后 MiB | peak RSS 前 MiB | 后 MiB |',
          '| --- | ---: | ---: | ---: | ---: | ---: | ---: |']
for corpus in ('variable', 'mixed'):
    for profile in ('off', 'on-default', 'on-small', 'on-hot'):
        a, b = group(before, corpus, profile)['metrics'], group(after, corpus, profile)['metrics']
        lines.append(f"| {corpus}/{profile} | " + ' | '.join(fmt(g[k], 1024 ** 2) for k in (
            'initial_resident_charge', 'rss', 'peak_rss') for g in (a, b)) + ' |')
lines += ['', 'charge 包含共享工作区与实际被保留 payload/cache 的保守记账，不等于 RSS。',
          'RSS/peak RSS 沿用原基准 Linux statm/getrusage 口径，其一致性未校准；不作为严格内存上界。',
          '原始样本另列各场景 charge、cache/pin 统计、Busy、Pending、queue high water 和拒绝数。',
          '进程累计 rejected_tasks 包含预填时的压缩队列拒绝，不能归因为冷读拒绝或数据丢失。', '',
          '### CPU 调度控制实验', '',
          '本机 i7-14700K 的 CPU 0–15 为同一 P 核集合、16–27 为 E 核，拓扑原始值见 cpu-topology.txt。',
          '为检查完整矩阵中约 4/6 ms 的热区尾耗时分组，另做 OFF/默认 × 两语料 × 三独立进程，共 12 进程。',
          '通过 taskset 将所有线程限制在 0–15；仍为原 50000 行、四场景各 128 请求及 60 Hz，不减少负载。',
          '本实验不替换不绑核完整矩阵，不修改生产程序亲和性，也不证明差异唯一来自 P/E 核调度。', '',
          '| 语料 | OFF 热区 Renderer P95 | ON 热区 Renderer P95 | ON/OFF | ≤10% |',
          '| --- | ---: | ---: | ---: | --- |']
for corpus, g in cpu_control['summary'].items():
    ratio = g['hot_cpu_on_over_off']
    lines.append(f"| {corpus} | {fmt(g['off']['hot_wheel']['frame_cpu.p95_ms'])} | "
                 f"{fmt(g['on-default']['hot_wheel']['frame_cpu.p95_ms'])} | {ratio:.3f} | "
                 f"{'通过' if ratio <= 1.1 else '未通过'} |")
lines += ['', '| 语料 | 控制样本冷翻页 P95 | P99 | 冷跳转 P95 | P99 |',
          '| --- | ---: | ---: | ---: | ---: |']
for corpus, g in cpu_control['summary'].items():
    m = g['on-default']
    lines.append(f"| {corpus} | " + ' | '.join(fmt(m[scene][key]) for scene in (
        'cold_pages', 'cold_scrollbar_jumps') for key in (
        'event_to_frame.p95_ms', 'event_to_frame.p99_ms')) + ' |')
lines += ['', '控制样本有一轮 variable 冷翻页 P95=50.08 ms、P99=74.71 ms，超过严格 50 ms P95 门槛约 0.08 ms；',
          '因此不能把本轮完整不绑核矩阵的通过外推到所有调度条件。该样本保留在范围和原始 JSON 中。',
          'Renderer 的 frame_cpu 是 QElapsedTimer 测得的渲染函数墙钟耗时，包含调度影响；',
          '整进程 CPU 为 user+kernel 时间，两者不能混为一谈。未绑核 OFF 的绝对耗时本轮也较旧样本下降，',
          '不把所有绝对 CPU 下降都归因于本次代码。亲和性控制的原始样本、范围及实际集合保存在',
          '[cpu-pcore-control/matrix.json](cpu-pcore-control/matrix.json)。', '',
          '## 验证、来源与复现', '',
          '- Release 主程序/GUI 基准/相关目标、Debug 主程序/GUI 基准/相关目标构建成功。',
          '- 最终 Release Core/Scrollback/Renderer/P5/纯历史后端 **5/5**；Debug Scrollback/Renderer/P5/纯历史后端 **4/4**。',
          '- Debug/Release 真实 GPU self-test 通过，保留 Pending 和增量帧先执行检查。',
          '  新增检查在首次冷读提交瞬间记录 handoff 次数，避免事件循环返回后的正常定时器触发造成误报。',
          '- 跨块请求回归先 RED（请求 1 块，期望 2 块）后 GREEN，验证无重复 Pending 任务、无部分视口及单行范围。',
          '- 最终只读复审未发现新的高置信问题；没有运行无关全套，没有新增默认 CTest 注册项。',
          '- 前后完整矩阵共 24576 次请求、192 次最终像素抽检；CPU 控制另有 6144 次请求、48 次抽检，均显式记账/通过。',
          '  测试范围、来源哈希复核和已知缺口另见 [validation.json](validation.json) 与 [review.md](review.md)。',
          '- 基线 [before-desktop/matrix.json](before-desktop/matrix.json)、最终 [after/matrix.json](after/matrix.json)、',
          '  [comparison.json](comparison.json) 保存所有原始请求、三轮范围及各自来源哈希。',
          '  初始额外后端来源见 [initial-extra-provenance.json](initial-extra-provenance.json)。', '',
          '```sh',
          'cmake --build build/Release --target NovaTerm novaterm_history_gui_benchmark novaterm_renderer_tests novaterm_renderer_p5_tests novaterm_scrollback_tests novaterm_history_no_qt_link_check -j4',
          "ctest --test-dir build/Release -R '^novaterm_(renderer|renderer_p5|scrollback|core)_tests$|^novaterm_history_no_qt_link_check$' --output-on-failure",
          'NOVATERM_RHI_API=opengl build/Release/bin/novaterm_history_gui_benchmark --self-test',
          '# 构建/测试结束后再采样；每次选择全新输出目录，runner 拒绝覆盖旧样本。',
          'NOVATERM_RHI_API=opengl python3 tests/benchmarks/history/lz4/run-gui-matrix.py \\',
          '  --executable build/Release/bin/novaterm_history_gui_benchmark --output build/history-gui-new-run',
          '# 可选单轮诊断，不混入正式矩阵：',
          'NOVATERM_RHI_API=opengl build/Release/bin/novaterm_history_gui_benchmark \\',
          '  --profile on-default --corpus variable --trace-pipeline --output build/history-gui-new-trace.json',
          'python3 tests/benchmarks/archive/lz4/history-gui-linux-2026-10-21/compare.py',
          'python3 tests/benchmarks/archive/lz4/history-gui-linux-2026-10-21/analyze-trace.py',
          '# 仅用于该主机拓扑的补充 CPU 对照，同样选择全新目录：',
          'NOVATERM_RHI_API=opengl taskset -c 0-15 python3 tests/benchmarks/archive/lz4/history-gui-linux-2026-10-21/run-cpu-affinity.py \\',
          '  --executable build/Release/bin/novaterm_history_gui_benchmark --output build/history-gui-new-cpu-control',
          'python3 tests/benchmarks/archive/lz4/history-gui-linux-2026-10-21/write-report.py',
          '```', '',
          '复现 before 使用 [initial-source/](initial-source/)：初始基准、runner 与原有 tracked 改动补丁。',
          '基准、runner、Renderer 及 tests/CMakeLists 的重建文本 SHA256 均匹配基线 matrix；',
          '补丁已在 HEAD 文件副本上通过 `git apply --check`，没有改动当前工作树。',
          '它包含用户原有未提交基准和统计，不是仅 checkout HEAD 即可得到。',
          '在独立 checkout 的同一 source_revision 上应用 initial-tracked.patch，再把初始基准和 runner',
          '分别复制到 tests/benchmarks/history/lz4/HistoryGuiScrollBenchmark.cpp 与 tests/benchmarks/history/lz4/run-gui-matrix.py，',
          '以同样 Release 配置构建并使用新输出目录采样。不要向当前优化工作树反向应用该补丁。',
          '本轮没有重置、stash、覆盖历史目录或保存凭据，未连接用户服务器或真实串口。', '',
          '## 剩余缺口', '',
          '- 默认不绑核 mixed 的热区 Renderer P95 相对 OFF 超过 10%；同类核心控制不能替代默认调度的稳定验收。',
          '- 显示器呈现、完整主窗口/Ela 动画、真实鼠标采样、Windows/macOS/其他 GPU 后端未测。',
          '- 缺少多会话 30 分钟长稳、真实 UART、固定接收速率 CPU、模型锁/在途关闭延迟及消毒器验收。',
          '- 本轮预取依赖已有列宽匹配的显示布局；布局尚未就绪/淘汰/改宽时保持原冷读兜底，未承诺同样延迟。',
          '- 只合并尚未启动的准备请求；worker 已开始的过期块不会抢占取消，极端快速拖动和预算压力仍需专项。',
          '- 此前后端追加/扫描的性能缺口不因 GUI 改善而关闭。默认压缩开关仍 false，步骤 7 发布状态保持未完成。', '']
(root / 'README.md').write_text('\n'.join(lines))
