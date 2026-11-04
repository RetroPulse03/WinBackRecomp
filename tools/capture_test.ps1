# Runs the game for a while and saves screenshots of its window.
# Usage: powershell -ExecutionPolicy Bypass -File tools\capture_test.ps1 [-Seconds 30] [-Shots 3] [-Out generated\captures]
# Set WINBACK_AUTOPRESS (see src/main/input.cpp) for scripted input.
param(
    [int]$Seconds = 30,
    [int]$Shots = 3,
    [string]$Out = "generated\captures",
    [string]$Exe = "build\WinBackRecompiled.exe"
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[Win]::SetProcessDPIAware() | Out-Null

New-Item -ItemType Directory -Force $Out | Out-Null
$proc = Start-Process -FilePath $Exe -ArgumentList "--rom", "romaserom.us.z64" -PassThru `
    -RedirectStandardOutput "$Out\stdout.log" -RedirectStandardError "$Out\stderr.log"

$interval = [math]::Max(1, [int]($Seconds / $Shots))
for ($i = 1; $i -le $Shots; $i++) {
    Start-Sleep -Seconds $interval
    if ($proc.HasExited) { Write-Output "process exited with code $($proc.ExitCode)"; break }
    $proc.Refresh()
    $h = $proc.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { Write-Output "no window yet"; continue }
    # A minimized window has no image. 4 is SW_SHOWNOACTIVATE, which leaves the focus alone.
    if ([Win]::IsIconic($h)) { [Win]::ShowWindow($h, 4) | Out-Null; Start-Sleep -Milliseconds 300 }
    # Ask the window for its own image, so other windows on screen can't end up in the shot.
    $r = New-Object Win+RECT
    [Win]::GetClientRect($h, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap ($r.Right), ($r.Bottom)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $g.GetHdc()
    [Win]::PrintWindow($h, $dc, 3) | Out-Null  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    $g.ReleaseHdc($dc)
    $path = Join-Path $Out ("shot{0}.png" -f $i)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Output "saved $path"
}
if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
