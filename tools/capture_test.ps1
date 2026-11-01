# Runs the game for a while and saves screenshots of its window.
# Usage: powershell -ExecutionPolicy Bypass -File tools\capture_test.ps1 [-Seconds 30] [-Shots 3] [-Out captures]
# Set WINBACK_AUTOPRESS (see src/main/input.cpp) for scripted input.
param(
    [int]$Seconds = 30,
    [int]$Shots = 3,
    [string]$Out = "captures",
    [string]$Exe = "build\WinBackRecompiled.exe"
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[Win]::SetProcessDPIAware() | Out-Null

New-Item -ItemType Directory -Force $Out | Out-Null
$proc = Start-Process -FilePath $Exe -ArgumentList "--rom", "baserom.us.z64" -PassThru `
    -RedirectStandardOutput "$Out\stdout.log" -RedirectStandardError "$Out\stderr.log"

$interval = [math]::Max(1, [int]($Seconds / $Shots))
for ($i = 1; $i -le $Shots; $i++) {
    Start-Sleep -Seconds $interval
    if ($proc.HasExited) { Write-Output "process exited with code $($proc.ExitCode)"; break }
    $proc.Refresh()
    $h = $proc.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { Write-Output "no window yet"; continue }
    [Win]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 300
    $r = New-Object Win+RECT
    [Win]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.Right - $r.Left; $hh = $r.Bottom - $r.Top
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
    $path = Join-Path $Out ("shot{0}.png" -f $i)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Output "saved $path"
}
if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
