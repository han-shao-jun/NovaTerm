param([string]$Executable = 'build/history-standalone/checks/novaterm_history_benchmark.exe', [string]$Output = 'docs/architecture/benchmarks/history-2026-10-17/off-on-final.json', [int]$Rows = 10000, [long]$Budget = 1073741824, [int]$PaceMilliseconds = 0)
$ErrorActionPreference = 'Stop'
$measurements = foreach ($mode in @('off','on')) {
    foreach ($corpus in @('repeat','variable','mixed','entropy','long')) {
        foreach ($run in 1..3) {
            $count = if ($corpus -eq 'long') { 100 } else { $Rows }
            $json = & $Executable $corpus $count $Budget $mode $PaceMilliseconds
            if ($LASTEXITCODE -ne 0) { throw "benchmark failed: $mode $corpus" }
            $sample = $json | ConvertFrom-Json
            $sample | Add-Member -NotePropertyName run -NotePropertyValue $run
            $sample
        }
    }
}
$measurements | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Output -Encoding utf8
$measurements | Select-Object mode,corpus,run,retained_lines,resident_charge,compressed_blocks,append_ms,scan_ms | Format-Table
