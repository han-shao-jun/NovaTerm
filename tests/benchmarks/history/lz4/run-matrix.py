#!/usr/bin/env python3
"""Run the synthetic history compression matrix (Python 3.9+, standard library).

Paths are relative to the working directory. Each measurement uses an independent
process; OFF/ON order alternates across runs. Gates describe the synthetic backend
only and do not establish UART, Parser, or GUI release readiness.
"""
import argparse
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time


CORPORA = ('repeat', 'variable', 'mixed', 'entropy', 'long', 'rewrite')
METRICS = (
    'resident_charge', 'rss', 'peak_rss', 'retained_lines', 'append_ms', 'append_wall_ms',
    'ingest_cpu_ms', 'ingest_wall_ms', 'append_cell_mib_per_s', 'batch_p95_us', 'batch_p99_us',
    'settle_ms', 'scan_ms', 'scan_cpu_ms', 'scan_cell_mib_per_s', 'read_p95_us', 'read_p99_us',
    'cold_p95_us', 'cold_p99_us', 'cold_encoded_p95_us', 'cold_encoded_p99_us',
    'hot_p95_us', 'hot_p99_us', 'index_ms', 'idle_store_close_ms', 'idle_service_stop_ms',
    'compressed_blocks', 'cold_encoded_samples',
)
MEASUREMENT = {
    'append': 'sum of append/continuation + processHistory + publish calls; excludes generation/hash/pacing',
    'ingest_cpu': 'process user+kernel including worker, corpus generation, hash, ingestion and settle; not receive CPU',
    'scan': 'before cold-cache sampling: sequential cursor reads + chars[0] checksum; excludes full-field verification',
    'throughput': 'Cell storage MiB/s (sizeof(Cell)); not transport byte throughput',
    'cold': 'one distinct sampled block each, CacheOnly request to Found; includes worker scheduling; not GUI frame',
    'memory': 'resident_charge before reads; RSS after ingestion; peak RSS includes corpus batch/read/verification',
    'percentiles': 'nearest rank per process; summary gives median/min/max of process percentiles',
    'close': 'idle store destruction and idle service stop; excludes queued-work close latency',
}
UNMEASURED = [
    'transport/Parser throughput', 'fixed-rate receive CPU', 'GUI frame latency',
    'search/reflow/cancel', 'model lock P99', 'queued-work close latency',
    '1/4/8-session 30min soak', 'real UART', 'other platforms',
]


def bounded_integer(low, high):
    def parse(value):
        try:
            number = int(value)
        except ValueError as error:
            raise argparse.ArgumentTypeError('must be an integer') from error
        if not low <= number <= high:
            raise argparse.ArgumentTypeError(f'must be in {low}..{high}')
        return number
    return parse


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    suffix = '.exe' if os.name == 'nt' else ''
    parser.add_argument('--executable', '-Executable', type=Path,
                        default=Path('build/history-isolated-final/checks/novaterm_history_benchmark' + suffix))
    parser.add_argument('--output', '-Output', type=Path, default=Path('build/history-performance.json'))
    parser.add_argument('--rows', '-Rows', type=bounded_integer(1, 10000000), default=10000)
    parser.add_argument('--long-rows', '-LongRows', type=bounded_integer(1, 10000000), default=100)
    parser.add_argument('--budget', '-Budget', type=bounded_integer(1, 9223372036854775807), default=1073741824)
    parser.add_argument('--pace-milliseconds', '-PaceMilliseconds', type=bounded_integer(0, 1000), default=0)
    parser.add_argument('--runs', '-Runs', type=bounded_integer(3, 100), default=3)
    parser.add_argument('--read-samples', '-ReadSamples', type=bounded_integer(1, 10000), default=64)
    parser.add_argument('--corpora', '-Corpora', nargs='+', choices=CORPORA, default=list(CORPORA))
    parser.add_argument('--scenario', '-Scenario', choices=('SameContent', 'EqualBudget'), default='SameContent')
    parser.add_argument('--enforce-gates', '-EnforceGates', action='store_true')
    args = parser.parse_args(argv)
    if len(args.corpora) != len(set(args.corpora)):
        parser.error('corpora must be unique')
    return args


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest().upper()


def validate_sample(sample, corpus, count, mode, args):
    if not isinstance(sample, dict):
        raise ValueError('benchmark sample must be a JSON object')
    if sample.get('schema_version') != 2 or sample.get('verified') is not True or sample.get('build_type') != 'Release':
        raise ValueError('matrix requires verified schema 2 Release samples')
    expected = {'rows': count, 'budget': args.budget, 'corpus': corpus, 'mode': mode.upper(),
                'pace_ms_per_64_rows': args.pace_milliseconds}
    if any(sample.get(key) != value for key, value in expected.items()):
        raise ValueError('sample parameters differ from request')
    if mode == 'on' and sample.get('admitted') != 1:
        raise ValueError('ON admission failed')
    for key in ('hash_fnv1a64', 'retained_hash', 'expected_retained_hash'):
        if not isinstance(sample.get(key), str) or not sample[key]:
            raise ValueError(f'missing or invalid {key}')
    if sample['retained_hash'] != sample['expected_retained_hash']:
        raise ValueError('retained hash mismatch')
    for key in METRICS + ('input_cells', 'verified_cells', 'cold_samples'):
        value = sample.get(key)
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
            raise ValueError(f'missing or invalid numeric sample field: {key}')
    for key in ('retained_lines', 'input_cells', 'verified_cells', 'cold_samples', 'cold_encoded_samples'):
        if not isinstance(sample[key], int):
            raise ValueError(f'sample count must be an integer: {key}')
    if sample['retained_lines'] > count or sample['verified_cells'] > sample['input_cells']:
        raise ValueError('retained sample exceeds input')
    # The current executable reports actual samples, capped by retained chunks,
    # rather than echoing the requested read count. Do not require equality.
    if not 0 <= sample['cold_encoded_samples'] <= sample['cold_samples'] <= min(args.read_samples, sample['retained_lines']):
        raise ValueError('read sample counts differ from request or retained content')
    if 'read_samples' in sample and sample['read_samples'] != args.read_samples:
        raise ValueError('requested read sample count differs from request')


def summarize(samples, corpora):
    summaries = []
    for corpus in corpora:
        for mode in ('OFF', 'ON'):
            group = [s for s in samples if s['corpus'] == corpus and s['mode'] == mode]
            metrics = {}
            for metric in METRICS:
                values = [s[metric] for s in group]
                metrics[metric] = {'median': statistics.median(values), 'min': min(values), 'max': max(values)}
            summaries.append({'corpus': corpus, 'mode': mode, 'runs': len(group), 'metrics': metrics})
    return summaries


def compare(samples, summaries, corpora, scenario):
    comparisons = []
    for corpus in corpora:
        group = [s for s in samples if s['corpus'] == corpus]
        if len({s['hash_fnv1a64'] for s in group}) != 1:
            raise ValueError(f'input differs: {corpus}')
        same_content = all(s['retained_lines'] == s['rows'] and s['verified_cells'] == s['input_cells'] for s in group)
        if same_content and len({s['retained_hash'] for s in group}) != 1:
            raise ValueError(f'OFF/ON content differs: {corpus}')
        off, on = [next(s['metrics'] for s in summaries if s['corpus'] == corpus and s['mode'] == mode)
                   for mode in ('OFF', 'ON')]
        def ratio(metric):
            denominator = off[metric]['median']
            if denominator <= 0:
                raise ValueError(f'cannot compare zero OFF {metric}: {corpus}')
            return on[metric]['median'] / denominator
        saving = 1.0 - ratio('resident_charge')
        append_ratio = ratio('append_cell_mib_per_s')
        scan_ratio = ratio('scan_cell_mib_per_s')
        retention_ratio = ratio('retained_lines')
        cold_measured = all(s['cold_encoded_samples'] > 0 for s in group if s['mode'] == 'ON')
        gates = {}
        if scenario == 'SameContent':
            gates.update(full_content=same_content,
                         append_ratio_ge_0_90=same_content and append_ratio >= 0.90,
                         scan_ratio_ge_0_80=same_content and scan_ratio >= 0.80)
            if corpus in ('repeat', 'variable'):
                gates['charge_saving_ge_0_40'] = same_content and saving >= 0.40
        if cold_measured:
            gates['cold_p95_le_50ms'] = on['cold_encoded_p95_us']['max'] <= 50000
            gates['cold_p99_le_100ms'] = on['cold_encoded_p99_us']['max'] <= 100000
        comparisons.append({
            'corpus': corpus, 'full_content': same_content, 'charge_saving_fraction': saving,
            'append_throughput_on_over_off': append_ratio, 'scan_throughput_on_over_off': scan_ratio,
            'retained_lines_on_over_off': retention_ratio, 'cold_compressed_measured': cold_measured, 'gates': gates,
        })
    return comparisons


def run_matrix(args):
    directory = Path(__file__).resolve().parent
    repo = Path(__file__).resolve().parents[4]
    exe = args.executable.resolve()
    if not exe.is_file() and os.name == 'nt' and not exe.suffix:
        exe = exe.with_suffix('.exe')
    if not exe.is_file():
        raise ValueError(f'benchmark executable does not exist: {exe}')
    def git(*command):
        return subprocess.run(['git', '-c', f'safe.directory={repo}', '-C', str(repo), *command],
                              check=True, capture_output=True, text=True, encoding='utf-8').stdout.strip()
    revision = git('rev-parse', 'HEAD')
    if not revision:
        raise ValueError('cannot identify repository revision')
    dirty = bool(git('status', '--porcelain'))
    started = datetime.datetime.now(datetime.timezone.utc)
    begin = time.monotonic()
    samples = []
    for corpus in args.corpora:
        for run in range(1, args.runs + 1):
            modes = ('off', 'on') if run % 2 else ('on', 'off')
            for mode in modes:
                count = args.long_rows if corpus == 'long' else args.rows
                command = [str(exe), corpus, str(count), str(args.budget), mode,
                           str(args.pace_milliseconds), str(args.read_samples)]
                result = subprocess.run(command, check=True, capture_output=True, text=True, encoding='utf-8')
                try:
                    sample = json.loads(result.stdout)
                    validate_sample(sample, corpus, count, mode, args)
                except (ValueError, TypeError) as error:
                    raise ValueError(f'{corpus} {mode} run {run}: {error}') from error
                sample['run'] = run
                samples.append(sample)
    summaries = summarize(samples, args.corpora)
    comparisons = compare(samples, summaries, args.corpora, args.scenario)
    passed = all(all(comparison['gates'].values()) for comparison in comparisons)
    report = {
        'schema_version': 2, 'target_commit': 'f662302aa964e81c4ea5b9aefda1bbe3b2f2189b',
        'source_revision': revision, 'working_tree_dirty': dirty, 'scenario': args.scenario,
        'started_utc': started.isoformat(), 'elapsed_seconds': time.monotonic() - begin,
        'environment': {'os': platform.platform(), 'cpu': platform.processor() or None,
                        'gpu': None, 'qt': 'not linked', 'executable': str(exe)},
        'measurement': MEASUREMENT, 'executable_sha256': digest(exe),
        'benchmark_sources': [{'file': name, 'sha256': digest(directory / name)} for name in
                              ('HistoryBenchmark.cpp', 'HistoryBenchmarkSupport.h', 'run-matrix.py')],
        'samples': samples, 'summaries': summaries, 'comparisons': comparisons,
        'synthetic_gates_passed': passed, 'unmeasured': UNMEASURED,
        'invocation': {'rows': args.rows, 'long_rows': args.long_rows, 'budget': args.budget,
                       'pace_milliseconds': args.pace_milliseconds, 'runs': args.runs,
                       'read_samples': args.read_samples, 'corpora': args.corpora},
    }
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    print('corpus       charge saving    append ON/OFF    scan ON/OFF    retained ON/OFF')
    for entry in comparisons:
        print(f"{entry['corpus']:<12} {entry['charge_saving_fraction']:>12.3f} "
              f"{entry['append_throughput_on_over_off']:>16.3f} "
              f"{entry['scan_throughput_on_over_off']:>14.3f} {entry['retained_lines_on_over_off']:>18.3f}")
    print(f'Report: {output}')
    if args.enforce_gates and not passed:
        print('synthetic performance gates failed; see saved report', file=sys.stderr)
        return 1
    return 0


def main(argv=None):
    return run_matrix(parse_args(argv))


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'error: {error}', file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr, file=sys.stderr)
        sys.exit(1)
