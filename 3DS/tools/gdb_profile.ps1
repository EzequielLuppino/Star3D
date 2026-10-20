# Sampling profiler: run the build in Azahar and repeatedly pause it with GDB,
# recording where the game thread is. Prints a histogram of functions.
#
#   powershell -ExecutionPolicy Bypass -File tools\gdb_profile.ps1 [-Warmup 20] [-Samples 25]
#
# Temporarily enables Azahar's GDB stub (port 24689) and restores the setting.
param(
    [int]$Warmup = 20,
    [int]$Samples = 25,
    [string]$Rom = ""
)

$ErrorActionPreference = "Stop"
$ProjectDir = Split-Path -Parent $PSScriptRoot
if (-not $Rom) { $Rom = Join-Path $ProjectDir "build\us_3ds\star3d.3dsx" }
$Elf = [IO.Path]::ChangeExtension($Rom, ".elf")
$Azahar = "C:\Program Files\Azahar\azahar.exe"
$Gdb = "C:\devkitPro\devkitARM\bin\arm-none-eabi-gdb.exe"
$Config = Join-Path $env:APPDATA "Azahar\config\qt-config.ini"
$Port = 24689
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Set-GdbStub([bool]$enabled) {
    $value = if ($enabled) { "true" } else { "false" }
    $lines = Get-Content $Config
    $lines = $lines -replace '^use_gdbstub\\default=.*$', 'use_gdbstub\default=false' `
                    -replace '^use_gdbstub=.*$', "use_gdbstub=$value"
    [IO.File]::WriteAllLines($Config, $lines, $Utf8NoBom)
}

Get-Process azahar -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
Set-GdbStub $true
$leaf = @{}
$caller = @{}

try {
    Start-Process -FilePath $Azahar -ArgumentList "`"$Rom`"" | Out-Null
    Start-Sleep -Seconds 3
    $runner = Start-Process -FilePath $Gdb -PassThru -WindowStyle Hidden -ArgumentList @(
        "-batch", "-nx", "-ex", "`"target remote 127.0.0.1:$Port`"", "-ex", "continue", "`"$Elf`"")
    Write-Host "Warming up for $Warmup s..."
    Start-Sleep -Seconds $Warmup
    $runner | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500

    for ($i = 0; $i -lt $Samples; $i++) {
        # Connecting halts the CPU; exiting gdb detaches and lets it run again
        $out = & $Gdb -batch -nx -ex "target remote 127.0.0.1:$Port" -ex "bt 3" -ex "detach" $Elf 2>&1
        $frames = @($out | Where-Object { $_ -match '^#\d' })
        $names = @($frames | ForEach-Object { if ($_ -match '(?:in )?([A-Za-z_][A-Za-z0-9_]*) \(') { $Matches[1] } })
        if ($names.Count -gt 0) { $leaf[$names[0]] = 1 + [int]$leaf[$names[0]] }
        if ($names.Count -gt 1) { $key = "$($names[1]) > $($names[0])"; $caller[$key] = 1 + [int]$caller[$key] }
        Write-Host -NoNewline "."
        Start-Sleep -Milliseconds 700
    }
    Write-Host "`n===== top functions ($Samples samples) ====="
    $leaf.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 15 |
        ForEach-Object { "{0,4}  {1}" -f $_.Value, $_.Key }
    Write-Host "===== caller > function ====="
    $caller.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 12 |
        ForEach-Object { "{0,4}  {1}" -f $_.Value, $_.Key }
}
finally {
    Get-Process azahar -ErrorAction SilentlyContinue | Stop-Process -Force
    Get-Process arm-none-eabi-gdb -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
    Set-GdbStub $false
}
