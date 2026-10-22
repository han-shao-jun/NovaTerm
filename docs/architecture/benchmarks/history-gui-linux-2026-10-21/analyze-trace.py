#!/usr/bin/env python3
"""Explain GUI pipeline ordering; traces are diagnostic, not matrix samples."""
import json
import math
from pathlib import Path


def dist(values):
    if not values:
        return {'samples': 0}
    values.sort()
    return dict(samples=len(values), min=min(values), max=max(values),
                **{f'p{int(p * 100)}': values[math.ceil(p * len(values)) - 1]
                   for p in (.5, .95, .99)})


def analyze(path):
    sample = json.loads(path.read_text())
    result = {}
    for scene in sample['scenarios'][:3]:
        trace = scene['pipeline_trace']
        spans = [i for i, row in enumerate(trace) if row['kind'] == 'request_handled']
        counts, first_draw, early_ready, late_ready_draw = [], [], 0, []
        for k, start in enumerate(spans):
            events = trace[start:spans[k + 1] if k + 1 < len(spans) else len(trace)]
            request = events[0]
            draws = [e for e in events if e['kind'] == 'render_begin']
            ready = [e for e in events if e['kind'] == 'history_ready']
            counts.append(max(e['pending_frames'] for e in events) - request['pending_frames'])
            if draws:
                first_draw.append(draws[0]['at_ms'] - request['at_ms'])
                if ready and ready[0]['at_ms'] < draws[0]['at_ms']:
                    early_ready += 1
            for r in ready:
                d = next((e for e in draws if e['at_ms'] >= r['at_ms']), None)
                if d:
                    late_ready_draw.append(d['at_ms'] - r['at_ms'])
        result[scene['name']] = dict(
            requests=len(spans), pending_passes=dist(counts),
            requests_with_multiple_pending=sum(n > 1 for n in counts),
            ready_before_first_draw=early_ready, handler_to_first_draw_ms=dist(first_draw),
            ready_to_next_draw_ms=dist(late_ready_draw),
            caveat='history_ready is GUI signal delivery, not worker decode completion timestamp')
    return result


root = Path(__file__).resolve().parent
result = {name: analyze(root / name) for name in
          ('trace-before.json', 'trace-batch-only.json', 'trace-final.json')}
(root / 'pipeline-analysis.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
