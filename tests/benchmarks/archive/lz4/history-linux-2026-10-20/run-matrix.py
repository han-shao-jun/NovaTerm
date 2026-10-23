#!/usr/bin/env python3
"""Linux reproduction of the existing PowerShell schema-2 benchmark matrices.
Usage: python3 run-matrix.py REPO EXECUTABLE OUTPUT_DIRECTORY
Runs serially; does not modify production code or policy.
"""
import datetime
import hashlib
import json
import pathlib
import platform
import statistics
import subprocess
import sys
import time

ROOT, EXE, OUT = (pathlib.Path(arg).resolve() for arg in sys.argv[1:])
OUT.mkdir(parents=True, exist_ok=True)
def command(*args):
    return subprocess.check_output(args, cwd=ROOT, text=True).strip()
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
def distribution(values):
    return dict(median=statistics.median(values), min=min(values), max=max(values))
def require(condition, message):
    if not condition:
        raise RuntimeError(message)

revision = command('git', 'rev-parse', 'HEAD')
dirty = bool(command('git', 'status', '--porcelain'))
cpu_info = pathlib.Path('/proc/cpuinfo').read_text()
cpu = next(line.split(':', 1)[1].strip() for line in cpu_info.splitlines() if line.startswith('model name'))
gpu = '\n'.join(line for line in command('lspci').splitlines() if any(x in line.lower() for x in ('vga', '3d controller', 'display controller')))
environment = dict(os=platform.platform(), distribution=platform.freedesktop_os_release()['PRETTY_NAME'], cpu=cpu, gpu=gpu,
    meminfo_at_start=pathlib.Path('/proc/meminfo').read_text().splitlines()[:3], qt='not linked', executable=str(EXE),
    compiler=command('c++', '--version').splitlines()[0], cpu_affinity='OS default; no pinning',
    sampling='serial independent processes; no concurrent build or other task benchmark')
metrics = ('resident_charge rss peak_rss retained_lines append_ms append_wall_ms ingest_cpu_ms ingest_wall_ms '
    'append_cell_mib_per_s batch_p95_us batch_p99_us settle_ms scan_ms scan_cpu_ms scan_cell_mib_per_s '
    'read_p95_us read_p99_us cold_p95_us cold_p99_us cold_encoded_p95_us cold_encoded_p99_us hot_p95_us hot_p99_us '
    'index_ms idle_store_close_ms idle_service_stop_ms compressed_blocks cold_encoded_samples').split()
sources = [dict(file=name, sha256=sha(ROOT / 'tests/benchmarks/history/lz4' / name))
    for name in ('HistoryBenchmark.cpp', 'HistoryBenchmarkSupport.h', 'run-matrix.ps1')]
corpora = ('repeat', 'variable', 'mixed', 'entropy', 'long', 'rewrite')
scenarios = [
    ('same-content', 'SameContent', 10000, 1073741824, 0, corpora),
    ('equal-budget', 'EqualBudget', 25000, 33554432, 0, corpora),
    ('equal-budget-paced', 'EqualBudget', 25000, 33554432, 2, ('repeat', 'variable')),
]
for filename, scenario, rows, budget, pace, selected in scenarios:
    started = datetime.datetime.now(datetime.timezone.utc).isoformat()
    begin = time.monotonic()
    samples = []
    for corpus in selected:
        count = 100 if corpus == 'long' else rows
        for run in range(1, 4):
            for mode in (('off', 'on') if run % 2 else ('on', 'off')):
                args = [str(EXE), corpus, str(count), str(budget), mode, str(pace), '64']
                result = subprocess.run(args, capture_output=True, text=True, timeout=120)
                if result.returncode:
                    (OUT / f'{filename}-failure.json').write_text(json.dumps(dict(command=args, stdout=result.stdout, stderr=result.stderr, returncode=result.returncode), indent=2))
                    raise RuntimeError(f'{corpus}/{mode}/{run}: {result.stderr}')
                sample = json.loads(result.stdout)
                require(sample['schema_version'] == 2 and sample['verified'] and sample['build_type'] == 'Release', 'verified schema-2 Release required')
                require(mode != 'on' or sample['admitted'] == 1, 'ON admission failed')
                require(sample['retained_hash'] == sample['expected_retained_hash'], 'retained hash mismatch')
                require((sample['rows'], sample['budget'], sample['corpus'], sample['mode'], sample['pace_ms_per_64_rows']) ==
                    (count, budget, corpus, mode.upper(), pace), 'parameter mismatch')
                sample['run'] = run
                samples.append(sample)
        print(f'{filename}: {corpus} 6/6 verified', flush=True)
    summaries = []
    comparisons = []
    for corpus in selected:
        group = [s for s in samples if s['corpus'] == corpus]
        require(len({s['hash_fnv1a64'] for s in group}) == 1, 'input differs')
        same = all(s['retained_lines'] == s['rows'] and s['verified_cells'] == s['input_cells'] for s in group)
        require(not same or len({s['retained_hash'] for s in group}) == 1, 'OFF/ON content differs')
        modes = {}
        for mode in ('OFF', 'ON'):
            mode_samples = [s for s in group if s['mode'] == mode]
            values = {metric: distribution([s[metric] for s in mode_samples]) for metric in metrics}
            summaries.append(dict(corpus=corpus, mode=mode, runs=len(mode_samples), metrics=values))
            modes[mode] = values
        off, on = modes['OFF'], modes['ON']
        saving = 1 - on['resident_charge']['median'] / off['resident_charge']['median']
        append_ratio = on['append_cell_mib_per_s']['median'] / off['append_cell_mib_per_s']['median']
        scan_ratio = on['scan_cell_mib_per_s']['median'] / off['scan_cell_mib_per_s']['median']
        retention = on['retained_lines']['median'] / off['retained_lines']['median']
        cold_measured = all(s['cold_encoded_samples'] > 0 for s in group if s['mode'] == 'ON')
        gates = {}
        if scenario == 'SameContent':
            gates.update(full_content=same, append_ratio_ge_0_90=same and append_ratio >= .90, scan_ratio_ge_0_80=same and scan_ratio >= .80)
            if corpus in ('repeat', 'variable'):
                gates['charge_saving_ge_0_40'] = same and saving >= .40
        if cold_measured:
            gates.update(cold_p95_le_50ms=on['cold_encoded_p95_us']['max'] <= 50000,
                cold_p99_le_100ms=on['cold_encoded_p99_us']['max'] <= 100000)
        comparisons.append(dict(corpus=corpus, full_content=same, charge_saving_fraction=saving,
            append_throughput_on_over_off=append_ratio, scan_throughput_on_over_off=scan_ratio,
            retained_lines_on_over_off=retention, cold_compressed_measured=cold_measured, gates=gates))
    report = dict(schema_version=2, target_commit='f662302aa964e81c4ea5b9aefda1bbe3b2f2189b', source_revision=revision,
        working_tree_dirty=dirty, scenario=scenario, started_utc=started, elapsed_seconds=time.monotonic() - begin,
        parameters=dict(rows=rows, long_rows=100, budget=budget, pace_ms=pace, read_samples=64, runs=3, corpora=selected),
        environment=environment, executable_sha256=sha(EXE), benchmark_sources=sources,
        runner=dict(file=pathlib.Path(__file__).name, sha256=sha(pathlib.Path(__file__)), methodology='matches run-matrix.ps1; all subprocesses run sequentially'),
        measurement=dict(append='append/continuation + processHistory + publish calls; excludes generation/hash/pacing',
            ingest_cpu='process user+kernel including worker, generation/hash/ingestion/settle; not receive CPU',
            scan='cursor reads + chars[0] checksum before cold-cache sampling; excludes full-field verification',
            throughput='Cell storage MiB/s (sizeof(Cell)); not transport byte throughput',
            cold='first CacheOnly request to Found for distinct blocks; includes worker scheduling; not GUI frame',
            memory='resident_charge before reads; RSS after ingestion; peak RSS includes batches/verification',
            percentiles='nearest rank per process; summaries median/min/max of process percentiles',
            close='idle destruction/stop; excludes queued-work close'),
        samples=samples, summaries=summaries, comparisons=comparisons,
        synthetic_gates_passed=all(all(c['gates'].values()) for c in comparisons),
        unmeasured=['transport/Parser throughput', 'fixed-rate receive CPU', 'GUI frame latency', 'search/reflow/cancel',
            'model lock P99', 'queued-work close latency', '1/4/8-session 30min soak', 'real UART', 'other platforms'])
    (OUT / f'{filename}.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(f'{filename}: {len(samples)} samples; synthetic gates passed={report["synthetic_gates_passed"]}', flush=True)
