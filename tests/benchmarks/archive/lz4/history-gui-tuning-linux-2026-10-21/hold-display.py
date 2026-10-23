#!/usr/bin/env python3
"""Keep a temporary display lease across build/measurement phase handoffs."""
import json
from pathlib import Path
import sys
import time

root = Path(__file__).resolve().parent
sys.path.insert(0, str(root.parents[4] / 'tests/benchmarks/history/lz4'))
from gui_display_guard import DisplayGuard

release = root / 'release-task-display-guard'
if release.exists():
    raise SystemExit('release marker already exists; do not reuse this task guard')
guard = DisplayGuard()
try:
    with guard:
        (root / 'task-display-guard.json').write_text(json.dumps(guard.description, indent=2) + '\n')
        print('Task display lease ready; bounded to 30 minutes, release by marker.', flush=True)
        deadline = time.monotonic() + 1800
        while not release.exists() and time.monotonic() < deadline:
            time.sleep(5)
            guard.check()
finally:
    (root / 'task-display-guard.json').write_text(json.dumps(guard.description, indent=2) + '\n')
