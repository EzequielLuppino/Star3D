# Run the 3DS build in Azahar, capture a screenshot and collect logs.
#
#   powershell -ExecutionPolicy Bypass -File tools\run_emu.ps1 [-Seconds 15] [-Rom path] [-Interactive]
#
# Outputs (in build\emu\):
#   screenshot.png   Azahar window just before it is closed
#   shot_NNN.png     With -ShotEvery N: a screenshot every N seconds (NNN = seconds)
#   game_log.txt     The port's own log (sdmc:/3ds/star3d/Star3D.log)
#   azahar_log.txt   Emulator log (includes svcOutputDebugString as Debug.Emulated)
# Crash addresses found in the emulator log are resolved with addr2line.
param(
    [int]$Seconds = 15,
    [string]$Rom = "",
    [switch]$Interactive,  # Leave the emulator running instead of closing it
    [int]$ShotEvery = 0,   # Periodic screenshots (seconds), 0 = only the final one
    [int]$DumpFrame = 0,   # Log a GX batch dump of this render frame (gx_render.c)
    [int]$Burst = 0        # Before closing: this many screenshots ~50 ms apart (burst_NN.png)
)

$ErrorActionPreference = "Stop"
$ProjectDir = Split-Path -Parent $PSScriptRoot
if (-not $Rom) { $Rom = Join-Path $ProjectDir "build\us_3ds\star3d.3dsx" }
$Elf = [IO.Path]::ChangeExtension($Rom, ".elf")
$Azahar = "C:\Program Files\Azahar\azahar.exe"
$AzaharData = Join-Path $env:APPDATA "Azahar"
$GameLog = Join-Path $AzaharData "sdmc\3ds\star3d\Star3D.log"
$EmuLog = Join-Path $AzaharData "log\azahar_log.txt"
$OutDir = Join-Path $ProjectDir "build\emu"
$Addr2Line = "C:\devkitPro\devkitARM\bin\arm-none-eabi-addr2line.exe"

if (-not (Test-Path $Rom)) { throw "ROM not found: $Rom (build first)" }
New-Item -ItemType Directory -Force $OutDir | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win32 {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int cmd);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int w, int h, uint flags);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
}
"@
[Win32]::SetProcessDPIAware() | Out-Null

# Start from a clean state
Get-Process azahar -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
if (Test-Path $GameLog) { Remove-Item $GameLog -Force }
if ($DumpFrame -gt 0) {
    Set-Content -Path (Join-Path $AzaharData "sdmc\3ds\star3d\gxdump") -Value $DumpFrame -Encoding ascii
}

Write-Host "Launching $Rom for $Seconds s..."
$proc = Start-Process -FilePath $Azahar -ArgumentList "`"$Rom`"" -PassThru

# Wait for the main window
$deadline = (Get-Date).AddSeconds(20)
while ($proc.MainWindowHandle -eq 0 -and (Get-Date) -lt $deadline -and -not $proc.HasExited) {
    Start-Sleep -Milliseconds 250
    $proc.Refresh()
}

# Capture the emulator window (PrintWindow works even when it is covered)
function Save-Window($hwnd, $path) {
    $r = New-Object Win32+RECT
    [Win32]::GetWindowRect($hwnd, [ref]$r) | Out-Null
    $w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
    if ($w -le 0 -or $h -le 0) { return }
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    $ok = [Win32]::PrintWindow($hwnd, $hdc, 2)
    $g.ReleaseHdc($hdc)
    if (-not $ok) { $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size) }
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
}

Get-ChildItem $OutDir -Filter "shot_*.png" -ErrorAction SilentlyContinue | Remove-Item -Force
$elapsed = 0
while ($elapsed -lt $Seconds -and -not $proc.HasExited) {
    Start-Sleep -Seconds 1
    $elapsed++
    if ($ShotEvery -gt 0 -and ($elapsed % $ShotEvery) -eq 0 -and $elapsed -lt $Seconds) {
        $proc.Refresh()
        Save-Window $proc.MainWindowHandle (Join-Path $OutDir ("shot_{0:D3}.png" -f $elapsed))
    }
}

if ($proc.HasExited) {
    Write-Host "Azahar exited early (code $($proc.ExitCode))"
} else {
    # Screenshot of the emulator window
    $hwnd = $proc.MainWindowHandle
    [Win32]::ShowWindow($hwnd, 9) | Out-Null   # SW_RESTORE
    # Move fully on-screen and on top (SWP_NOSIZE) so nothing covers the capture
    [Win32]::SetWindowPos($hwnd, [IntPtr]::Zero, 40, 40, 0, 0, 0x0001) | Out-Null
    [Win32]::SetForegroundWindow($hwnd) | Out-Null
    Start-Sleep -Milliseconds 700
    Get-ChildItem $OutDir -Filter "burst_*.png" -ErrorAction SilentlyContinue | Remove-Item -Force
    for ($i = 0; $i -lt $Burst; $i++) {
        Save-Window $hwnd (Join-Path $OutDir ("burst_{0:D2}.png" -f $i))
        Start-Sleep -Milliseconds 50
    }
    $shot = Join-Path $OutDir "screenshot.png"
    Save-Window $hwnd $shot
    Write-Host "Screenshot: $shot"

    if ($Interactive) {
        Write-Host "Leaving Azahar running (-Interactive)"
    } else {
        # Close normally first so logs are flushed, then force
        $proc.CloseMainWindow() | Out-Null
        if (-not $proc.WaitForExit(5000)) { $proc | Stop-Process -Force }
    }
}

# Collect logs
if (Test-Path $GameLog) {
    Copy-Item $GameLog (Join-Path $OutDir "game_log.txt") -Force
    Write-Host "`n===== game log ====="
    Get-Content $GameLog
} else {
    Write-Host "`n(no game log at $GameLog)"
}

if (Test-Path $EmuLog) {
    $emuCopy = Join-Path $OutDir "azahar_log.txt"
    Copy-Item $EmuLog $emuCopy -Force
    Write-Host "`n===== emulator errors / crashes ====="
    $interesting = Select-String -Path $emuCopy -Pattern "<Critical>|<Error>|Unmapped|abort|exception|crash|svcBreak|Break" |
        Where-Object { $_.Line -notmatch "news.db|ClCertA|title id for ROM|Delay generator|ConfigureNew3DSCPU" }
    $interesting | Select-Object -First 40 | ForEach-Object { $_.Line }

    # Resolve code addresses (0x001xxxxx is the 3dsx .text range) to source lines
    if ((Test-Path $Elf) -and (Test-Path $Addr2Line)) {
        $addrs = $interesting | ForEach-Object { [regex]::Matches($_.Line, "0x0*(1[0-9a-fA-F]{5})\b") } |
            ForEach-Object { "0x" + $_.Groups[1].Value } | Select-Object -Unique -First 10
        if ($addrs) {
            Write-Host "`n===== addr2line ====="
            & $Addr2Line -f -C -p -e $Elf @addrs
        }
    }
}
