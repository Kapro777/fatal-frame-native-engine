# safe-run.ps1 -- run the port safely on the Celeron notebook.
#
# The notebook is weak and this session runs on it, so the game process is
# constrained: idle priority, one core, bounded hang (the runtime's
# watchdog), hard kill after MAX_SECONDS, and a before/after snapshot of
# the game directory so any file the title writes (saves) is visible.
#
# Usage:  .\safe-run.ps1 [-Exe lethalshot_native.exe] [-MaxSeconds 60] [-Watchdog 20]

param(
    [string]$Exe = "lethalshot_native.exe",
    [int]$MaxSeconds = 60,
    [int]$Watchdog = 20
)

$ErrorActionPreference = 'Stop'
$runDir = $PSScriptRoot
if (Test-Path (Join-Path $runDir 'run')) { $runDir = Join-Path $runDir 'run' }
Set-Location $runDir

# --- snapshot the game tree (the kernel bridge redirects title I/O there) ---
function Snapshot($root) {
    Get-ChildItem $root -Recurse -File -ErrorAction SilentlyContinue |
        Select-Object -Property FullName, Length, LastWriteTime
}
$before = Snapshot "$runDir\game"

# --- constrained run ---
$env:RECOMP_WATCHDOG_SECS = "$Watchdog"
$env:RECOMP_KERNEL_LOG_BUDGET = "2000"
Remove-Item out.txt, err.txt -ErrorAction SilentlyContinue

$exePath = Join-Path (Get-Location) $Exe
if (-not (Test-Path $exePath)) { throw "executable not found: $exePath" }
$p = Start-Process -FilePath $exePath -WorkingDirectory (Get-Location) `
                   -RedirectStandardOutput out.txt -RedirectStandardError err.txt `
                   -PassThru -WindowStyle Hidden
try {
    $p.PriorityClass = 'Idle'                       # never crowd the notebook
    $p.ProcessorAffinity = 1                        # a single core
} catch {
    "note: could not constrain the process ($($_.Exception.Message))"
}

$sw = [Diagnostics.Stopwatch]::StartNew()
$cpuSw = [Diagnostics.Stopwatch]::StartNew()
$cpuBefore = $p.TotalProcessorTime
while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $MaxSeconds) {
    Start-Sleep -Milliseconds 250
}
$cpuAfter = $p.TotalProcessorTime
$sw.Stop()

if (-not $p.HasExited) {
    "HARD KILL after $MaxSeconds s (exit not reached)"
    Stop-Process -Id $p.Id -Force
} else {
    "exited by itself after $([math]::Round($sw.Elapsed.TotalSeconds,1)) s (code $($p.ExitCode))"
}
"cpu time: $([math]::Round(($cpuAfter - $cpuBefore).TotalSeconds,2)) s of $([math]::Round($sw.Elapsed.TotalSeconds,1)) s wall"

$after = Snapshot "$runDir\game"
$diff = Compare-Object $before $after -Property FullName, Length, LastWriteTime
if ($diff) {
    "=== files the title changed under game\ ==="
    $diff | ForEach-Object { "$($_.SideIndicator) $($_.FullName) $($_.Length)" }
} else {
    "no writes under game\"
}

"err.txt: $((Get-Item err.txt).Length) bytes   out.txt: $((Get-Item out.txt).Length) bytes"