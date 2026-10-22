#!/usr/bin/env python3
"""Verify sample accounting, adopted defaults, source freeze and test evidence."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parent
trial = json.loads((root / 'three-rounds-active-display/matrix.json').read_text())
final = json.loads((root / 'final-unpinned/matrix.json').read_text())
selection = json.loads((root / 'selection.json').read_text())
assert len(trial['samples']) == 30 and len(final['samples']) == 18
assert trial['environment']['cpu_affinity_actual'] == list(range(16))
assert final['environment']['cpu_affinity_actual'] == list(range(28))
expected = selection['parameters']
geometry = set()
for matrix in (trial, final):
    for sample in matrix['samples']:
        assert sample['verified'] and sample['build_type'] == 'Release'
        assert sample['input_rows'] == 50000 and sample['samples_per_scenario'] == 128
        assert sample['pixel_checks'] == 4
        geometry.add(tuple(sample[k] for k in ('columns', 'rows', 'dpr', 'initial_retained_lines')))
        for scene in sample['scenarios']:
            assert scene['requests'] == scene['completed'] + scene['coalesced'] == 128
            assert scene['input_rejected'] == scene['decode_errors'] == 0
            assert len(scene['events']) == 128
        if sample['configuration'] == 'on-reference':
            assert sample['target_block_bytes'] == 512 * 1024 and sample['hot_bytes'] == 4 * 2**20
for sample in final['samples']:
    if sample['configuration'] == 'on-default':
        assert sample['target_block_bytes'] == expected['block_kib'] * 1024
        assert sample['hot_bytes'] == expected['hot_mib'] * 2**20
assert len(geometry) == 1
for source in final['source_files']:
    assert hashlib.sha256(Path(source['file']).read_bytes()).hexdigest() == source['sha256'], source['file']
assert hashlib.sha256(Path('build/Release/bin/novaterm_history_gui_benchmark').read_bytes()).hexdigest() == final['executable_sha256']
for folder in ('three-rounds-active-display', 'final-unpinned'):
    display = json.loads((root / folder / 'display-guard.json').read_text())
    assert display['released'] and not display['aborted'] and not display['release_errors']
task_display = json.loads((root / 'task-display-guard.json').read_text())
assert task_display['released'] and not task_display['aborted'] and not task_display['release_errors']

tests = {}
for filename, count in (('final-release-ctest.log', 5), ('final-debug-ctest.log', 4)):
    assert f'0 tests failed out of {count}' in (root / filename).read_text()
    tests[filename] = dict(passed=count, failed=0)
for filename in ('final-release-self-test.log', 'final-debug-self-test.log'):
    assert 'history GUI completed-viewport check passed' in (root / filename).read_text()
    tests[filename] = dict(passed=True)
assert 'Ran 6 tests' in (root / 'display-guard-tests.log').read_text() and '\nOK' in (root / 'display-guard-tests.log').read_text()
tests['display-guard-tests.log'] = dict(passed=6, failed=0)

gates = {}
for corpus in ('variable', 'mixed'):
    groups = {g['profile']: g for g in final['summaries'] if g['corpus'] == corpus}
    new, off = groups['on-default'], groups['off']
    scenes = new['scenarios']
    cold = [scenes[s] for s in ('cold_pages', 'cold_scrollbar_jumps')]
    cpu_ratio = scenes['hot_wheel']['frame_cpu.p95_ms']['median'] / off['scenarios']['hot_wheel']['frame_cpu.p95_ms']['median']
    saving = 1 - new['metrics']['initial_resident_charge']['median'] / off['metrics']['initial_resident_charge']['median']
    gates[corpus] = dict(
        cold_median_p95=max(s['event_to_frame.p95_ms']['median'] for s in cold),
        cold_median_p99=max(s['event_to_frame.p99_ms']['median'] for s in cold),
        cold_single_run_p95_max=max(s['event_to_frame.p95_ms']['max'] for s in cold),
        cold_single_run_p99_max=max(s['event_to_frame.p99_ms']['max'] for s in cold),
        median_latency_pass=all(s['event_to_frame.p95_ms']['median'] <= 50 and s['event_to_frame.p99_ms']['median'] <= 100 for s in cold),
        all_runs_latency_pass=all(s['event_to_frame.p95_ms']['max'] <= 50 and s['event_to_frame.p99_ms']['max'] <= 100 for s in cold),
        hot_cpu_on_over_off=cpu_ratio, hot_cpu_pass=cpu_ratio <= 1.10,
        charge_saving=saving, memory_pass=saving >= .40)
report = dict(version='0.2.77', selection=selection['winner'], parameters=expected,
              samples=dict(controlled=30, final_unpinned=18),
              requests=sum(scene['requests'] for m in (trial, final) for s in m['samples'] for scene in s['scenarios']),
              pixel_checks=sum(s['pixel_checks'] for m in (trial, final) for s in m['samples']),
              tests=tests, gates=gates, final_sources_and_executable_match=True,
              display_leases_for_both_matrices_released=True,
              task_display_lease_released=True,
              boundary='completed content+glyph frameSubmitted; no compositor/scanout',
              not_run=['unrelated full CTest', 'Windows/macOS', 'real UART', 'multi-session soak', 'sanitizers'])
(root / 'validation.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
print(json.dumps(report, ensure_ascii=False, indent=2))
