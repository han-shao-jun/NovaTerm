param(
    [switch]$Smoke,
    [switch]$Diagnostics,
    [switch]$ChunkControls,
    [switch]$LongCases,
    [switch]$TimingControls,
    [switch]$StableRawCase,
    [switch]$StableTerminalCase,
    [switch]$DriverQueueProbe,
    [switch]$LargeQueueCases,
    [string]$OutputDirectory,
    [string]$QtPrefix = 'C:\Programs\Qt\6.8.3\msvc2022_64',
    [string]$ExecutablePath,
    [string]$PortA = 'COM1',
    [string]$PortB = 'COM2'
)
$ErrorActionPreference = 'Stop'
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
if (-not $OutputDirectory) {
    $resultGroup = if ($DriverQueueProbe -or $LargeQueueCases) { 'large-queue' } elseif ($TimingControls -or $StableRawCase -or $StableTerminalCase) { 'timing-controls' } elseif ($ChunkControls) { 'block-controls' } elseif ($Diagnostics) { 'controls' } elseif ($Smoke) { 'smoke' } else { 'matrix' }
    $OutputDirectory = Join-Path $repoRoot "build/serial-stress-results/$resultGroup"
}
if (-not $ExecutablePath) { $ExecutablePath = Join-Path $repoRoot 'build/serial-stress-native/serial_stress.exe' }
$env:PATH = (Join-Path $QtPrefix 'bin') + ';' + $env:PATH
$env:QT_PLUGIN_PATH = Join-Path $QtPrefix 'plugins'
$env:NOVATERM_RHI_API = 'd3d11'
$env:QT_WIDGETS_RHI = '1'
Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue
$stressExe = $ExecutablePath
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
& $stressExe --self-test
if ($LASTEXITCODE -ne 0) { throw 'Frame verifier self-test failed' }
$failures = 0
if ($DriverQueueProbe) {
    & $stressExe $PortA $PortB 8000000 1 visible paced "$OutputDirectory\queue-probe.json" --bytes 1048576 --driver-rx-queue 262144 --stable-timers --deferred-verify
    if ($LASTEXITCODE -ne 0) { $failures++ }
} elseif ($LargeQueueCases) {
    foreach ($direction in @(@($PortA,$PortB),@($PortB,$PortA))) {
        foreach ($mode in @('visible','hidden','raw')) {
            $name = "8000000-$($direction[0])-$($direction[1])-$mode-stable-deferred-paced-67108864-rx262144"
            Write-Output "Starting enlarged queue $name"
            & $stressExe $direction[0] $direction[1] 8000000 20 $mode paced "$OutputDirectory\$name.json" --bytes 67108864 --driver-rx-queue 262144 --stable-timers --deferred-verify
            if ($LASTEXITCODE -eq 7) { throw 'Driver queue enlargement rejected; stop matrix' }
            if ($LASTEXITCODE -ne 0) { $failures++ }
        }
    }
} elseif ($StableTerminalCase) {
    $name = "8000000-$PortA-$PortB-visible-stable-deferred-paced-67108864"
    & $stressExe $PortA $PortB 8000000 20 visible paced "$OutputDirectory\$name.json" --bytes 67108864 --stable-timers --deferred-verify
    if ($LASTEXITCODE -ne 0) { $failures++ }
} elseif ($StableRawCase) {
    $name = "8000000-$PortA-$PortB-raw-stable-paced-67108864"
    & $stressExe $PortA $PortB 8000000 20 raw paced "$OutputDirectory\$name.json" --bytes 67108864 --stable-timers
    if ($LASTEXITCODE -ne 0) { $failures++ }
} elseif ($TimingControls) {
    foreach ($mode in @('raw','visible')) {
        $extraArgs = if ($mode -eq 'visible') { @('--stable-timers') } else { @() }
        $name = "8000000-$PortA-$PortB-$mode-paced-67108864"
        Write-Output "Starting timing control $name"
        & $stressExe $PortA $PortB 8000000 20 $mode paced "$OutputDirectory\$name.json" --bytes 67108864 $extraArgs
        if ($LASTEXITCODE -ne 0) { $failures++ }
    }
    $name = "8000000-$PortA-$PortB-raw-stable-paced-67108864"
    & $stressExe $PortA $PortB 8000000 20 raw paced "$OutputDirectory\$name.json" --bytes 67108864 --stable-timers
    if ($LASTEXITCODE -ne 0) { $failures++ }
    $name = "8000000-$PortA-$PortB-visible-stable-deferred-paced-67108864"
    & $stressExe $PortA $PortB 8000000 20 visible paced "$OutputDirectory\$name.json" --bytes 67108864 --stable-timers --deferred-verify
    if ($LASTEXITCODE -ne 0) { $failures++ }
} elseif ($LongCases) {
    foreach ($direction in @(@($PortA,$PortB),@($PortB,$PortA))) {
        $name = "8000000-$($direction[0])-$($direction[1])-visible-paced-67108864"
        Write-Output "Starting sustained $name"
        & $stressExe $direction[0] $direction[1] 8000000 20 visible paced "$OutputDirectory\$name.json" --bytes 67108864
        if ($LASTEXITCODE -ne 0) { $failures++ }
    }
} elseif ($ChunkControls) {
    foreach ($port in @($PortA,$PortB)) {
        & $stressExe --probe $port "$OutputDirectory\driver-$port.json"
        if ($LASTEXITCODE -ne 0) { $failures++ }
    }
    foreach ($direction in @(@($PortA,$PortB),@($PortB,$PortA))) {
        foreach ($mode in @('visible','raw')) {
            $name = "8000000-$($direction[0])-$($direction[1])-$mode-deferred-burst-8388608-chunk4096"
            Write-Output "Starting block control $name"
            & $stressExe $direction[0] $direction[1] 8000000 20 $mode burst "$OutputDirectory\$name.json" --bytes 8388608 --deferred-verify --tx-chunk 4096
            if ($LASTEXITCODE -ne 0) { $failures++ }
        }
    }
} elseif ($Diagnostics) {
    foreach ($direction in @(@($PortA,$PortB),@($PortB,$PortA))) {
        foreach ($mode in @('visible','raw')) {
            $name = "8000000-$($direction[0])-$($direction[1])-$mode-deferred-burst-67108864"
            Write-Output "Starting control $name"
            & $stressExe $direction[0] $direction[1] 8000000 20 $mode burst "$OutputDirectory\$name.json" --bytes 67108864 --deferred-verify
            if ($LASTEXITCODE -ne 0) { $failures++ }
        }
    }
} elseif ($Smoke) {
    foreach ($visibility in @('hidden', 'visible')) {
        & $stressExe $PortA $PortB 1000000 1 $visibility paced "$OutputDirectory\smoke-$visibility.json"
        if ($LASTEXITCODE -ne 0) { $failures++ }
    }
} else {
    foreach ($baud in @(1000000,3000000,5000000,8000000)) {
        foreach ($direction in @(@($PortA,$PortB),@($PortB,$PortA))) {
            foreach ($dataBytes in @(65536,1048576,8388608)) {
                $name = "$baud-$($direction[0])-$($direction[1])-visible-paced-$dataBytes"
                Write-Output "Starting $name"
                & $stressExe $direction[0] $direction[1] $baud 20 visible paced "$OutputDirectory\$name.json" --bytes $dataBytes
                if ($LASTEXITCODE -ne 0) { $failures++ }
            }
            $name = "$baud-$($direction[0])-$($direction[1])-hidden-paced-8388608"
            Write-Output "Starting $name"
            & $stressExe $direction[0] $direction[1] $baud 20 hidden paced "$OutputDirectory\$name.json" --bytes 8388608
            if ($LASTEXITCODE -ne 0) { $failures++ }
        }
    }
    foreach ($direction in @(@($PortA,$PortB),@($PortB,$PortA))) {
        $name = "8000000-$($direction[0])-$($direction[1])-visible-paced-67108864"
        Write-Output "Starting sustained $name"
        & $stressExe $direction[0] $direction[1] 8000000 20 visible paced "$OutputDirectory\$name.json" --bytes 67108864
        if ($LASTEXITCODE -ne 0) { $failures++ }
        $name = "8000000-$($direction[0])-$($direction[1])-visible-burst"
        & $stressExe $direction[0] $direction[1] 8000000 20 visible burst "$OutputDirectory\$name.json" --bytes 67108864
        if ($LASTEXITCODE -ne 0) { $failures++ }
    }
}
Write-Output "Matrix finished: failures=$failures"
exit $failures
