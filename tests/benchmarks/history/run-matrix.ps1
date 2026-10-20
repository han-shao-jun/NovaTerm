param(
    [string]$Executable = 'build/history-isolated-final/checks/novaterm_history_benchmark.exe',
    [string]$Output = 'build/history-performance.json',
    [ValidateRange(1,10000000)][int]$Rows = 10000,
    [ValidateRange(1,10000000)][int]$LongRows = 100,
    [ValidateRange(1,9223372036854775807)][long]$Budget = 1073741824,
    [ValidateRange(0,1000)][int]$PaceMilliseconds = 0,
    [ValidateRange(3,100)][int]$Runs = 3,
    [ValidateRange(1,10000)][int]$ReadSamples = 64,
    [ValidateSet('repeat','variable','mixed','entropy','long','rewrite')]
    [string[]]$Corpora = @('repeat','variable','mixed','entropy','long','rewrite'),
    [ValidateSet('SameContent','EqualBudget')][string]$Scenario = 'SameContent',
    [switch]$EnforceGates
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$exe = (Resolve-Path -LiteralPath $Executable).Path
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../..')).Path
$revision = (& git -c "safe.directory=$repo" -C $repo rev-parse HEAD) -join ''
if ($LASTEXITCODE -ne 0) { throw 'cannot identify repository revision' }
$dirty = @(& git -c "safe.directory=$repo" -C $repo status --porcelain).Count -gt 0
function Distribution($Values) {
    $sorted = @($Values | Sort-Object)
    $n = $sorted.Count
    $median = if ($n % 2) { $sorted[[int][Math]::Floor($n / 2)] }
        else { ($sorted[$n / 2 - 1] + $sorted[$n / 2]) / 2.0 }
    [ordered]@{ median = $median; min = $sorted[0]; max = $sorted[-1] }
}
$started = [DateTime]::UtcNow
$measurements = @(foreach ($corpus in $Corpora) {
    foreach ($run in 1..$Runs) {
        # 轮换 OFF/ON 顺序，避免模式固定在同一温度/负载阶段。
        $modes = if ($run % 2) { @('off','on') } else { @('on','off') }
        foreach ($mode in $modes) {
            $count = if ($corpus -eq 'long') { $LongRows } else { $Rows }
            $json = & $exe $corpus $count $Budget $mode $PaceMilliseconds $ReadSamples
            if ($LASTEXITCODE -ne 0) { throw "benchmark failed: $mode $corpus run $run" }
            $sample = ($json -join "`n") | ConvertFrom-Json
            if ($sample.schema_version -ne 2 -or !$sample.verified -or $sample.build_type -ne 'Release') {
                throw 'matrix requires verified schema 2 Release samples'
            }
            if ($mode -eq 'on' -and $sample.admitted -ne 1) { throw 'ON admission failed' }
            if ($sample.retained_hash -ne $sample.expected_retained_hash) { throw 'retained hash mismatch' }
            if ($sample.rows -ne $count -or $sample.budget -ne $Budget -or $sample.corpus -ne $corpus -or $sample.mode -ne $mode) {
                throw 'sample parameters differ from request'
            }
            $sample | Add-Member -NotePropertyName run -NotePropertyValue $run
            $sample
        }
    }
})
$metrics = @('resident_charge','rss','peak_rss','retained_lines','append_ms','append_wall_ms',
    'ingest_cpu_ms','ingest_wall_ms','append_cell_mib_per_s','batch_p95_us','batch_p99_us','settle_ms',
    'scan_ms','scan_cpu_ms','scan_cell_mib_per_s','read_p95_us','read_p99_us',
    'cold_p95_us','cold_p99_us','cold_encoded_p95_us','cold_encoded_p99_us','hot_p95_us','hot_p99_us','index_ms',
    'idle_store_close_ms','idle_service_stop_ms','compressed_blocks','cold_encoded_samples')
$summaries = @(foreach ($corpus in $Corpora) {
    foreach ($mode in @('OFF','ON')) {
        $group = @($measurements | Where-Object { $_.corpus -eq $corpus -and $_.mode -eq $mode })
        $values = [ordered]@{}
        foreach ($metric in $metrics) { $values[$metric] = Distribution @($group | ForEach-Object { $_.$metric }) }
        [pscustomobject]@{ corpus = $corpus; mode = $mode; runs = $group.Count; metrics = $values }
    }
})
$comparisons = @(foreach ($corpus in $Corpora) {
    $group = @($measurements | Where-Object { $_.corpus -eq $corpus })
    if (@($group.hash_fnv1a64 | Select-Object -Unique).Count -ne 1) { throw "input differs: $corpus" }
    $sameContent = @($group | Where-Object { $_.retained_lines -ne $_.rows -or $_.verified_cells -ne $_.input_cells }).Count -eq 0
    if ($sameContent -and @($group.retained_hash | Select-Object -Unique).Count -ne 1) { throw "OFF/ON content differs: $corpus" }
    $off = ($summaries | Where-Object { $_.corpus -eq $corpus -and $_.mode -eq 'OFF' }).metrics
    $on = ($summaries | Where-Object { $_.corpus -eq $corpus -and $_.mode -eq 'ON' }).metrics
    $saving = 1.0 - $on.resident_charge.median / [double]$off.resident_charge.median
    $appendRatio = $on.append_cell_mib_per_s.median / [double]$off.append_cell_mib_per_s.median
    $scanRatio = $on.scan_cell_mib_per_s.median / [double]$off.scan_cell_mib_per_s.median
    $retentionRatio = $on.retained_lines.median / [double]$off.retained_lines.median
    $coldMeasured = @($group | Where-Object { $_.mode -eq 'ON' -and $_.cold_encoded_samples -eq 0 }).Count -eq 0
    # 这些仅是后端合成门槛，不能据此宣布 UART/Parser/GUI 发布达标。
    $gates = [ordered]@{}
    if ($Scenario -eq 'SameContent') {
        $gates.full_content = $sameContent
        $gates.append_ratio_ge_0_90 = $sameContent -and $appendRatio -ge 0.90
        $gates.scan_ratio_ge_0_80 = $sameContent -and $scanRatio -ge 0.80
        if ($corpus -in @('repeat','variable')) { $gates.charge_saving_ge_0_40 = $sameContent -and $saving -ge 0.40 }
    }
    if ($coldMeasured) {
        # 要求每轮均通过，并分别保留每轮分位数及其范围。
        $gates.cold_p95_le_50ms = $on.cold_encoded_p95_us.max -le 50000
        $gates.cold_p99_le_100ms = $on.cold_encoded_p99_us.max -le 100000
    }
    [pscustomobject]@{
        corpus = $corpus; full_content = $sameContent; charge_saving_fraction = $saving
        append_throughput_on_over_off = $appendRatio; scan_throughput_on_over_off = $scanRatio
        retained_lines_on_over_off = $retentionRatio; cold_compressed_measured = $coldMeasured
        gates = $gates
    }
})
$cpu = $null; $gpu = $null; $os = [Environment]::OSVersion.VersionString
if (Get-Command Get-CimInstance -ErrorAction SilentlyContinue) {
    try {
        $cpu = (Get-CimInstance Win32_Processor | Select-Object -ExpandProperty Name) -join '; '
        $gpu = (Get-CimInstance Win32_VideoController | Select-Object -ExpandProperty Name) -join '; '
    } catch { Write-Warning 'hardware metadata unavailable; fill it before publishing results' }
}
$failed = @($comparisons | Where-Object { @($_.gates.Values | Where-Object { $_ -eq $false }).Count -gt 0 })
$report = [ordered]@{
    schema_version = 2; target_commit = 'f662302aa964e81c4ea5b9aefda1bbe3b2f2189b'
    source_revision = $revision; working_tree_dirty = $dirty; scenario = $Scenario
    started_utc = $started.ToString('o'); elapsed_seconds = ([DateTime]::UtcNow - $started).TotalSeconds
    environment = [ordered]@{ os = $os; cpu = $cpu; gpu = $gpu; qt = 'not linked'; executable = $exe }
    measurement = [ordered]@{
        append = 'sum of append/continuation + processHistory + publish calls; excludes generation/hash/pacing'
        ingest_cpu = 'process user+kernel including worker, corpus generation, hash, ingestion and settle; not receive CPU'
        scan = 'before cold-cache sampling: sequential cursor reads + chars[0] checksum; excludes full-field verification'
        throughput = 'Cell storage MiB/s (sizeof(Cell)); not transport byte throughput'
        cold = 'one distinct sampled block each, CacheOnly request to Found; includes worker scheduling; not GUI frame'
        memory = 'resident_charge before reads; RSS after ingestion; peak RSS includes corpus batch/read/verification'
        percentiles = 'nearest rank per process; summary gives median/min/max of process percentiles'
        close = 'idle store destruction and idle service stop; excludes queued-work close latency'
    }
    executable_sha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    benchmark_sources = @(foreach ($name in @('HistoryBenchmark.cpp','HistoryBenchmarkSupport.h','run-matrix.ps1')) {
        [ordered]@{ file = $name; sha256 = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $name) -Algorithm SHA256).Hash }
    })
    samples = $measurements; summaries = $summaries; comparisons = $comparisons
    synthetic_gates_passed = ($failed.Count -eq 0)
    unmeasured = @('transport/Parser throughput','fixed-rate receive CPU','GUI frame latency','search/reflow/cancel',
        'model lock P99','queued-work close latency','1/4/8-session 30min soak','real UART','other platforms')
}
$outputPath = [IO.Path]::GetFullPath($Output)
$null = New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($outputPath))
$report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $outputPath -Encoding utf8
$comparisons | Select-Object corpus,charge_saving_fraction,append_throughput_on_over_off,scan_throughput_on_over_off,retained_lines_on_over_off | Format-Table
Write-Host "Report: $outputPath"
if ($EnforceGates -and $failed.Count) { throw 'synthetic performance gates failed; see saved report' }
