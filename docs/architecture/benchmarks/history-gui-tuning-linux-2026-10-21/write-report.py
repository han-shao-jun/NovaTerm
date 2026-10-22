#!/usr/bin/env python3
"""Render the tuning selection and final default-scheduler verification."""
import json
from pathlib import Path

root = Path(__file__).resolve().parent
trial = json.loads((root / 'three-rounds-active-display/matrix.json').read_text())
final = json.loads((root / 'final-unpinned/matrix.json').read_text())
selection = json.loads((root / 'selection.json').read_text())
validation = json.loads((root / 'validation.json').read_text())
winner = selection['winner']
parameters = selection['parameters']
block, hot = parameters.get('block_kib', 512), parameters.get('hot_mib', 4)
failures = []
for corpus, gate in validation['gates'].items():
    for field, label in (('median_latency_pass', '冷区中位数延迟'),
                         ('hot_cpu_pass', '热区 CPU/OFF'), ('memory_pass', '内存收益')):
        if not gate[field]:
            failures.append(f'{corpus} {label}')


def fmt(d, scale=1):
    return f"{d['median'] / scale:.2f} [{d['min'] / scale:.2f}, {d['max'] / scale:.2f}]"


def group(matrix, corpus, config):
    return next(g for g in matrix['summaries'] if g['corpus'] == corpus and g['profile'] == config)


def scene(g, name, key):
    return g['scenarios'][name][key]


lines = ['# 三轮 LZ4 GUI 参数调整（2026-10-21，0.2.77）', '',
         f'本批候选按预先写定的规则选择 **{block} KiB 块 / {hot} MiB 热区**（{winner}）。',
         '选择范围是本轮三个调整与固定 reference，不宣称全局最优。压缩 enabled 仍默认 false；',
         'Parser 单写、不可变快照、预算和完成标记的实际全量重建绑定保持不变。', '',
         ('默认调度中位数门槛均通过；单轮最大值和未覆盖范围仍单列。' if not failures else
          '**默认调度仍有未通过项：' + '、'.join(failures) + '。不能据同类核心结果宣布全面验收。**'), '',
         '## 设计、环境与样本', '',
         '先比较三个调整：384 KiB/4 MiB、512 KiB/2 MiB、384 KiB/2 MiB；固定 512 KiB/4 MiB 为 reference，',
         '另保留 OFF。两语料、每项三独立进程，共 30 进程，配置顺序逐轮轮换。选择规则在采样前保存于',
         '[design.md](design.md)，未看结果后调整权重。随后以默认调度对 OFF/reference/实际生产默认做 18 进程复验。',
         '每进程仍是 50000 行、四场景各 128 次请求、60 Hz；不降低负载、扫描范围或完成判据。',
         '矩阵记录实际参数，reference 显式固定，不随生产默认漂移。构建、测试与采样分开，采样串行。', '',
         'Ubuntu 24.04 / Linux 6.8.0-147-generic，i7-14700K、约 31 GiB RAM、RTX 4070 Ti，',
         'Qt 6.8.3 / GCC 13.3.0 / Release / X11 xcb / OpenGL，1200×800、123×42 Cell、DPR 1.82292，约 59.997 Hz。',
         f"比较阶段实际 CPU 集合：{trial['environment']['cpu_affinity_actual']}；最终阶段：{final['environment']['cpu_affinity_actual']}。",
         '限制 P 核集合只用于同类核心成本控制，未修改生产亲和性；默认调度结果单独保留，不能用控制样本替代。',
         '所有表格均为每进程分位数的三轮 **中位数 [最小值, 最大值]**，单位 ms；不合并各轮分位数。',
         '事件延迟截止完整目标内容/字形绘制后的 frameSubmitted，不包含显示器呈现。', '',
         '首轮在第 23 个进程遇到锁屏/息屏超时；22 个完整样本及失败记录保留在 three-rounds/，',
         '不混入本次正式选型。用户解锁后，全量重跑到 three-rounds-active-display/，',
         '使用临时 D-Bus 租约抑制息屏，每秒检查锁屏和 Monitor On。异常即中止，退出释放租约，',
         '不修改持久电源配置。本任务另有有时限的跨阶段租约，最终释放状态见 task-display-guard.json。', '',
         '## 三轮候选比较与选择', '',
         '| 配置 | 两语料中位数门槛 | 最差尾分数 ms | 最大 charge MiB | 两语料四场景进程 CPU 合计 ms |',
         '| --- | --- | ---: | ---: | ---: |']
for candidate in selection['candidates']:
    lines.append(f"| {candidate['configuration']} | {'通过' if candidate['eligible'] else '未通过'} | "
                 f"{candidate['tail_score']:.2f} | {candidate['worst_charge'] / 2**20:.2f} | {candidate['total_cpu_ms']:.1f} |")
lines += ['', '门槛要求两语料冷区 P95≤50/P99≤100 ms、热区 Renderer P95≤OFF×1.10、charge 节省≥40%，',
          '且 charge 不高于 reference。最差尾分数取两语料冷区 P99、最早合并请求等待 P99、正确内容间隔 P99 的最大值。',
          '距最佳尾分数不超过 5 ms 的合格组优先较低 charge，再比较 CPU 和尾分数。此阈值不替代显著性检验。',
          '不合格或单轮越界也保留全部样本；选择详情见 [selection.json](selection.json)。', '',
          '| 语料/配置 | 冷翻页 P95 | P99 | 冷跳转 P95 | P99 | 热区 CPU P95 | charge MiB |',
          '| --- | ---: | ---: | ---: | ---: | ---: | ---: |']
for g in trial['summaries']:
    lines.append(f"| {g['corpus']}/{g['profile']} | " + ' | '.join(fmt(scene(g, s, key)) for s in (
        'cold_pages', 'cold_scrollbar_jumps') for key in ('event_to_frame.p95_ms', 'event_to_frame.p99_ms'))
        + f" | {fmt(scene(g, 'hot_wheel', 'frame_cpu.p95_ms'))} | {fmt(g['metrics']['initial_resident_charge'], 2**20)} |")
lines += ['', '## 最终默认调度复验', '',
          f'实际应用默认值为 {block} KiB/{hot} MiB；on-default 不传覆盖参数，以验证生产策略默认值确已生效。',
          'on-reference 始终显式传 512 KiB/4 MiB。下表不与同类核心样本混合。', '',
          '| 语料/配置 | 冷翻页 P95 | P99 | 冷跳转 P95 | P99 | 热区 CPU P95 |',
          '| --- | ---: | ---: | ---: | ---: | ---: |']
for g in final['summaries']:
    lines.append(f"| {g['corpus']}/{g['profile']} | " + ' | '.join(fmt(scene(g, s, key)) for s in (
        'cold_pages', 'cold_scrollbar_jumps') for key in ('event_to_frame.p95_ms', 'event_to_frame.p99_ms'))
        + f" | {fmt(scene(g, 'hot_wheel', 'frame_cpu.p95_ms'))} |")
lines += ['', '| 语料 | 新默认热区 CPU / OFF | 新默认 / reference | 新默认 charge MiB | 相对 OFF 节省 | 冷跳转进程 CPU ms |',
          '| --- | ---: | ---: | ---: | ---: | ---: |']
for corpus in ('variable', 'mixed'):
    off, ref, new = (group(final, corpus, c) for c in ('off', 'on-reference', 'on-default'))
    cpu = scene(new, 'hot_wheel', 'frame_cpu.p95_ms')['median']
    charge = new['metrics']['initial_resident_charge']
    saving = 1 - charge['median'] / off['metrics']['initial_resident_charge']['median']
    lines.append(f"| {corpus} | {cpu / scene(off, 'hot_wheel', 'frame_cpu.p95_ms')['median']:.3f} | "
                 f"{cpu / scene(ref, 'hot_wheel', 'frame_cpu.p95_ms')['median']:.3f} | {fmt(charge, 2**20)} | "
                 f"{100 * saving:.1f}% | {fmt(scene(new, 'cold_scrollbar_jumps', 'process_cpu_ms'))} |")
lines += ['', '## 连续滚动的四种口径', '',
          '以下为最终默认调度、持续输入滚轮场景；仍每 16.667 ms 投递并注入 16 行，所有请求均记为完成或合并。',
          '不将重复提交旧画面的 GPU 帧当作正确内容更新，不将合并请求当作完成。', '',
          '| 语料/配置 | 最新请求 P99 | 最早合并请求等待 P99 | 正确内容间隔 P99 | 普通 GPU 帧间隔 P99 |',
          '| --- | ---: | ---: | ---: | ---: |']
for g in final['summaries']:
    lines.append(f"| {g['corpus']}/{g['profile']} | " + ' | '.join(fmt(scene(g, 'live_input_wheel', k)) for k in (
        'event_to_frame.p99_ms', 'oldest_coalesced_batch_to_frame.p99_ms',
        'correct_content_frame_interval.p99_ms', 'frame_interval.p99_ms')) + ' |')
lines += ['', '## CPU、内存与验收限制', '',
          'frame_cpu 是渲染函数 QElapsedTimer 墙钟耗时，含调度影响；process_cpu 为整进程 user+kernel 时间。',
          '最终 mixed 的新默认/reference 热区 P95 比值为 1.158，未表现为所有 CPU 对比均下降；',
          '原参数三轮范围较宽，不能把每次默认调度差异都归因于参数。验收按预先约定的 CPU/OFF 门槛，',
          '并同时保留 reference 比值与同类核心对照，避免隐藏异构核心调度和进程间波动。',
          'charge 包含保守 payload/cache/工作区记账，不等于 RSS。当前/peak RSS 全部保留，原 Linux statm/getrusage',
          '口径仍可能出现 peak 小于当前值，未作为严格内存上界。缓存、队列和临时工作区限额均未扩大。', '',
          '| 语料/配置 | 初始 charge MiB | RSS MiB | peak RSS MiB |',
          '| --- | ---: | ---: | ---: |']
for g in final['summaries']:
    lines.append(f"| {g['corpus']}/{g['profile']} | " + ' | '.join(fmt(g['metrics'][key], 2**20) for key in (
        'initial_resident_charge', 'rss', 'peak_rss')) + ' |')
lines += ['', '最终门槛状态与单轮最大值：', '', '```json',
          json.dumps(validation['gates'], ensure_ascii=False, indent=2), '```', '',
          '默认调度及严格单轮门槛未通过项必须作为剩余缺口，不能以三轮中位数或控制环境遮盖。',
          '此前 GUI 报告中的 mixed CPU 不稳定和略超 50 ms 样本保持历史记录，不据新结果删除。',
          '仍未测显示器呈现、完整主窗口/Ela 动画、真实 UART、其他平台/GPU 后端、多会话长稳或消毒器；',
          '此前后端追加/扫描缺口继续存在，步骤 7 与压缩默认启用状态不变。', '',
          '## 验证、来源和复现', '',
          f"三轮比较 {len(trial['samples'])} 进程，最终复验 {len(final['samples'])} 进程；请求/像素及测试结果见 [validation.json](validation.json)。",
          'Release/Debug 相关测试、实际 GPU self-test、参数拒绝、最终来源哈希与构建日志均保留。',
          '本次提交包含此前未提交的基准、统计、原始报告、有界预取实现及本轮调参记录；不删除历史样本。',
          '版本按 AGENTS 约定递增为 0.2.77，中文提交消息说明测试范围及缺口，交付提交见 Git 历史。', '',
          '```sh',
          'cmake --build build/Release --target NovaTerm novaterm_history_gui_benchmark novaterm_core_tests novaterm_scrollback_tests novaterm_renderer_tests novaterm_renderer_p5_tests novaterm_history_no_qt_link_check -j4',
          '# 构建结束，再采样。每次指定全新输出目录：',
          'NOVATERM_RHI_API=opengl taskset -c 0-15 python3 tests/benchmarks/history/run-gui-matrix.py \\',
          '  --keep-display-on --output build/history-tuning-new --configurations off on-reference on-block384 on-hot2 on-block384-hot2',
          'NOVATERM_RHI_API=opengl python3 tests/benchmarks/history/run-gui-matrix.py \\',
          '  --keep-display-on --output build/history-final-new --configurations off on-reference on-default',
          "ctest --test-dir build/Release -R '^novaterm_(core|scrollback|renderer|renderer_p5)_tests$|^novaterm_history_no_qt_link_check$' --output-on-failure",
          'NOVATERM_RHI_API=opengl build/Release/bin/novaterm_history_gui_benchmark --self-test',
          '```', '',
          'taskset 的 0–15 只适用于此主机拓扑，其他机器需按其同类核心集合复测。',
          '所有样本含实际参数、窗口/平台/编译器、亲和性、来源/可执行文件哈希；阶段不合并、分位数不混池。', '']
(root / 'README.md').write_text('\n'.join(lines))
