$ErrorActionPreference = 'Stop'
$candidate = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\build\LiquidDock.exe')).Path
$baseline = @(Get-CimInstance Win32_Process -Filter "Name = 'LiquidDock.exe'" | Select-Object ProcessId,ExecutablePath)
$parentPid = $PID

function Stop-TestWatchdog($process) {
    if ($process -and !$process.HasExited) {
        if ($process.Path -ne $candidate) { throw 'Unexpected test process path; refusing termination.' }
        Stop-Process -Id $process.Id
        $process.WaitForExit()
    }
    if ($process) { $process.Dispose() }
}

$eventName = 'Local\LiquidDock.WatchdogRegression.' + [guid]::NewGuid().ToString('D')
$ready = [Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$eventName)
$process = $null
try {
    $process = Start-Process -FilePath $candidate -ArgumentList "--watchdog $parentPid --ready $eventName" -WindowStyle Hidden -PassThru
    if (!$ready.WaitOne(5000)) { throw 'Watchdog did not acknowledge the live parent process.' }
    if ($process.HasExited) { throw 'Watchdog exited before the monitored parent.' }
    'PASS: release watchdog opens the live parent and acknowledges readiness'
} finally {
    # The parent remains alive until the test watchdog is stopped; recovery never runs.
    Stop-TestWatchdog $process
    $ready.Dispose()
}

$process = $null
try {
    $missing = 'Local\LiquidDock.MissingReady.' + [guid]::NewGuid().ToString('D')
    $process = Start-Process -FilePath $candidate -ArgumentList "--watchdog $parentPid --ready $missing" -WindowStyle Hidden -PassThru
    if (!$process.WaitForExit(5000) -or $process.ExitCode -eq 0) { throw 'Missing readiness event must fail safely.' }
    'PASS: missing readiness channel fails without attempting taskbar recovery'
} finally { Stop-TestWatchdog $process }

$after = @(Get-CimInstance Win32_Process -Filter "Name = 'LiquidDock.exe'" | Select-Object ProcessId,ExecutablePath)
foreach ($old in $baseline) {
    if (!($after | Where-Object { $_.ProcessId -eq $old.ProcessId -and $_.ExecutablePath -eq $old.ExecutablePath })) {
        throw 'A pre-existing dock process changed during the test.'
    }
}
'PASS: pre-existing dock and watchdog processes remained running'
