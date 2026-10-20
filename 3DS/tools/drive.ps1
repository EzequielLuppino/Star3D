# Drive a running (or freshly started) Azahar session from a script file.
#
#   powershell -ExecutionPolicy Bypass -File tools\drive.ps1 -Script steps.txt [-Start] [-Rom path] [-Close]
#
# Script lines:
#   @start                 (or -Start) launch Azahar via run_emu.ps1 -Interactive
#   @frame N               wait until the game log reaches frame N
#   @after N               wait until N frames past the frame at which this line runs
#   @log REGEX [TIMEOUT]   wait until a log line matches (default 300 s)
#   @sleep S               sleep S seconds (wall clock)
#   @shot NAME [COUNT MS]  screenshot of the live window -> build\emu\NAME.png
#   @close                 close Azahar and copy the logs to build\emu
#   anything else          a remote command (port/remote_3ds.c); consecutive
#                          ones are sent together, and the script waits until the
#                          game picked the file up
param(
    [Parameter(Mandatory)] [string]$Script,
    [string]$Rom = "C:\opencode\3DS\build-docker\us_3ds\star3d.3dsx",
    [switch]$Start,
    [switch]$Close
)

$ErrorActionPreference = "Stop"
$Tools = $PSScriptRoot
$OutDir = Join-Path (Split-Path -Parent $Tools) "build\emu"
$Sd = Join-Path $env:APPDATA "Azahar\sdmc"
$GameLog = Join-Path $Sd "3ds\star3d\Star3D.log"
$CmdFile = Join-Path $Sd "3ds\star3d\cmd.txt"

function Cur-Frame {
    if (-not (Test-Path $GameLog)) { return 0 }
    $tail = Get-Content $GameLog -Tail 40 -ErrorAction SilentlyContinue
    $f = 0
    foreach ($l in $tail) { if ($l -match '^\[\s*[\d.]+ f(\d+)') { $f = [int]$Matches[1] } }
    return $f
}

function Wait-Frame([int]$n, [int]$timeout = 600) {
    $deadline = (Get-Date).AddSeconds($timeout)
    while ((Cur-Frame) -lt $n) {
        if ((Get-Date) -gt $deadline) { Write-Host "timeout waiting for frame $n (at $(Cur-Frame))"; return }
        if (-not (Get-Process azahar -ErrorAction SilentlyContinue)) { throw "Azahar is gone" }
        Start-Sleep -Milliseconds 500
    }
}

function Send-Cmds([string[]]$cmds) {
    if ($cmds.Count -eq 0) { return }
    $deadline = (Get-Date).AddSeconds(60)
    while ((Test-Path $CmdFile) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
    [IO.File]::WriteAllText($CmdFile, ($cmds -join "`n") + "`n")
    $deadline = (Get-Date).AddSeconds(60)
    while ((Test-Path $CmdFile) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
}

function Close-Emu {
    $p = Get-Process azahar -ErrorAction SilentlyContinue
    if ($p) {
        $p | ForEach-Object { $_.CloseMainWindow() | Out-Null }
        Start-Sleep -Seconds 4
    }
    # CloseMainWindow often leaves Azahar running: force-kill until it is gone
    for ($i = 0; $i -lt 5 -and (Get-Process azahar -ErrorAction SilentlyContinue); $i++) {
        Stop-Process -Name azahar -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
    }
    if (Get-Process azahar -ErrorAction SilentlyContinue) { Write-Host "WARNING: Azahar is still running" }
    if (Test-Path $GameLog) { Copy-Item $GameLog (Join-Path $OutDir "game_log.txt") -Force }
}

if ($Start) {
    & powershell -ExecutionPolicy Bypass -File (Join-Path $Tools "run_emu.ps1") -Rom $Rom -Seconds 3 -Interactive | Out-Null
}

$pending = @()
foreach ($raw in Get-Content $Script) {
    $line = $raw.Trim()
    if (-not $line -or $line.StartsWith("#")) { continue }
    if (-not $line.StartsWith("@")) { $pending += $line; continue }
    Send-Cmds $pending; $pending = @()
    $parts = $line.Substring(1) -split '\s+'
    switch ($parts[0]) {
        "start" { & powershell -ExecutionPolicy Bypass -File (Join-Path $Tools "run_emu.ps1") -Rom $Rom -Seconds 3 -Interactive | Out-Null }
        "frame" { Wait-Frame ([int]$parts[1]) }
        "after" { Wait-Frame ((Cur-Frame) + [int]$parts[1]) }
        "log" {
            $re = $parts[1]; $to = 300; if ($parts.Count -gt 2) { $to = [int]$parts[2] }
            $deadline = (Get-Date).AddSeconds($to)
            while (-not (Select-String -Path $GameLog -Pattern $re -Quiet -ErrorAction SilentlyContinue)) {
                if ((Get-Date) -gt $deadline) { Write-Host "timeout waiting for /$re/"; break }
                Start-Sleep -Milliseconds 500
            }
        }
        "sleep" { Start-Sleep -Seconds ([double]$parts[1]) }
        "shot" {
            $a = @("-Name", $parts[1])
            if ($parts.Count -gt 2) { $a += @("-Count", $parts[2]) }
            if ($parts.Count -gt 3) { $a += @("-EveryMs", $parts[3]) }
            & powershell -ExecutionPolicy Bypass -File (Join-Path $Tools "emu_shot.ps1") @a | Out-Null
            Write-Host "shot $($parts[1]) at frame $(Cur-Frame)"
        }
        "close" { Close-Emu }
        default { Write-Host "unknown directive $line" }
    }
}
Send-Cmds $pending
if ($Close) { Close-Emu }
Write-Host "done at frame $(Cur-Frame)"
