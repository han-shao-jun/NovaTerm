#!/usr/bin/env python3
"""Apply the selection rules fixed in design.md to three independent runs."""
import json
from pathlib import Path

root = Path(__file__).resolve().parent
matrix = json.loads((root / 'three-rounds-active-display/matrix.json').read_text())
groups = {(s['corpus'], s['profile']): s for s in matrix['summaries']}
configs = ['on-reference', 'on-block384', 'on-hot2', 'on-block384-hot2']
results = []
for config in configs:
    per_corpus = {}
    for corpus in ('variable', 'mixed'):
        g = groups[corpus, config]
        off = groups[corpus, 'off']
        reference = groups[corpus, 'on-reference']
        scenes = g['scenarios']
        heat = scenes['hot_wheel']['frame_cpu.p95_ms']['median'] / off['scenarios']['hot_wheel']['frame_cpu.p95_ms']['median']
        charge = g['metrics']['initial_resident_charge']['median']
        saving = 1 - charge / off['metrics']['initial_resident_charge']['median']
        cold95 = max(scenes[s]['event_to_frame.p95_ms']['median'] for s in ('cold_pages', 'cold_scrollbar_jumps'))
        cold99 = max(scenes[s]['event_to_frame.p99_ms']['median'] for s in ('cold_pages', 'cold_scrollbar_jumps'))
        oldest = scenes['live_input_wheel']['oldest_coalesced_batch_to_frame.p99_ms']['median']
        correct = scenes['live_input_wheel']['correct_content_frame_interval.p99_ms']['median']
        per_corpus[corpus] = dict(
            hot_cpu_on_over_off=heat, charge=charge, saving=saving,
            cold_p95=cold95, cold_p99=cold99, oldest_p99=oldest, correct_interval_p99=correct,
            gpu_interval_p99=scenes['live_input_wheel']['frame_interval.p99_ms']['median'],
            latest_p99=scenes['live_input_wheel']['event_to_frame.p99_ms']['median'],
            cold_p95_single_run_max=max(scenes[s]['event_to_frame.p95_ms']['max'] for s in ('cold_pages', 'cold_scrollbar_jumps')),
            cold_p99_single_run_max=max(scenes[s]['event_to_frame.p99_ms']['max'] for s in ('cold_pages', 'cold_scrollbar_jumps')),
            cpu_ms=sum(scenes[s]['process_cpu_ms']['median'] for s in scenes),
            eligible=(heat <= 1.10 and saving >= .40 and cold95 <= 50 and cold99 <= 100
                      and charge <= reference['metrics']['initial_resident_charge']['median']))
    results.append(dict(configuration=config, per_corpus=per_corpus,
                        eligible=all(s['eligible'] for s in per_corpus.values()),
                        tail_score=max(max(s['cold_p99'], s['oldest_p99'], s['correct_interval_p99'])
                                       for s in per_corpus.values()),
                        worst_charge=max(s['charge'] for s in per_corpus.values()),
                        total_cpu_ms=sum(s['cpu_ms'] for s in per_corpus.values())))
eligible = [s for s in results if s['eligible']]
if not eligible:
    winner = 'on-reference'
    reason = 'No group passes all median gates; preserve reference and report failures.'
else:
    best_tail = min(s['tail_score'] for s in eligible)
    close = [s for s in eligible if s['tail_score'] <= best_tail + 5]
    chosen = min(close, key=lambda s: (s['worst_charge'], s['total_cpu_ms'], s['tail_score']))
    winner = chosen['configuration']
    reason = 'Within 5 ms of best worst-corpus tail score, lowest charge, then CPU and tail.'
result = dict(winner=winner, reason=reason, rules='design.md, fixed before sampling', candidates=results,
              parameters=matrix['configurations'][winner])
(root / 'selection.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
print(json.dumps(result, ensure_ascii=False, indent=2))
