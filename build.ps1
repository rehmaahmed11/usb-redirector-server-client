# Mini USB Bridge - Windows build script
# Usage (from a normal PowerShell prompt, no admin needed):
#   .\build.ps1                 # build Release, run tests, collect dist\app
#   .\build.ps1 -Installer      # additionally build the Inno Setup installer
#   .\build.ps1 -Clean
# SPDX-License-Identifier: MIT
param(
    [switch]$Clean,
    [switch]$Installer,
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Definition
Set-Location $root

function Find-CMake {
    $candidates = @(
        "cmake",
        "$env:ProgramFiles\CMake\bin\cmake.exe",
        "${env:ProgramFiles(x86)}\CMake\bin\cmake.exe"
    )
    foreach ($c in $candidates) {
        $cmd = Get-Command $c -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
        if (Test-Path $c) { return $c }
    }
    throw "CMake was not found. Install CMake 3.20+ or run this script from a Visual Studio Developer PowerShell."
}

$cmake = Find-CMake
Write-Host "CMake: $cmake" -ForegroundColor Cyan

if ($Clean) {
    Remove-Item -Recurse -Force "$root\build", "$root\dist" -ErrorAction SilentlyContinue
    Write-Host "Cleaned build and dist." -ForegroundColor Yellow
    return
}

# Configure (Visual Studio generator picks up the installed MSVC toolset).
if (-not (Test-Path "$root\build\CMakeCache.txt")) {
    & $cmake -S $root -B "$root\build" -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Visual Studio generator unavailable, falling back to the default generator..." -ForegroundColor Yellow
        & $cmake -S $root -B "$root\build" -A x64
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
    }
}

& $cmake --build "$root\build" --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

# Unit tests (portable core: crypto, pairing, protocol, USB/IP header checks)
$testExe = Get-ChildItem "$root\build" -Recurse -Filter test_core.exe -ErrorAction SilentlyContinue | Select-Object -First 1
if ($testExe) {
    Write-Host "Running core tests..." -ForegroundColor Cyan
    & $testExe.FullName
    if ($LASTEXITCODE -ne 0) { throw "Core tests failed" }
}

# Collect artifacts in the layout the Inno Setup script expects.
$appDir = "$root\dist\app"
New-Item -ItemType Directory -Force -Path $appDir | Out-Null
$exe = Get-ChildItem "$root\build" -Recurse -Filter MiniUsbBridge.exe -ErrorAction SilentlyContinue |
       Where-Object { $_.FullName -notmatch 'CMakeFiles' } | Select-Object -First 1
$relay = Get-ChildItem "$root\build" -Recurse -Filter mubb-relay.exe -ErrorAction SilentlyContinue |
         Where-Object { $_.FullName -notmatch 'CMakeFiles' } | Select-Object -First 1
if (-not $exe) { throw "MiniUsbBridge.exe was not produced" }
Copy-Item $exe.FullName "$appDir\MiniUsbBridge.exe" -Force
if ($relay) { Copy-Item $relay.FullName "$appDir\mubb-relay.exe" -Force }

$size = [math]::Round((Get-Item "$appDir\MiniUsbBridge.exe").Length / 1MB, 2)
Write-Host "Built: $appDir\MiniUsbBridge.exe ($size MB)" -ForegroundColor Green

if ($Installer) {
    $iscc = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $iscc) {
        throw "Inno Setup 6 was not found. Install it from https://jrsoftware.org/isdl.php (or: winget install JRSoftware.InnoSetup)"
    }
    & $iscc "$root\installer\MiniUsbBridge.iss"
    if ($LASTEXITCODE -ne 0) { throw "Installer build failed" }
    Get-ChildItem "$root\dist\MiniUsbBridge-*-setup.exe" | ForEach-Object {
        $mb = [math]::Round($_.Length / 1MB, 2)
        Write-Host "Installer: $($_.FullName) ($mb MB)" -ForegroundColor Green
    }
}

Write-Host ""
Write-Host "Next steps:" -ForegroundColor Cyan
Write-Host "  1. Install the server backend on the PC with the USB device : winget install usbipd"
Write-Host "  2. Install the client backend on the PC that needs it       : https://github.com/vadimgrn/usbip-win2/releases"
Write-Host "  3. Run dist\app\MiniUsbBridge.exe  (or the setup.exe with -Installer)"
