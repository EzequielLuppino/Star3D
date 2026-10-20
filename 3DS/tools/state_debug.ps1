# Boot the build in Azahar, load the quick-save state (slot 0), let it run and
# take screenshots, then optionally halt it with GDB and print expressions.
# The ROM/ELF must be the exact build the state was saved with.
#
#   powershell -ExecutionPolicy Bypass -File tools\state_debug.ps1 [-Seconds 6] [-Rom path]
#       [-ShotEvery 1] [-Gdb] [-Eval "expr","expr"] [-GdbCmds file] [-NoQuickLoad]
#
# Quick Load has no default hotkey; the script binds it to F12 for the run and
# restores qt-config.ini afterwards. Output goes to build\emu\ (state_NNN.png).
# -NoQuickLoad keeps the fresh boot (to drive a new build with the GDB hooks,
# since a savestate only works with the build it was made with).
param(
    [int]$Seconds = 6,
    [string]$Rom = "",
    [int]$ShotEvery = 1,
    [switch]$Gdb,
    [switch]$NoQuickLoad,
    [string[]]$Eval = @(),
    [string]$GdbCmds = "",     # File with extra gdb commands, run after halting
    [int]$GdbAt = 0,           # Also halt at this second (GdbAtCmds), then keep running
    [string]$GdbAtCmds = ""    # Commands for the -GdbAt halt; must end with "detach"
)

$ErrorActionPreference = "Stop"
$ProjectDir = Split-Path -Parent $PSScriptRoot
if (-not $Rom) { $Rom = Join-Path $ProjectDir "build\us_3ds\star3d.3dsx" }
$Elf = [IO.Path]::ChangeExtension($Rom, ".elf")
$Azahar = "C:\Program Files\Azahar\azahar.exe"
$GdbExe = "C:\devkitPro\devkitARM\bin\arm-none-eabi-gdb.exe"
$Config = Join-Path $env:APPDATA "Azahar\config\qt-config.ini"
$GameLog = Join-Path $env:APPDATA "Azahar\sdmc\3ds\star3d\Star3D.log"
$OutDir = Join-Path $ProjectDir "build\emu"
$Port = 24689
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
New-Item -ItemType Directory -Force $OutDir | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win32S {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int cmd);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr w, IntPtr l);
}
"@
[Win32S]::SetProcessDPIAware() | Out-Null

function Save-Window($hwnd, $path) {
    $r = New-Object Win32S+RECT
    [Win32S]::GetWindowRect($hwnd, [ref]$r) | Out-Null
    $w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
    if ($w -le 0 -or $h -le 0) { return }
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [Win32S]::PrintWindow($hwnd, $hdc, 2) | Out-Null
    $g.ReleaseHdc($hdc)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
}

$configBackup = [IO.File]::ReadAllText($Config)
function Set-Config {
    $lines = Get-Content $Config
    $lines = $lines -replace '^use_gdbstub\\default=.*$', 'use_gdbstub\default=false' `
                    -replace '^use_gdbstub=.*$', ("use_gdbstub=" + $(if ($Gdb) { "true" } else { "false" })) `
                    -replace '^(Shortcuts\\Main%20Window\\Quick%20Load\\KeySeq)\\default=.*$', '$1\default=false' `
                    -replace '^(Shortcuts\\Main%20Window\\Quick%20Load\\KeySeq)=.*$', '$1=F12'
    [IO.File]::WriteAllLines($Config, $lines, $Utf8NoBom)
}

Get-Process azahar -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
Set-Config
Get-ChildItem $OutDir -Filter "state_*.png" -ErrorAction SilentlyContinue | Remove-Item -Force

try {
    $emu = Start-Process -FilePath $Azahar -ArgumentList "`"$Rom`"" -PassThru
    $deadline = (Get-Date).AddSeconds(20)
    while ($emu.MainWindowHandle -eq 0 -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250; $emu.Refresh() }
    $runner = $null
    if ($Gdb) {
        Start-Sleep -Seconds 2
        $runner = Start-Process -FilePath $GdbExe -PassThru -WindowStyle Hidden -ArgumentList @(
            "-batch", "-nx", "-ex", "`"target remote 127.0.0.1:$Port`"", "-ex", "continue", "`"$Elf`"")
    }
    $hwnd = $emu.MainWindowHandle
    if (-not $NoQuickLoad) {
        Start-Sleep -Seconds 4   # let the app boot before replacing its state
        [Win32S]::ShowWindow($hwnd, 9) | Out-Null
        [Win32S]::SetForegroundWindow($hwnd) | Out-Null
        Start-Sleep -Milliseconds 400
        # Post the key to the window: SendKeys goes to whichever window has focus,
        # and Windows refuses SetForegroundWindow while the user is typing elsewhere
        [Win32S]::PostMessage($hwnd, 0x100, [IntPtr]0x7B, [IntPtr]0x00580001) | Out-Null   # WM_KEYDOWN F12
        Start-Sleep -Milliseconds 100
        [Win32S]::PostMessage($hwnd, 0x101, [IntPtr]0x7B, [IntPtr]0xC0580001) | Out-Null   # WM_KEYUP
        Write-Host "Quick Load sent"
    }

    function Invoke-GdbHalt([string]$cmds) {
        $runner | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 500
        $args2 = @("-batch", "-nx", "-ex", "set pagination off", "-ex", "target remote 127.0.0.1:$Port",
                   "-ex", "bt 8")
        foreach ($e in $Eval) { $args2 += @("-ex", "print $e") }
        if ($cmds) { $args2 += @("-x", $cmds) }
        $ErrorActionPreference = "Continue"
        & $GdbExe @args2 $Elf 2>&1 | Where-Object { $_ -notmatch "^warning: (No executable|Unable to find)" }
        $ErrorActionPreference = "Stop"
    }

    for ($t = 1; $t -le $Seconds; $t++) {
        Start-Sleep -Seconds 1
        if ($ShotEvery -gt 0 -and ($t % $ShotEvery) -eq 0) {
            Save-Window $hwnd (Join-Path $OutDir ("state_{0:D3}.png" -f $t))
        }
        if ($Gdb -and $GdbAt -eq $t) {
            Invoke-GdbHalt $GdbAtCmds
            # gdb detached (the commands file must end with "detach"); keep the game running
            $runner = Start-Process -FilePath $GdbExe -PassThru -WindowStyle Hidden -ArgumentList @(
                "-batch", "-nx", "-ex", "`"target remote 127.0.0.1:$Port`"", "-ex", "continue", "`"$Elf`"")
        }
    }

    if ($Gdb) {
        Invoke-GdbHalt $GdbCmds
    }
}
finally {
    Get-Process azahar -ErrorAction SilentlyContinue | Stop-Process -Force
    Get-Process arm-none-eabi-gdb -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
    [IO.File]::WriteAllText($Config, $configBackup, $Utf8NoBom)
    if (Test-Path $GameLog) { Copy-Item $GameLog (Join-Path $OutDir "game_log.txt") -Force }
}
