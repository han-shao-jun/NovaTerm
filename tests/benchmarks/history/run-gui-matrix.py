#!/usr/bin/env python3
"""Serial, independent-process GUI history scrolling benchmark matrix.
Run from the repository root; requires an exposed desktop/GPU window.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import shutil
import subprocess
import time

CONFIGURATIONS = {
    'off': dict(profile='off'),
    'on-default': dict(profile='on-default'),
    'on-small': dict(profile='on-small'),
    'on-hot': dict(profile='on-hot'),
    'on-reference': dict(profile='on-default', block_kib=512, hot_mib=4),
    'on-block384': dict(profile='on-default', block_kib=384, hot_mib=4),
    'on-hot2': dict(profile='on-default', block_kib=512, hot_mib=2),
    'on-block384-hot2': dict(profile='on-default', block_kib=384, hot_mib=2),
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, default=Path('build/Release/bin/novaterm_history_gui_benchmark'))
    parser.add_argument('--output', type=Path, default=Path('build/history-gui-performance'))
    parser.add_argument('--rows', type=int, default=50000)
    parser.add_argument('--samples', type=int, default=128)
    parser.add_argument('--runs', type=int, default=3)
    parser.add_argument('--configurations', nargs='+', choices=CONFIGURATIONS,
                        default=['off', 'on-default', 'on-small', 'on-hot'])
    parser.add_argument('--keep-display-on', action='store_true',
                        help='X11: require unlocked desktop, temporarily inhibit blanking, monitor each process')
    args = parser.parse_args()
    if args.runs < 3 or not 16 <= args.samples <= 1000 or not 10000 <= args.rows <= 200000:
        parser.error('need >=3 runs, 16..1000 samples, 10000..200000 rows')
    if len(set(args.configurations)) != len(args.configurations):
        parser.error('configurations must be unique')
    if args.output.exists() and any(args.output.iterdir()):
        parser.error('output directory must be empty; preserve previous samples in their original directory')
    args.output.mkdir(parents=True, exist_ok=True)
    if args.keep_display_on:
        from gui_display_guard import DisplayGuard
        guard = DisplayGuard()
        try:
            with guard:
                run_matrix(args, guard)
        finally:
            if args.output.exists():
                (args.output / 'display-guard.json').write_text(json.dumps(guard.description, indent=2) + '\n')
    else:
        run_matrix(args)


def run_matrix(args, display=None):
    root = Path(__file__).resolve().parents[3]
    exe = args.executable.resolve()
    out = args.output.resolve()
    if out.exists() and any(out.iterdir()):
        raise ValueError('output directory must be empty; preserve previous samples in their original directory')
    out.mkdir(parents=True, exist_ok=True)
    def git(*command):
        return subprocess.check_output(['git', '-C', str(root), *command], text=True).strip()
    sources = ['CMakeLists.txt', 'tests/benchmarks/HistoryGuiScrollBenchmark.cpp', 'tests/benchmarks/history/HistoryBenchmarkSupport.h',
               'tests/benchmarks/history/run-gui-matrix.py', 'tests/CMakeLists.txt',
               'src/renderer/TerminalRenderer.cpp', 'src/renderer/TerminalRenderer.h',
               'src/renderer/RenderScheduler.cpp', 'src/renderer/RenderScheduler.h',
               'src/core/scrollback/LineLayout.cpp', 'src/core/scrollback/ScrollbackSnapshot.cpp',
               'src/core/terminal/TerminalCore.cpp', 'src/history/HistoryReadContext.cpp',
               'src/history/HistoryCompressionService.cpp', 'src/history/HistoryCompressionService.h']
    if display:
        sources.append('tests/benchmarks/history/gui_display_guard.py')
    metadata = dict(source_revision=git('rev-parse', 'HEAD'), working_tree_dirty=bool(git('status', '--porcelain')),
                    executable_sha256=digest(exe), source_files=[dict(file=p, sha256=digest(root / p)) for p in sources],
                    environment=dict(os=platform.platform(), qt_platform='real exposed desktop window',
                                     rhi_environment='see per-process rhi_api and stderr; use NOVATERM_RHI_API',
                                     cpu_affinity='inherited from parent; see cpu_affinity_actual',
                                     cpu_affinity_actual=sorted(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else None),
                    started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    invocation=dict(rows=args.rows, samples=args.samples, runs=args.runs,
                                    configurations=args.configurations),
                    configurations={name: CONFIGURATIONS[name] for name in args.configurations},
                    display_guard=dict(display.description) if display else None,
                    boundary='event posted -> complete content+glyph draw pass frameSubmitted; no compositor scanout measurement',
                    methodology='profiles rotate across runs; independent processes; serial; do not build while sampling',
                    percentiles='nearest rank in each process; summary median/min/max of per-process percentiles; not pooled',
                    pixel_check='independent final-state grabFramebuffer after each measured stage; readback redraws; not timed frame capture')
    if Path('/proc/cpuinfo').exists():
        metadata['environment']['cpu'] = next(line.split(':', 1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines() if line.startswith('model name'))
        metadata['environment']['meminfo'] = Path('/proc/meminfo').read_text().splitlines()[:3]
    if shutil.which('lspci'):
        metadata['environment']['gpu'] = '\n'.join(line for line in subprocess.check_output(['lspci'], text=True).splitlines() if 'VGA' in line or '3D controller' in line)
    start = time.monotonic()
    measurements = []
    profiles = args.configurations
    for corpus in ('variable', 'mixed'):
        for run in range(1, args.runs + 1):
            shift = (run - 1) % len(profiles)
            for profile in profiles[shift:] + profiles[:shift]:
                name = f'{corpus}-{profile}-{run}'
                configuration = CONFIGURATIONS[profile]
                output = out / f'{name}.json'
                command = [str(exe), '--corpus', corpus, '--profile', configuration['profile'], '--rows', str(args.rows),
                           '--samples', str(args.samples), '--output', str(output)]
                for field, flag in (('block_kib', '--block-kib'), ('hot_mib', '--hot-mib')):
                    if field in configuration:
                        command.extend([flag, str(configuration[field])])
                with (out / f'{name}.log').open('w') as log:
                    if display:
                        from gui_display_guard import run_visible
                        code = run_visible(command, log, display)
                    else:
                        code = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=180).returncode
                if code:
                    raise RuntimeError(f'{name} failed: see {out / (name + ".log")}')
                sample = json.loads(output.read_text())
                assert sample['verified'] and sample['schema_version'] == 1 and sample['build_type'] == 'Release'
                assert sample['input_rows'] == args.rows and sample['samples_per_scenario'] == args.samples
                assert sample['corpus'] == corpus and sample['profile'] == configuration['profile']
                if 'block_kib' in configuration:
                    assert sample['target_block_bytes'] == configuration['block_kib'] * 1024
                if 'hot_mib' in configuration:
                    assert sample['hot_bytes'] == configuration['hot_mib'] * 1024 * 1024
                assert sample['pixel_checks'] == 4
                for scenario in sample['scenarios']:
                    assert scenario['requests'] == scenario['completed'] + scenario['coalesced'] == args.samples
                    assert not scenario['input_rejected'] and not scenario['decode_errors']
                    assert scenario['event_to_frame']['samples'] == scenario['completed']
                    assert scenario['event_to_handler']['samples'] == args.samples
                    assert len(scenario['events']) == args.samples
                sample['run'] = run
                sample['configuration'] = profile
                measurements.append(sample)
                print(f'{name}: verified ({len(measurements)}/{2 * args.runs * len(profiles)})', flush=True)
    summaries = []
    for corpus in ('variable', 'mixed'):
        for profile in profiles:
            group = [s for s in measurements if s['corpus'] == corpus and s['configuration'] == profile]
            def dist(values):
                return dict(median=statistics.median(values), min=min(values), max=max(values))
            summary = dict(corpus=corpus, profile=profile, runs=len(group), metrics={}, scenarios={})
            for field in ('initial_resident_charge', 'initial_retained_lines', 'initial_compressed_blocks',
                          'prefill_wall_ms', 'prefill_process_cpu_ms', 'rss', 'peak_rss'):
                summary['metrics'][field] = dist([s[field] for s in group])
            for name in ('hot_wheel', 'cold_pages', 'cold_scrollbar_jumps', 'live_input_wheel'):
                scenes = [next(x for x in s['scenarios'] if x['name'] == name) for s in group]
                metrics = {}
                for field in ('completed', 'coalesced', 'duration_ms', 'process_cpu_ms', 'correct_content_fps',
                              'cache_hits', 'cache_misses', 'history_pending_frames', 'busy_reads',
                              'frame_gaps_over_16_7_ms', 'frame_gaps_over_33_3_ms',
                              'correct_content_gaps_over_16_7_ms', 'correct_content_gaps_over_33_3_ms'):
                    metrics[field] = dist([s[field] for s in scenes])
                for field in ('event_to_frame', 'event_to_handler', 'oldest_coalesced_batch_to_frame',
                              'frame_interval', 'correct_content_frame_interval', 'frame_cpu', 'heartbeat_lag'):
                    for percentile in ('p50_ms', 'p95_ms', 'p99_ms', 'max_ms'):
                        metrics[field + '.' + percentile] = dist([s[field][percentile] for s in scenes])
                for key, samples_key in (('frame_gaps_over_16_7_ms', 'frame_interval'),
                                         ('frame_gaps_over_33_3_ms', 'frame_interval'),
                                         ('correct_content_gaps_over_16_7_ms', 'correct_content_frame_interval'),
                                         ('correct_content_gaps_over_33_3_ms', 'correct_content_frame_interval')):
                    metrics[key + '.fraction'] = dist([s[key] / s[samples_key]['samples'] if s[samples_key]['samples'] else 0 for s in scenes])
                summary['scenarios'][name] = metrics
            summaries.append(summary)
    for corpus in ('variable', 'mixed'):
        same = [s for s in measurements if s['corpus'] == corpus]
        assert len({(s['columns'], s['rows'], s['input_rows'], s['initial_retained_lines']) for s in same}) == 1
    assert digest(exe) == metadata['executable_sha256'], 'executable changed during sampling'
    for source in metadata['source_files']:
        assert digest(root / source['file']) == source['sha256'], 'source changed during sampling'
    report = dict(schema_version=1, **metadata, elapsed_seconds=time.monotonic() - start,
                  samples=measurements, summaries=summaries,
                  unmeasured=['OS mouse hardware sampling', 'compositor/scanout latency', 'real UART',
                              'multi-session long soak', 'other operating systems/backends'])
    (out / 'matrix.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(f'All {len(measurements)} processes verified; report: {out / "matrix.json"}', flush=True)


if __name__ == '__main__':
    main()
