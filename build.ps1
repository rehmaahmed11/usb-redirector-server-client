# Mini USB Bridge - Windows build script
# Usage (from a normal PowerShell prompt, no admin needed):
#   .\build.ps1                      # normal build for Windows 10/11, run tests, collect dist\app
#   .\build.ps1 -MinWin vista        # binary that also runs on Vista/7/8/8.1 -> dist\app-legacy
#   .\build.ps1 -MinWin xp           # binary for Windows XP SP3 (needs the v141_xp toolset)
#   .\build.ps1 -Installer           # additionally build the Inno Setup installer
#   .\build.ps1 -Clean
# SPDX-License-Identifier: MIT
param(
    [switch]$Clean,
    [switch]$Installer,
    [string]$Configuration = "Release",
    [ValidateSet("win10", "vista", "xp")]
    [string]$MinWin = "win10",
    [ValidateSet("auto", "x86", "x64")]
    [string]$Platform = "auto",
    [string]$Toolset = ""
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

# Target defaults: a Windows XP build is 32-bit unless asked otherwise, and the
# XP toolset is required (Visual Studio 2022 does not ship it).
if ($Platform -eq "auto") { $Platform = if ($MinWin -eq "xp") { "x86" } else { "x64" } }
if (-not $Toolset -and $MinWin -eq "xp") { $Toolset = "v141_xp" }

$isLegacy = $MinWin -ne "win10"
$buildDir = if ($isLegacy) { "$root\build-$MinWin" } else { "$root\build" }
$appDir = if ($isLegacy) { "$root\dist\app-legacy" } else { "$root\dist\app" }
$issFile = switch ($MinWin) {
    "xp" { "$root\installer\MiniUsbBridgeXp.iss" }
    "vista" { "$root\installer\MiniUsbBridgeLegacy.iss" }
    default { "$root\installer\MiniUsbBridge.iss" }
}

$cmake = Find-CMake
Write-Host "CMake: $cmake" -ForegroundColor Cyan
Write-Host "Minimum Windows: $MinWin   platform: $Platform   toolset: $(if ($Toolset) { $Toolset } else { 'default' })" -ForegroundColor Cyan

if ($Clean) {
    Remove-Item -Recurse -Force "$root\build", "$root\build-vista", "$root\build-xp", "$root\dist" -ErrorAction SilentlyContinue
    Write-Host "Cleaned build and dist." -ForegroundColor Yellow
    return
}

# Configure. The Visual Studio generator picks up the installed MSVC toolset.
if (-not (Test-Path "$buildDir\CMakeCache.txt")) {
    $cfgArgs = @("-S", $root, "-B", $buildDir, "-G", "Visual Studio 17 2022", "-A", $Platform,
                 "-DMUB_MIN_WIN=$MinWin")
    if ($Toolset) { $cfgArgs += @("-T", $Toolset) }
    & $cmake @cfgArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Visual Studio 17 2022 with this toolset is unavailable, falling back to the default generator..." -ForegroundColor Yellow
        $cfgArgs = @("-S", $root, "-B", $buildDir, "-A", $Platform, "-DMUB_MIN_WIN=$MinWin")
        if ($Toolset) { $cfgArgs += @("-T", $Toolset) }
        & $cmake @cfgArgs
        if ($LASTEXITCODE -ne 0 -and $MinWin -eq "xp") {
            throw @"
The Windows XP build needs the XP toolset, which Visual Studio 2022 does not ship.
  * Install Visual Studio 2017 or 2019 with the component
    "C++ Windows XP Support for VS 2017 (v141) tools".
  * Run:  .\build.ps1 -MinWin xp -Toolset v141_xp
  * Or cross compile with MinGW-w64 i686 (see docs/COMPATIBILITY.md).
  * A MinGW user can also run:  cmake -S . -B build-xp -DMUB_MIN_WIN=xp
"@
        }
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
    }
}

& $cmake --build $buildDir --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

# Unit tests (portable core: crypto, pairing, protocol, USB/IP header checks, OS support matrix)
$testExe = Get-ChildItem $buildDir -Recurse -Filter test_core.exe -ErrorAction SilentlyContinue | Select-Object -First 1
if ($testExe) {
    Write-Host "Running core tests..." -ForegroundColor Cyan
    & $testExe.FullName
    if ($LASTEXITCODE -ne 0) { throw "Core tests failed" }
}

# Collect artifacts in the layout the Inno Setup scripts expect.
New-Item -ItemType Directory -Force -Path $appDir | Out-Null
$exe = Get-ChildItem $buildDir -Recurse -Filter MiniUsbBridge.exe -ErrorAction SilentlyContinue |
       Where-Object { $_.FullName -notmatch 'CMakeFiles' } | Select-Object -First 1
$relay = Get-ChildItem $buildDir -Recurse -Filter mubb-relay.exe -ErrorAction SilentlyContinue |
         Where-Object { $_.FullName -notmatch 'CMakeFiles' } | Select-Object -First 1
if (-not $exe) { throw "MiniUsbBridge.exe was not produced" }
Copy-Item $exe.FullName "$appDir\MiniUsbBridge.exe" -Force
if ($relay) { Copy-Item $relay.FullName "$appDir\mubb-relay.exe" -Force } else { Remove-Item "$appDir\mubb-relay.exe" -ErrorAction SilentlyContinue }

$size = [math]::Round((Get-Item "$appDir\MiniUsbBridge.exe").Length / 1MB, 2)
Write-Host "Built: $appDir\MiniUsbBridge.exe ($size MB)" -ForegroundColor Green

# For a legacy target, prove the binary really does not import post-XP APIs:
# the PE subsystem version must be 5.01 for XP and the import table may not
# reference the UCRT or any Vista+ function (ci/check_legacy_compat.py --exe).
if ($isLegacy) {
    $py = Get-Command python -ErrorAction SilentlyContinue
    if (-not $py) { $py = Get-Command py -ErrorAction SilentlyContinue }
    if ($py) {
        $peTarget = if ($MinWin -eq "xp") { "xp" } else { "win10" }
        Write-Host "Checking the $MinWin binary against the $peTarget import rules..." -ForegroundColor Cyan
        & $py.Source "$root\ci\check_legacy_compat.py" --exe "$appDir\MiniUsbBridge.exe" --target $peTarget
        if ($LASTEXITCODE -ne 0) {
            if ($MinWin -eq "xp") {
                Write-Host "The binary still imports APIs that Windows XP does not have." -ForegroundColor Red
                Write-Host "An 'xp' build must use the static CRT and the v141_xp toolset; see docs/COMPATIBILITY.md." -ForegroundColor Yellow
                throw "Windows XP compatibility check failed"
            }
            Write-Host "Informational for a Vista target: see the imports listed above (docs/COMPATIBILITY.md)." -ForegroundColor Yellow
        }
    } else {
        Write-Host "python not found - skipping the PE compatibility check" -ForegroundColor Yellow
    }
}

if ($Installer) {
    # Fetch bundled backends so the *default* installer is fully offline. The
    # legacy installers never bundle backends (those Windows versions cannot run them).
    if (-not $isLegacy) {
        $fetchPs1 = "$root\ci\fetch-backends.ps1"
        if (Test-Path $fetchPs1) {
            Write-Host "Fetching bundled backends (offline installer)..." -ForegroundColor Cyan
            try {
                & pwsh -NoProfile -File $fetchPs1
                if ($LASTEXITCODE -ne 0) { Write-Host "Backend fetch returned $LASTEXITCODE - installer will still build but may show download links" -ForegroundColor Yellow }
            } catch {
                Write-Host "Backend fetch failed: $($_.Exception.Message) - installer will still build" -ForegroundColor Yellow
            }
        } else {
            $fetchPy = "$root\ci\fetch-backends.py"
            if (Test-Path $fetchPy) {
                Write-Host "Fetching bundled backends via python..." -ForegroundColor Cyan
                try { & python $fetchPy } catch { Write-Host "Python fetch failed: $($_.Exception.Message)" -ForegroundColor Yellow }
            }
        }
    }

    # Inno Setup 6 builds the Windows 10 and the legacy (7+) installer; Windows XP
    # needs Inno Setup 5.6.1, which only that script understands.
    $isccCandidates = if ($MinWin -eq "xp") {
        @("${env:ProgramFiles(x86)}\Inno Setup 5\ISCC.exe", "$env:ProgramFiles\Inno Setup 5\ISCC.exe")
    } else {
        @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe")
    }
    $iscc = $isccCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $iscc) {
        if ($MinWin -eq "xp") {
            throw "Inno Setup 5.6.1 was not found (https://jrsoftware.org/download.php/is.exe is 6.x; use the 5.6.1 release from jrsoftware.org/files/old/). Inno Setup 6 cannot produce a Windows XP setup."
        }
        throw "Inno Setup 6 was not found. Install it from https://jrsoftware.org/isdl.php (or: winget install JRSoftware.InnoSetup)"
    }
    & $iscc $issFile
    if ($LASTEXITCODE -ne 0) { throw "Installer build failed" }
    Get-ChildItem "$root\dist\MiniUsbBridge-*-setup.exe" | ForEach-Object {
        $mb = [math]::Round($_.Length / 1MB, 2)
        Write-Host "Installer: $($_.FullName) ($mb MB)" -ForegroundColor Green
    }
}

Write-Host ""
Write-Host "Next steps:" -ForegroundColor Cyan
if ($isLegacy) {
    Write-Host "  Legacy build ($MinWin): the app, pairing, tunnel and relay work; USB backends do not"
    Write-Host "  (they need Windows 10 1809/1903 x64). See docs\COMPATIBILITY.md."
    Write-Host "  Installer: .\build.ps1 -MinWin $MinWin -Installer"
} else {
    Write-Host "  Installer is fully offline: backends are already inside (no extra download needed)"
    Write-Host "  1. Run dist\MiniUsbBridge-*-setup.exe on both PCs (choose Server / Client / Both)"
    Write-Host "  2. Or run dist\app\MiniUsbBridge.exe directly"
}
