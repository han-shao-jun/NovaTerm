#!/usr/bin/env python3
"""Compare independent-process GUI matrices; never pool percentiles."""
import json
from pathlib import Path


def main():
    root = Path(__file__).resolve().parent
    matrices = {name: json.loads((root / name / 'matrix.json').read_text())
                for name in ('before-desktop', 'after')}
    paired = {}
    for name, matrix in matrices.items():
        assert len(matrix['samples']) == 24
        paired[name] = {(s['corpus'], s['profile'], s['run']): s for s in matrix['samples']}
        assert len(paired[name]) == 24
    for key, before in paired['before-desktop'].items():
        after = paired['after'][key]
        for field in ('input_rows', 'samples_per_scenario', 'columns', 'rows', 'dpr',
                      'initial_retained_lines', 'target_block_bytes', 'hot_bytes',
                      'build_type', 'compiler', 'qt', 'platform', 'rhi_api'):
            assert before[field] == after[field], (key, field, before[field], after[field])
        for scene in after['scenarios']:
            assert scene['completed'] + scene['coalesced'] == scene['requests'] == 128
            assert scene['input_rejected'] == scene['decode_errors'] == 0
    rows = []
    for corpus in ('variable', 'mixed'):
        for profile in ('off', 'on-default', 'on-small', 'on-hot'):
            groups = {name: next(s for s in m['summaries']
                                 if s['corpus'] == corpus and s['profile'] == profile)
                      for name, m in matrices.items()}
            for scene in ('hot_wheel', 'cold_pages', 'cold_scrollbar_jumps', 'live_input_wheel'):
                rows.append(dict(corpus=corpus, profile=profile, scene=scene,
                                 before=groups['before-desktop']['scenarios'][scene],
                                 after=groups['after']['scenarios'][scene]))
    gates = {}
    for corpus in ('variable', 'mixed'):
        current = {s['profile']: s for s in matrices['after']['summaries'] if s['corpus'] == corpus}
        old = next(s for s in matrices['before-desktop']['summaries']
                   if s['corpus'] == corpus and s['profile'] == 'on-default')
        hot = current['on-default']['scenarios']['hot_wheel']['frame_cpu.p95_ms']['median']
        off = current['off']['scenarios']['hot_wheel']['frame_cpu.p95_ms']['median']
        cold = {scene: {key: current['on-default']['scenarios'][scene]['event_to_frame.' + key]
                        for key in ('p95_ms', 'p99_ms')}
                for scene in ('cold_pages', 'cold_scrollbar_jumps')}
        gates[corpus] = dict(
            hot_renderer_cpu_after_over_before=hot / old['scenarios']['hot_wheel']['frame_cpu.p95_ms']['median'],
            hot_renderer_cpu_on_over_off=hot / off,
            initial_charge_saving=1 - current['on-default']['metrics']['initial_resident_charge']['median']
            / current['off']['metrics']['initial_resident_charge']['median'],
            cold=cold,
            cold_all_runs_pass=all(v['p95_ms']['max'] <= 50 and v['p99_ms']['max'] <= 100 for v in cold.values()))
    result = dict(method='median/min/max of three per-process quantiles; not pooled', gates=gates, rows=rows)
    (root / 'comparison.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(gates, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
