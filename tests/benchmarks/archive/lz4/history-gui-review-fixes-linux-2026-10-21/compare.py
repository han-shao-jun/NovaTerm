"""Reproduce tables from the preserved matrices; no pooled percentiles."""
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
CURRENT = json.loads((HERE / 'matrix/matrix.json').read_text())
BASELINE = json.loads((HERE.parent / 'history-gui-tuning-linux-2026-10-21/final-unpinned/matrix.json').read_text())

def groups(matrix):
    return {(s['corpus'], s['profile']): s for s in matrix['summaries']}

new, old = groups(CURRENT), groups(BASELINE)

def display(value, divisor=1):
    return '{:.2f} [{:.2f}, {:.2f}]'.format(*(value[k] / divisor for k in ('median', 'min', 'max')))

def table(headers, rows):
    return '\n'.join(['| ' + ' | '.join(headers) + ' |', '| ' + ' | '.join(['---'] * len(headers)) + ' |']
                     + ['| ' + ' | '.join(map(str, row)) + ' |' for row in rows])

cold, compare, costs, cost_compare, intervals, gates = [], [], [], [], [], []
for corpus in ('variable', 'mixed'):
    for profile in CURRENT['invocation']['configurations']:
        group = new[corpus, profile]
        for stage in ('cold_pages', 'cold_scrollbar_jumps'):
            s = group['scenarios'][stage]
            cold.append([corpus, profile, stage, display(s['event_to_frame.p95_ms']), display(s['event_to_frame.p99_ms'])])
        costs.append([corpus, profile, display(group['scenarios']['hot_wheel']['frame_cpu.p95_ms']),
                      display(group['metrics']['initial_resident_charge'], 1024**2), display(group['metrics']['rss'], 1024**2)])
    for stage in ('hot_wheel', 'cold_pages', 'cold_scrollbar_jumps', 'live_input_wheel'):
        a, b = old[corpus, 'on-default']['scenarios'][stage], new[corpus, 'on-default']['scenarios'][stage]
        compare.append([corpus, stage, display(a['event_to_frame.p95_ms']), display(b['event_to_frame.p95_ms']),
                        display(a['event_to_frame.p99_ms']), display(b['event_to_frame.p99_ms'])])
    for label, matrix in (('before', old), ('after', new)):
        for profile in ('off', 'on-default'):
            s = matrix[corpus, profile]['scenarios']['live_input_wheel']
            intervals.append([corpus, label, profile, *(display(s[key + '.p99_ms']) for key in
                             ('event_to_frame', 'oldest_coalesced_batch_to_frame', 'correct_content_frame_interval', 'frame_interval'))])
    a, b = old[corpus, 'on-default'], new[corpus, 'on-default']
    cost_compare.append([corpus, display(a['scenarios']['hot_wheel']['frame_cpu.p95_ms']),
                         display(b['scenarios']['hot_wheel']['frame_cpu.p95_ms']),
                         display(a['metrics']['initial_resident_charge'], 1024**2),
                         display(b['metrics']['initial_resident_charge'], 1024**2),
                         display(a['metrics']['rss'], 1024**2), display(b['metrics']['rss'], 1024**2)])
    on, off = new[corpus, 'on-default'], new[corpus, 'off']
    cpu_ratio = on['scenarios']['hot_wheel']['frame_cpu.p95_ms']['median'] / off['scenarios']['hot_wheel']['frame_cpu.p95_ms']['median']
    old_cpu = old[corpus, 'on-default']['scenarios']['hot_wheel']['frame_cpu.p95_ms']['median']
    cpu_before_ratio = on['scenarios']['hot_wheel']['frame_cpu.p95_ms']['median'] / old_cpu
    charge_saved = 1 - on['metrics']['initial_resident_charge']['median'] / off['metrics']['initial_resident_charge']['median']
    cold_p95 = max(on['scenarios'][s]['event_to_frame.p95_ms']['max'] for s in ('cold_pages', 'cold_scrollbar_jumps'))
    cold_p99 = max(on['scenarios'][s]['event_to_frame.p99_ms']['max'] for s in ('cold_pages', 'cold_scrollbar_jumps'))
    gates.append(dict(corpus=corpus, cold_p95_all_runs_max_ms=cold_p95, cold_p99_all_runs_max_ms=cold_p99,
                      hot_cpu_p95_on_off=cpu_ratio, hot_cpu_p95_after_before=cpu_before_ratio,
                      charge_saved_fraction=charge_saved, cold_pass=cold_p95 <= 50 and cold_p99 <= 100,
                      cpu_vs_off_pass=cpu_ratio <= 1.10, cpu_vs_before_pass=cpu_before_ratio <= 1.10))

samples = CURRENT['samples']
assert len(samples) == 24 and all(s['verified'] for s in samples)
requests = sum(s['requests'] for p in samples for s in p['scenarios'])
accounted = sum(s['completed'] + s['coalesced'] for p in samples for s in p['scenarios'])
assert requests == accounted == 12288
assert sum(p['pixel_checks'] for p in samples) == 96
result = dict(gates=gates, processes=len(samples), requests=requests, pixel_checks=96,
              baseline='../history-gui-tuning-linux-2026-10-21/final-unpinned/matrix.json',
              scope='same settings historical comparison; not a simultaneous paired A/B')
(HERE / 'comparison.json').write_text(json.dumps(result, indent=2) + '\n')
sections = [
    '### 默认策略前后延迟（ms）\n\n' + table(['语料', '场景', '之前 P95', '之后 P95', '之前 P99', '之后 P99'], compare),
    '### 完整四策略冷区复测（ms）\n\n' + table(['语料', '策略', '场景', 'P95', 'P99'], cold),
    '### 默认策略 CPU/内存前后对照\n\n' + table(['语料', '之前热区 CPU P95 ms', '之后热区 CPU P95 ms', '之前 charge MiB', '之后 charge MiB', '之前 RSS MiB', '之后 RSS MiB'], cost_compare),
    '### CPU 与内存\n\n' + table(['语料', '策略', '热区 Renderer CPU P95 ms', '初始 charge MiB', '进程 RSS MiB'], costs),
    '### 连续输入滚动四种口径（P99，ms）\n\n' + table(['语料', '前后', '策略', '最新请求', '最早合并请求', '正确内容帧间隔', '普通 GPU 帧间隔'], intervals),
    '### 默认策略门槛\n\n' + table(['语料', '冷区单轮 P95 最大', 'P99 最大', '热区 CPU/OFF', '热区 CPU/之前默认', 'charge 节省'],
         [[g['corpus'], f"{g['cold_p95_all_runs_max_ms']:.3f}", f"{g['cold_p99_all_runs_max_ms']:.3f}",
           f"{g['hot_cpu_p95_on_off']:.3f}", f"{g['hot_cpu_p95_after_before']:.3f}", f"{g['charge_saved_fraction']:.1%}"] for g in gates])
]
(HERE / 'tables.md').write_text('\n\n'.join(sections) + '\n')
print(json.dumps(result, indent=2))
