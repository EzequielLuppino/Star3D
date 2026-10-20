# Screenshot of the running Azahar window (it is left running), see also
# port/remote_3ds.c to drive it with sdmc:/3ds/star3d/cmd.txt.
#
#   powershell -ExecutionPolicy Bypass -File tools\emu_shot.ps1 [-Name shot] [-Count 1] [-EveryMs 500]
#
# Writes build\emu\<Name>.png, or <Name>_NN.png with -Count > 1.
param(
    [string]$Name = "live",
    [int]$Count = 1,
    [int]$EveryMs = 500
)

$ErrorActionPreference = "Stop"
$OutDir = Join-Path (Split-Path -Parent $PSScriptRoot) "build\emu"
New-Item -ItemType Directory -Force $OutDir | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win32Shot {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int cmd);
}
"@
[Win32Shot]::SetProcessDPIAware() | Out-Null

$proc = Get-Process azahar -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $proc) { throw "Azahar is not running" }
$hwnd = $proc.MainWindowHandle
# A minimized window prints as its title bar only (160x28): restore it first
if ([Win32Shot]::IsIconic($hwnd)) { [Win32Shot]::ShowWindow($hwnd, 9) | Out-Null; Start-Sleep -Milliseconds 500 }

for ($i = 0; $i -lt $Count; $i++) {
    $r = New-Object Win32Shot+RECT
    [Win32Shot]::GetWindowRect($hwnd, [ref]$r) | Out-Null
    $w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [Win32Shot]::PrintWindow($hwnd, $hdc, 2) | Out-Null  # works even when covered
    $g.ReleaseHdc($hdc)
    $file = if ($Count -gt 1) { "{0}_{1:D2}.png" -f $Name, $i } else { "$Name.png" }
    $bmp.Save((Join-Path $OutDir $file), [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Host (Join-Path $OutDir $file)
    if ($i + 1 -lt $Count) { Start-Sleep -Milliseconds $EveryMs }
}
