#!/usr/bin/env python3
"""Supplementary hot CPU control: same full GUI load, P-core pool on this host."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--executable', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
args = p.parse_args()
affinity = sorted(os.sched_getaffinity(0))
assert affinity == list(range(16)), 'run under taskset -c 0-15 on the recorded i7-14700K'
out = args.output.resolve()
if out.exists() and any(out.iterdir()):
    p.error('output must be a new/empty directory')
out.mkdir(parents=True, exist_ok=True)
exe = args.executable.resolve()
sha = hashlib.sha256(exe.read_bytes()).hexdigest()
start = time.monotonic()
samples = []
for corpus in ('variable', 'mixed'):
    for run in range(1, 4):
        for profile in (('off', 'on-default') if run % 2 else ('on-default', 'off')):
            name = f'{corpus}-{profile}-{run}'
            output = out / f'{name}.json'
            with (out / f'{name}.log').open('w') as log:
                subprocess.run([str(exe), '--corpus', corpus, '--profile', profile,
                                '--rows', '50000', '--samples', '128', '--output', str(output)],
                               stdout=log, stderr=subprocess.STDOUT, timeout=180, check=True)
            s = json.loads(output.read_text())
            assert s['verified'] and s['pixel_checks'] == 4
            assert s['input_rows'] == 50000 and s['samples_per_scenario'] == 128
            for scene in s['scenarios']:
                assert scene['requests'] == scene['completed'] + scene['coalesced'] == 128
                assert scene['input_rejected'] == scene['decode_errors'] == 0
            s['run'] = run
            samples.append(s)
            print(f'{name}: verified ({len(samples)}/12)', flush=True)


def dist(values):
    return dict(median=statistics.median(values), min=min(values), max=max(values))


summary = {}
for corpus in ('variable', 'mixed'):
    summary[corpus] = {}
    for profile in ('off', 'on-default'):
        group = [s for s in samples if s['corpus'] == corpus and s['profile'] == profile]
        summary[corpus][profile] = {
            scene: {key: dist([next(x for x in s['scenarios'] if x['name'] == scene)[field][percentile]
                              for s in group])
                    for key, field, percentile in (
                        ('frame_cpu.p95_ms', 'frame_cpu', 'p95_ms'),
                        ('event_to_frame.p95_ms', 'event_to_frame', 'p95_ms'),
                        ('event_to_frame.p99_ms', 'event_to_frame', 'p99_ms'))}
            for scene in ('hot_wheel', 'cold_pages', 'cold_scrollbar_jumps', 'live_input_wheel')}
    summary[corpus]['hot_cpu_on_over_off'] = (
        summary[corpus]['on-default']['hot_wheel']['frame_cpu.p95_ms']['median']
        / summary[corpus]['off']['hot_wheel']['frame_cpu.p95_ms']['median'])
assert hashlib.sha256(exe.read_bytes()).hexdigest() == sha
report = dict(executable_sha256=sha, cpu_affinity=affinity, elapsed_seconds=time.monotonic() - start,
              purpose='supplementary CPU scheduling control; not a replacement for the full unpinned matrix',
              load='unchanged 50000 input rows, 128 requests per scenario, 60 Hz, four scenarios per process',
              samples=samples, summary=summary)
(out / 'matrix.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
print(json.dumps(summary, ensure_ascii=False, indent=2), flush=True)
