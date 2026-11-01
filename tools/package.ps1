# Packages a built WinBackRecompiled.exe into a release zip:
#   dist/WinBackRecompiled-v<version>-Windows-x64.zip
# Launchers (RecompLauncher, Quiver Launcher) pick the release file by the
# system and processor in its name, and expect the exe at the top of the zip
# next to recomplauncher.json.
#
#   powershell -ExecutionPolicy Bypass -File tools\package.ps1 [-BuildDir build]
param(
    [string]$BuildDir = "build"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$exe = Join-Path $BuildDir "WinBackRecompiled.exe"
if (-not (Test-Path $exe)) { throw "$exe not found; build first." }
$version = (Get-Item $exe).VersionInfo.ProductVersion
if (-not $version) { throw "$exe has no version resource." }

$name = "WinBackRecompiled-v$version-Windows-x64"
$stage = Join-Path "dist" $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null

foreach ($f in "WinBackRecompiled.exe", "SDL2.dll", "dxil.dll", "dxcompiler.dll") {
    Copy-Item (Join-Path $BuildDir $f) $stage
}
Copy-Item README.md, LICENSE, recomplauncher.json $stage
# The menu font is compiled into the exe; its license travels with it.
Copy-Item assets\fonts\OFL.txt (Join-Path $stage "Lato-OFL.txt")

$zip = Join-Path "dist" "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
Write-Host "wrote $zip"
