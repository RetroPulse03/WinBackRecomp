# Packages a built WinBackRecompiled.exe into a release zip:
#   dist/WinBackRecompiled-v<version>-Windows.zip
# N64RecompLauncher picks the asset whose name contains "Windows" and expects
# the exe at the top level of the archive.
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

$name = "WinBackRecompiled-v$version-Windows"
$stage = Join-Path "dist" $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null

foreach ($f in "WinBackRecompiled.exe", "SDL2.dll", "dxil.dll", "dxcompiler.dll") {
    Copy-Item (Join-Path $BuildDir $f) $stage
}
Copy-Item README.md, LICENSE $stage

$zip = Join-Path "dist" "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
Write-Host "wrote $zip"
