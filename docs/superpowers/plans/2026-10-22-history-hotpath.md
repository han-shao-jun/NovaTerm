# History hot-path investigation and small-fix plan

> Execute inline; collect evidence before selecting an implementation change.

**Goal:** distinguish CPU work from scheduling in the Windows GUI history hot phase, then retain only a small change supported by traces and A/B measurements.

**Architecture:** preserve Parser single-writer ownership, immutable snapshots, bounded history/cache budgets, Pending behavior and completed-viewport provenance. Keep existing Release benchmark workload and independent-process comparisons.

**Tech stack:** Qt 6.8.3, C++17, MSVC `/O2 /Ob2`, WPR CPU sampled/CSwitch/ReadyThread, xperf and local PDBs.

**Spec:** user's request; `docs/ARCHITECTURE.md`, `docs/architecture/Rendering_Architecture.md`, `tests/benchmarks/history/lz4/README.md`.

## Constraints and review focus

- Preserve the earlier uncommitted ProbeRenderer constructor fix and unrelated `.dide/` files.
- No policy-default, workload, budget or completion-criterion changes.
- Separate prefill, hot rendering, cold rendering and live input in profile analysis.
- Separate actual on-CPU samples, ready/waiting time and renderer wall-clock measurements.
- Pin lifetime must survive cache eviction; partial/Pending snapshots must never become completed viewports.
- Raw and compressed history, wrapped/empty lines and anchor eviction must retain identical contents and identities.

## Task 1: evidence

- [x] Build an independent optimized executable with first-party PDBs; preserve its source/binary hashes.
- [x] Capture OFF/ON for variable/mixed, with WPR stage markers; use default scheduling and a separate P-core control.
- [x] Export hot-phase stacks, context switches and trace loss statistics; identify a repeatable first-party cost rather than infer it from wall-clock ratios.

## Task 2: bounded fix, conditional on evidence

- [x] Record the concrete hotspot and a single proposed change here before changing production code.
  Evidence: eight optimized WPR runs have zero trace loss. Hot scenes do not
  touch the decode cache; CPU samples under rendererSnapshot occur in both
  OFF/ON (10–21 inclusive samples per hot interval). Source inspection shows
  viewport() already pins each logical line, then rendererSnapshot() rereads
  it for every display row. Reuse those existing pins in monotonically ordered
  display rows, retaining ownership in RendererSnapshot; do not add a cache.
  Check encoded wrapped history with all-dirty and all-clean rows: one cache
  hit per logical pin, stable content/identities, pins valid after clear.
- [x] Add the smallest meaningful regression check for any behavior/lifetime risk, demonstrate it before/after as applicable, and implement the change.
- [x] If traces show only noise/external waits or no safely removable cost, preserve production code and report that evidence instead of inventing a fix.

## Task 3: verify and report

- [x] Build affected targets and run the relevant Core/Scrollback/History/Renderer tests plus the real GPU self-test.
- [x] Re-run unprofiled, independent-process OFF/ON comparisons at the same load; do not mix sampled traces into performance acceptance.
- [x] Record all gate failures and platform limitations, confirm profiler/display cleanup, and update the architecture benchmark documentation. Do not commit or push without a request.

## Execution record

- Independent optimized WPR capture: 8/8 workload checks, no lost buffers/events.
  GUI CPU and sampled render work do not reproduce the full prior wall-clock
  P95 ratio. Default GUI threads ran on P cores; no E-core execution observed.
- Red/green: wrapped encoded history originally made 6 cache hits for 2 pins;
  reuse makes 2. Review strengthened distinct row/slice contents, blank padding
  and mixed dirty rows; final three data rows pass. Independent read-only
  review found no remaining actionable issue.
- Final Release related tests 5/5, 14.63 s; actual GPU self-test passed.
  AddressSanitizer Core 92/92, 63.12 s; not UBSan or leak detection.
- Unprofiled interleaved A/B 24/24, 12288 requests, 96 pixel checks; sources
  and copied binaries stayed frozen. Cold-page hits about 11370 to 5800.
  ON hot P95 medians decreased about 20%/3%, while process CPU did not track
  that improvement; do not attribute the entire wall change to the patch.
  Fixed mixed ON/OFF=1.151 still fails 1.10. Budgets/defaults unchanged.
- WPR and display lease released. Traces/binaries/logs remain under build/;
  conclusions are in history-gui-hotpath-windows-2026-10-22/README.md.
  Subsequent user request: create the delivery commit as 0.2.79; do not push.
  No unrelated full suite, other platforms or real UART.
