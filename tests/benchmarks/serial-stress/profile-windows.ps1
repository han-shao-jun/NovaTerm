<#
@file profile-windows.ps1
@brief WPR CPU 采样：只在串口数据阶段录制，保证停止自己启动的会话。
#>
param(
    [string]$ExecutablePath,
    [string]$OutputDirectory,
    [string]$QtPrefix = 'C:\Programs\Qt\6.8.3\msvc2022_64',
    [string[]]$Modes = @('visible','hidden')
)
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
if (-not $ExecutablePath) { $ExecutablePath = Join-Path $repoRoot 'build/serial-stress-symbols/serial_stress.exe' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repoRoot 'build/serial-stress-profile/captures' }
$env:PATH = (Join-Path $QtPrefix 'bin') + ';' + $env:PATH
$env:QT_PLUGIN_PATH = Join-Path $QtPrefix 'plugins'
$env:NOVATERM_RHI_API = 'd3d11'
$env:QT_WIDGETS_RHI = '1'
Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
& $ExecutablePath --self-test
if ($LASTEXITCODE -ne 0) { throw 'Verifier self-test failed' }
foreach ($mode in $Modes) {
    if ($mode -notin @('visible','hidden','raw')) { throw "Invalid mode: $mode" }
    $base = Join-Path $OutputDirectory ("{0}-{1}" -f $mode, (Get-Date -Format 'HHmmss'))
    $resultPath = "$base.json"
    $tracePath = "$base.etl"
    $arguments = @('COM1','COM2','8000000','20',$mode,'paced',$resultPath,
        '--bytes','67108864','--driver-rx-queue','262144','--stable-timers',
        '--deferred-verify','--profile-handshake',$base)
    # 仅隐藏控制台，不向 Qt 顶层窗口传 SW_HIDE，否则可见验收无法暴露窗口。
    $serialProfileLaunch = [System.Diagnostics.ProcessStartInfo]::new()
    $serialProfileLaunch.FileName = $ExecutablePath
    $serialProfileLaunch.WorkingDirectory = $repoRoot
    $serialProfileLaunch.UseShellExecute = $false
    $serialProfileLaunch.CreateNoWindow = $true
    $serialProfileLaunch.RedirectStandardOutput = $true
    $serialProfileLaunch.RedirectStandardError = $true
    $serialProfileLaunch.Arguments = ($arguments | ForEach-Object { '"' + $_.Replace('"','\"') + '"' }) -join ' '
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $serialProfileLaunch
    if (-not $process.Start()) { throw 'Failed to launch benchmark' }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $started = $false
    try {
        $wait = [Diagnostics.Stopwatch]::StartNew()
        while (-not (Test-Path -LiteralPath "$base.ready")) {
            if ($process.HasExited) { throw 'Benchmark exited before profiler handshake' }
            if ($wait.Elapsed.TotalSeconds -gt 120) { throw 'Benchmark setup timed out' }
            Start-Sleep -Milliseconds 100
        }
        $status = (& wpr -status 2>&1 | Out-String)
        if ($status -notmatch 'WPR is not recording') { throw 'Another WPR session is active; it will not be interrupted' }
        & wpr -start CPU -filemode
        if ($LASTEXITCODE -ne 0) { throw "WPR start failed: $LASTEXITCODE" }
        $started = $true
        'go' | Set-Content -LiteralPath "$base.go" -Encoding ASCII
        Write-Output "Recording $mode PID=$($process.Id), 8M/64MiB, verified RX=256KiB"
        $wait.Restart()
        while (-not (Test-Path -LiteralPath "$base.done")) {
            if ($process.HasExited) { throw 'Benchmark exited during recording' }
            if ($wait.Elapsed.TotalSeconds -gt 150) { throw 'Input phase timed out' }
            Start-Sleep -Milliseconds 100
        }
    } finally {
        if ($started) {
            & wpr -stop $tracePath
            $stopStatus = $LASTEXITCODE
            $started = $false
            if ($stopStatus -ne 0) { Write-Error "WPR stop failed: $stopStatus" }
        }
        'release' | Set-Content -LiteralPath "$base.release" -Encoding ASCII
        if (Test-Path -LiteralPath "$base.done") {
            if (-not $process.WaitForExit(60000)) { Stop-Process -Id $process.Id -Force; throw 'Benchmark result timed out' }
        } elseif (-not $process.HasExited) {
            # 只终止本脚本创建的进程，不影响其他 NovaTerm 或录制会话。
            Stop-Process -Id $process.Id -Force
        }
        [IO.File]::WriteAllText("$base.stdout.txt", $stdoutTask.GetAwaiter().GetResult())
        [IO.File]::WriteAllText("$base.stderr.txt", $stderrTask.GetAwaiter().GetResult())
    }
    if ($process.ExitCode -ne 0) { throw "Benchmark failed: $($process.ExitCode)" }
    $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
    if (-not $result.pass -or -not $result.driverQueueExclusionConfirmed) { throw 'Profiled input completeness failed' }
    Write-Output "Captured $tracePath; frames=$($result.validFrames), loss=$($result.missingFrames)"
}
