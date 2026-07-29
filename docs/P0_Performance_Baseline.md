# NovaTerm P0 Performance Baseline

Date: 2026-07-29

## Purpose

This baseline measures the current single-threaded `TerminalCore` before the
ScreenBuffer, VTAdapter, parser worker, and render scheduler changes planned
for P1 and P2.

It is intended for relative comparison between NovaTerm revisions. It is not
an end-to-end terminal FPS or transport benchmark.

## Build

- Platform: Windows x64
- Compiler: MSVC 19.44.35220
- Qt: 6.8.3
- Build type: Release
- Input size: 10 MiB
- Scrollback input: 100,000 lines

Configure and build:

```powershell
cmake -S . -B build/p0-release -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DBUILD_TESTING=ON `
  -DNOVATERM_BUILD_BENCHMARKS=ON
cmake --build build/p0-release `
  --target novaterm_core_tests novaterm_core_benchmark
```

Run:

```powershell
ctest --test-dir build/p0-release --output-on-failure
build/p0-release/bin/novaterm_core_benchmark.exe `
  --bytes 10485760 `
  --lines 100000
```

## Results

| Metric | P0 baseline |
| --- | ---: |
| Core tests | 4 passed, 0 failed |
| Parser throughput | 11.52 MiB/s |
| Parser elapsed time for 10 MiB | 868.07 ms |
| Scrollback elapsed time for 100,000 lines | 337.53 ms |
| Stored scrollback lines | 100,000 |

The parser workload contains SGR sequences, ASCII text, UTF-8 CJK text, and
line breaks. The scrollback workload writes fixed terminal lines through
`TerminalCore::writeInput()`, so it includes libvterm parsing and scrollback
callbacks rather than measuring `ScrollbackBuffer` alone.

## Interpretation

The P0 parser result is below the architecture target of more than 20 MiB/s.
This confirms that parser batching, data ownership, and thread separation
should be measured during P1/P2 rather than optimized without a baseline.

The current benchmark does not measure:

- Transport read throughput or backpressure
- UI event-loop latency
- Dirty-region merge efficiency
- Render-command generation
- GPU frame time or FPS
- Process memory usage

Those measurements should be added when the corresponding boundaries exist.
