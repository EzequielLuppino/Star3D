# Run the build in Azahar for N seconds, then pause it with GDB and print where
# the game thread is (backtrace, registers, disassembly). For hangs and crashes.
#
#   powershell -ExecutionPolicy Bypass -File tools\gdb_snapshot.ps1 [-Seconds 8] [-Rom path]
#
# Temporarily enables Azahar's GDB stub (port 24689) and restores the setting.
param(
    [int]$Seconds = 8,
    [string]$Rom = "",
    [string[]]$Eval = @()  # Extra GDB expressions to print, e.g. -Eval "gMyGlobal","*ptr"
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

try {
    $emu = Start-Process -FilePath $Azahar -ArgumentList "`"$Rom`"" -PassThru
    Start-Sleep -Seconds 3

    # First connection: let the game run (Azahar waits for a debugger to attach)
    $runner = Start-Process -FilePath $Gdb -PassThru -WindowStyle Hidden -ArgumentList @(
        "-batch", "-nx", "-ex", "`"set pagination off`"", "-ex", "`"target remote 127.0.0.1:$Port`"",
        "-ex", "continue", "`"$Elf`"")
    Write-Host "Running for $Seconds s..."
    Start-Sleep -Seconds $Seconds
    $runner | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500

    # Second connection: halts the emulated CPU and reports where it is
    Write-Host "===== gdb snapshot ====="
    $evalArgs = @()
    foreach ($e in $Eval) { $evalArgs += @("-ex", "print $e") }
    # gdb warnings on stderr must not abort the script (2>&1 turns them into errors)
    $ErrorActionPreference = "Continue"
    & $Gdb -batch -nx `
        -ex "set pagination off" `
        -ex "target remote 127.0.0.1:$Port" `
        -ex "echo \n--- backtrace ---\n" -ex "bt 20" `
        -ex "echo \n--- registers ---\n" -ex "info registers" `
        -ex "echo \n--- code at pc ---\n" -ex "x/12i `$pc" `
        -ex "echo \n--- source ---\n" -ex "list *`$pc" `
        -ex "echo \n--- eval ---\n" @evalArgs `
        $Elf 2>&1 | Where-Object { $_ -notmatch "^warning: (No executable|Unable to find)" }
}
finally {
    Get-Process azahar -ErrorAction SilentlyContinue | Stop-Process -Force
    Get-Process arm-none-eabi-gdb -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
    Set-GdbStub $false
}
