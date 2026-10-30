# Mini USB Bridge - CI build script (Windows)
#
# One script, one report. Runs every stage of the tool chain:
#   checkout -> CMake configure (MSVC) -> build -> unit tests -> dist\app
#            -> Inno Setup (ISCC) -> setup.exe -> SHA-256 inventory
#
# Each stage is timed and its output is written to dist\ci\logs\<nn>-<stage>.log.
# At the end it writes dist\ci\toolchain-report.md, which records what ran, in
# which order, with which tool versions, what each stage exited with, the tail of
# any failing log, and the hash of every artifact. The report is written even when
# a stage fails, which is the point: CI should explain itself.
#
# Local use:   pwsh -File ci\windows-build.ps1
# SPDX-License-Identifier: MIT
param(
    [string]$Configuration = "Release",
    [switch]$SkipInstaller
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$buildDir = Join-Path $root "build"
$distDir = Join-Path $root "dist"
$appDir = Join-Path $distDir "app"
$ciDir = Join-Path $distDir "ci"
$logDir = Join-Path $ciDir "logs"
$reportPath = Join-Path $ciDir "toolchain-report.md"
$sumsPath = Join-Path $ciDir "SHA256SUMS.txt"

foreach ($d in @($buildDir, $distDir, $appDir, $ciDir, $logDir)) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}
$script:stages = New-Object System.Collections.ArrayList
$script:pipelineOk = $true
$script:toolVersions = New-Object System.Collections.ArrayList
$script:artifacts = New-Object System.Collections.ArrayList

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------
function Write-Section([string]$Text) {
    Write-Host ""
    Write-Host ("=" * 78) -ForegroundColor DarkGray
    Write-Host " $Text" -ForegroundColor Cyan
    Write-Host ("=" * 78) -ForegroundColor DarkGray
}

# Records a tool version for the report.
function Add-Tool([string]$Name, [string]$Version) {
    $v = ($Version -replace "\r?\n", " ").Trim()
    if ($v.Length -gt 120) { $v = $v.Substring(0, 120) + "..." }
    [void]$script:toolVersions.Add([pscustomobject]@{ Name = $Name; Version = $v })
    Write-Host ("  {0,-22} {1}" -f $Name, $v)
}

# Runs a native executable, tees its output into a per-stage log file.
# Returns the process exit code.
function Run-Native {
    param(
        [Parameter(Mandatory)][string]$Exe,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory)][string]$LogPath
    )
    Write-Host ("    {0} {1}" -f $Exe, ($Arguments -join " ")) -ForegroundColor DarkGray
    & $Exe @Arguments 2>&1 | Tee-Object -FilePath $LogPath -Append | Out-Host
    if ($null -eq $LASTEXITCODE) { return 0 }
    return [int]$LASTEXITCODE
}

# Wraps one pipeline stage: times it, keeps the exit code, stores the row.
function Invoke-Stage {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$Command,
        [Parameter(Mandatory)][scriptblock]$Action,
        [Parameter(Mandatory)][string]$LogPath
    )
    Write-Section "$Name"
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $code = 0
    $note = ""
    $global:LASTEXITCODE = 0
    try {
        $result = & $Action
        if ($null -ne $result -and $result -is [int]) { $code = $result }
        elseif ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) { $code = [int]$LASTEXITCODE }
    } catch {
        $code = 1
        $note = $_.Exception.Message
        Write-Host "  FAILED: $note" -ForegroundColor Red
        Add-Content -Path $LogPath -Value "EXCEPTION: $note"
    }
    $sw.Stop()
    $seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1)
    [void]$script:stages.Add([pscustomobject]@{
            Name    = $Name
            Command = $Command
            Code    = $code
            Seconds = $seconds
            Note    = $note
            Log     = (Split-Path -Leaf $LogPath)
        })
    if ($code -ne 0) {
        Write-Host ("  -> FAILED (exit {0}) after {1}s" -f $code, $seconds) -ForegroundColor Red
        return $false
    }
    Write-Host ("  -> ok in {0}s" -f $seconds) -ForegroundColor Green
    return $true
}

function Find-Iscc {
    $candidates = @()
    foreach ($base in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:LOCALAPPDATA)) {
        if ($base) {
            $candidates += (Join-Path $base "Inno Setup 6\ISCC.exe")
            $candidates += (Join-Path $base "Programs\Inno Setup 6\ISCC.exe")
        }
    }
    foreach ($c in $candidates) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    $onPath = Get-Command iscc.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    return $null
}

function Get-PeMachine([string]$Path) {
    # 0x8664 = x64, 0x14c = x86, 0xaa64 = ARM64. Reads the COFF header directly so
    # the report can prove the artifact really is a 64-bit PE.
    try {
        $fs = [System.IO.File]::OpenRead($Path)
        try {
            $br = New-Object System.IO.BinaryReader($fs)
            $fs.Position = 0x3C
            $peOffset = $br.ReadInt32()
            $fs.Position = $peOffset
            if ($br.ReadUInt32() -ne 0x00004550) { return "not a PE file" }
            $machine = $br.ReadUInt16()
            switch ($machine) {
                0x8664 { return "x64 (0x8664)" }
                0x014c { return "x86 (0x014c)" }
                0xAA64 { return "ARM64 (0xaa64)" }
                default { return ("0x{0:x4}" -f $machine) }
            }
        } finally { $fs.Dispose() }
    } catch { return "unreadable" }
}

# ---------------------------------------------------------------------------
# 0. tool versions
# ---------------------------------------------------------------------------
Write-Section "Tool chain versions"

$os = [System.Environment]::OSVersion.VersionString
if ($env:ImageOS) { $os = "$os  (GitHub runner image: $env:ImageOS $env:ImageVersion)" }
Add-Tool "Operating system" $os
Add-Tool "PowerShell" $PSVersionTable.PSVersion.ToString()

$gitVersion = (& git --version 2>&1 | Select-Object -First 1)
Add-Tool "git" $gitVersion

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue)
if ($cmake) {
    $cmakeVersion = (& cmake --version 2>&1 | Select-Object -First 1)
    Add-Tool "CMake" $cmakeVersion
    $cmakeExe = $cmake.Source
} else {
    Add-Tool "CMake" "NOT FOUND"
    $cmakeExe = $null
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vsVersion = (& $vswhere -latest -products * -property installationVersion 2>&1 | Select-Object -First 1)
    $vsPath = (& $vswhere -latest -products * -property installationPath 2>&1 | Select-Object -First 1)
    $vsName = (& $vswhere -latest -products * -property displayName 2>&1 | Select-Object -First 1)
    Add-Tool "Visual Studio" "$vsName $vsVersion"
    if ($vsPath -and (Test-Path (Join-Path $vsPath "VC\Tools\MSVC"))) {
        $toolset = (Get-ChildItem (Join-Path $vsPath "VC\Tools\MSVC") -Directory |
                    Sort-Object Name -Descending | Select-Object -First 1)
        if ($toolset) { Add-Tool "MSVC toolset" $toolset.Name }
        $hostCl = Get-ChildItem (Join-Path $vsPath "VC\Tools\MSVC") -Recurse -Filter cl.exe -ErrorAction SilentlyContinue |
                  Where-Object { $_.FullName -match 'Hostx64\\x64' } | Select-Object -First 1
        if ($hostCl) {
            $clVersion = (Get-Item $hostCl.FullName).VersionInfo.FileVersion
            Add-Tool "cl.exe" "$clVersion ($($hostCl.FullName))"
        }
    }
} else {
    Add-Tool "Visual Studio" "vswhere not found"
}

$sdkRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\Include"
if (Test-Path $sdkRoot) {
    $sdks = (Get-ChildItem $sdkRoot -Directory | Sort-Object Name -Descending | Select-Object -First 3 | ForEach-Object Name) -join ", "
    Add-Tool "Windows SDK" $sdks
}

$isccEarly = Find-Iscc
if ($isccEarly) {
    Add-Tool "Inno Setup (ISCC)" "$((Get-Item $isccEarly).VersionInfo.FileVersion) - $isccEarly"
} else {
    Add-Tool "Inno Setup (ISCC)" "not installed yet (the installer stage installs it)"
}

$pyVer = (& python --version 2>&1 | Select-Object -First 1)
Add-Tool "python" $pyVer

$commit = (& git rev-parse --short HEAD 2>&1)
$commitSubject = (& git log -1 --pretty=%s 2>&1)
Add-Tool "Commit" "$commit $commitSubject"

# ---------------------------------------------------------------------------
# 1. configure
# ---------------------------------------------------------------------------
$configureLog = Join-Path $logDir "10-configure.log"
$ok = Invoke-Stage -Name "CMake configure" -Command "cmake -S . -B build -A x64 -DMUB_BUILD_TESTS=ON" -LogPath $configureLog -Action {
    if (-not $cmakeExe) { throw "cmake is not installed on this runner" }
    Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
    $code = Run-Native $cmakeExe @("-S", $root, "-B", $buildDir, "-A", "x64", "-DMUB_BUILD_TESTS=ON") $configureLog
    if ($code -ne 0) {
        # Some images default to Ninja, where -A is invalid; retry with the plain default generator.
        Add-Content -Path $configureLog -Value "--- retrying without -A x64 (generator did not accept an architecture) ---"
        Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
        $code = Run-Native $cmakeExe @("-S", $root, "-B", $buildDir, "-DMUB_BUILD_TESTS=ON") $configureLog
    }
    return $code
}
$script:pipelineOk = $ok

# ---------------------------------------------------------------------------
# 2. build
# ---------------------------------------------------------------------------
if ($script:pipelineOk) {
    $buildLog = Join-Path $logDir "20-build.log"
    $script:pipelineOk = Invoke-Stage -Name "Build (MSVC, $Configuration)" `
        -Command "cmake --build build --config $Configuration --parallel" -LogPath $buildLog -Action {
        return (Run-Native $cmakeExe @("--build", $buildDir, "--config", $Configuration, "--parallel") $buildLog)
    }
}

# ---------------------------------------------------------------------------
# 3. unit tests
# ---------------------------------------------------------------------------
if ($script:pipelineOk) {
    $testLog = Join-Path $logDir "30-tests.log"
    $script:pipelineOk = Invoke-Stage -Name "Unit tests (portable core)" -Command "test_core.exe" -LogPath $testLog -Action {
        $testExe = Get-ChildItem $buildDir -Recurse -Filter test_core.exe -ErrorAction SilentlyContinue |
                   Where-Object { $_.FullName -notmatch 'CMakeFiles' } | Select-Object -First 1
        if (-not $testExe) { throw "test_core.exe was not produced by the build" }
        $code = Run-Native $testExe.FullName @() $testLog
        $summary = Select-String -Path $testLog -Pattern '^\d+ checks, \d+ failure' | Select-Object -Last 1
        if ($summary) { Add-Content -Path $testLog -Value "RESULT LINE: $($summary.Line)" }
        return $code
    }
}

# ---------------------------------------------------------------------------
# 4. stage the app directory the installer expects
# ---------------------------------------------------------------------------
if ($script:pipelineOk) {
    $stageLog = Join-Path $logDir "40-stage.log"
    $script:pipelineOk = Invoke-Stage -Name "Stage dist\app" -Command "copy MiniUsbBridge.exe + mubb-relay.exe" -LogPath $stageLog -Action {
        $exe = Get-ChildItem $buildDir -Recurse -Filter MiniUsbBridge.exe -ErrorAction SilentlyContinue |
               Where-Object { $_.FullName -notmatch 'CMakeFiles' } | Select-Object -First 1
        $relay = Get-ChildItem $buildDir -Recurse -Filter mubb-relay.exe -ErrorAction SilentlyContinue |
                 Where-Object { $_.FullName -notmatch 'CMakeFiles' } | Select-Object -First 1
        if (-not $exe) { throw "MiniUsbBridge.exe was not produced by the build" }
        Copy-Item $exe.FullName (Join-Path $appDir "MiniUsbBridge.exe") -Force
        Add-Content -Path $stageLog -Value "MiniUsbBridge.exe from $($exe.FullName)"
        if ($relay) {
            Copy-Item $relay.FullName (Join-Path $appDir "mubb-relay.exe") -Force
            Add-Content -Path $stageLog -Value "mubb-relay.exe from $($relay.FullName)"
        } else {
            Add-Content -Path $stageLog -Value "WARNING: mubb-relay.exe was not produced"
        }
        $mb = [math]::Round((Get-Item (Join-Path $appDir "MiniUsbBridge.exe")).Length / 1MB, 2)
        Add-Content -Path $stageLog -Value "MiniUsbBridge.exe = $mb MB, machine = $(Get-PeMachine (Join-Path $appDir 'MiniUsbBridge.exe'))"
        return 0
    }
}

# ---------------------------------------------------------------------------
# 5. installer (Inno Setup)
# ---------------------------------------------------------------------------
if ($script:pipelineOk -and -not $SkipInstaller) {
    $isccLog = Join-Path $logDir "50-installer.log"
    $script:pipelineOk = Invoke-Stage -Name "Inno Setup installer" -Command "ISCC.exe installer\MiniUsbBridge.iss" -LogPath $isccLog -Action {
        $iscc = Find-Iscc
        if (-not $iscc) {
            Add-Content -Path $isccLog -Value "ISCC.exe not found - installing Inno Setup with Chocolatey"
            $choco = Get-Command choco.exe -ErrorAction SilentlyContinue
            if (-not $choco) { throw "Neither Inno Setup nor Chocolatey is available on this runner" }
            $code = Run-Native $choco.Source @("install", "innosetup", "-y", "--no-progress", "--limit-output") $isccLog
            if ($code -ne 0) { throw "Chocolatey could not install Inno Setup (exit $code)" }
            $iscc = Find-Iscc
            if (-not $iscc) { throw "Inno Setup installed but ISCC.exe was not found" }
        }
        Add-Content -Path $isccLog -Value "ISCC: $iscc ($((Get-Item $iscc).VersionInfo.FileVersion))"
        # Verify every [Files] source exists before asking the compiler.
        $lintLog = Join-Path $logDir "45-iss-sources.log"
        $lintCode = Run-Native "python" @((Join-Path $root "ci\check_iss_sources.py"), (Join-Path $root "installer\MiniUsbBridge.iss")) $lintLog
        if ($lintCode -ne 0) { throw "installer\MiniUsbBridge.iss references files that do not exist (see 45-iss-sources.log)" }
        return (Run-Native $iscc @((Join-Path $root "installer\MiniUsbBridge.iss")) $isccLog)
    }
}

# ---------------------------------------------------------------------------
# 6. verify artifacts + inventory
# ---------------------------------------------------------------------------
$verifyLog = Join-Path $logDir "60-verify.log"
$verifyOk = Invoke-Stage -Name "Verify artifacts" -Command "hash + PE + version checks" -LogPath $verifyLog -Action {
    $appExe = Join-Path $appDir "MiniUsbBridge.exe"
    if (-not (Test-Path $appExe)) { throw "MiniUsbBridge.exe is missing from dist\app" }
    $setups = @(Get-ChildItem $distDir -Filter "MiniUsbBridge-*-setup.exe" -ErrorAction SilentlyContinue)
    if (-not $SkipInstaller -and $setups.Count -eq 0) { throw "no setup.exe was produced in dist\" }

    $files = @()
    $files += Get-Item $appExe
    $relayExe = Join-Path $appDir "mubb-relay.exe"
    if (Test-Path $relayExe) { $files += Get-Item $relayExe }
    foreach ($s in $setups) { $files += $s }

    $sumLines = @()
    foreach ($f in $files) {
        $hash = (Get-FileHash -Algorithm SHA256 -Path $f.FullName).Hash
        $machine = if ($f.Extension -eq ".exe") { Get-PeMachine $f.FullName } else { "-" }
        $version = ""
        try { $version = (Get-Item $f.FullName).VersionInfo.FileVersion } catch { }
        $sig = "n/a"
        if ($f.Extension -eq ".exe") {
            try {
                $authSig = Get-AuthenticodeSignature -FilePath $f.FullName
                $sig = $authSig.Status.ToString()
            } catch { $sig = "unknown" }
        }
        [void]$script:artifacts.Add([pscustomobject]@{
                Name    = $f.Name
                MB      = [math]::Round($f.Length / 1MB, 2)
                Bytes   = $f.Length
                Sha256  = $hash
                Machine = $machine
                Version = $version
                Signed  = $sig
            })
        $sumLines += "$hash  $($f.Name)"
        Add-Content -Path $verifyLog -Value ("{0}  {1,10} bytes  {2}  {3}  {4}" -f $hash, $f.Length, $machine, $version, $sig)
    }
    Set-Content -Path $sumsPath -Value ($sumLines -join "`n")
    Add-Content -Path $verifyLog -Value "SHA256SUMS written to $sumsPath"

    if (-not $SkipInstaller) {
        $setup = $setups[0]
        if ($setup.Length -lt 500000) { throw "setup.exe is suspiciously small ($($setup.Length) bytes)" }
        Add-Content -Path $verifyLog -Value "setup.exe size ok: $([math]::Round($setup.Length / 1MB, 2)) MB"
    }
    return 0
}
$script:pipelineOk = $script:pipelineOk -and $verifyOk

# ---------------------------------------------------------------------------
# 7. the report
# ---------------------------------------------------------------------------
function Write-Report {
    $lines = New-Object System.Collections.ArrayList

    $trigger = if ($env:GITHUB_EVENT_NAME) { $env:GITHUB_EVENT_NAME } else { "local" }
    $resultText = if ($script:pipelineOk) { "**PASS** - app + tests + installer built" } else { "**FAIL**" }
    $failedStage = ($script:stages | Where-Object { $_.Code -ne 0 } | Select-Object -First 1)

    [void]$lines.Add("# Mini USB Bridge - tool chain report")
    [void]$lines.Add("")
    [void]$lines.Add("| | |")
    [void]$lines.Add("|---|---|")
    [void]$lines.Add("| Result | $resultText |")
    [void]$lines.Add("| Trigger | ``$trigger`` |")
    [void]$lines.Add("| Ref | ``$($env:GITHUB_REF)`` |")
    [void]$lines.Add("| Commit | ``$($env:GITHUB_SHA)`` |")
    [void]$lines.Add("| Run | $($env:GITHUB_SERVER_URL)/$($env:GITHUB_REPOSITORY)/actions/runs/$($env:GITHUB_RUN_ID) (attempt $($env:GITHUB_RUN_ATTEMPT)) |")
    [void]$lines.Add("| Runner | $os |")
    [void]$lines.Add("| Generated | $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') UTC+00 |")
    [void]$lines.Add("")

    [void]$lines.Add("## Tool chain")
    [void]$lines.Add("")
    [void]$lines.Add("```")
    [void]$lines.Add("git checkout")
    [void]$lines.Add("  -> CMake configure   (generator: default Visual Studio, x64)")
    [void]$lines.Add("  -> MSVC build        (Release, /W4, static core lib)")
    [void]$lines.Add("  -> test_core.exe     (crypto, pairing, protocol, USB/IP wire)")
    [void]$lines.Add("  -> dist\app          (MiniUsbBridge.exe + mubb-relay.exe)")
    [void]$lines.Add("  -> check_iss_sources (every [Files] source must exist)")
    [void]$lines.Add("  -> ISCC.exe          (Inno Setup 6 -> setup.exe)")
    [void]$lines.Add("  -> verify + SHA-256  (PE machine, version, signature status)")
    [void]$lines.Add("```")
    [void]$lines.Add("")

    [void]$lines.Add("## Stages")
    [void]$lines.Add("")
    [void]$lines.Add("| # | Stage | Command | Result | Time | Log |")
    [void]$lines.Add("|---|---|---|---|---|---|")
    $i = 0
    foreach ($s in $script:stages) {
        $i++
        $status = if ($s.Code -eq 0) { "ok" } else { "**FAILED (exit $($s.Code))**" }
        [void]$lines.Add("| $i | $($s.Name) | ``$($s.Command)`` | $status | $($s.Seconds)s | ``$($s.Log)`` |")
    }
    $skipped = 7 - $script:stages.Count
    if ($skipped -gt 0) {
        [void]$lines.Add("")
        [void]$lines.Add("_$skipped stage(s) never ran: the pipeline stops at the first failure, by design._")
    }
    [void]$lines.Add("")

    if ($failedStage) {
        [void]$lines.Add("## Why it failed")
        [void]$lines.Add("")
        [void]$lines.Add("Stage ``$($failedStage.Name)`` exited with code $($failedStage.Code).")
        if ($failedStage.Note) { [void]$lines.Add(""); [void]$lines.Add("``````"); [void]$lines.Add($failedStage.Note); [void]$lines.Add("``````") }
        $tailLog = Join-Path $logDir $failedStage.Log
        if (Test-Path $tailLog) {
            [void]$lines.Add("")
            [void]$lines.Add("Last 40 lines of ``$($failedStage.Log)``:")
            [void]$lines.Add("")
            [void]$lines.Add("``````")
            foreach ($l in (Get-Content $tailLog -Tail 40)) { [void]$lines.Add($l) }
            [void]$lines.Add("``````")
        }
        [void]$lines.Add("")
    }

    [void]$lines.Add("## Tool versions")
    [void]$lines.Add("")
    [void]$lines.Add("| Tool | Version |")
    [void]$lines.Add("|---|---|")
    foreach ($t in $script:toolVersions) { [void]$lines.Add("| $($t.Name) | $($t.Version) |") }
    [void]$lines.Add("")

    [void]$lines.Add("## Artifacts")
    [void]$lines.Add("")
    if ($script:artifacts.Count -eq 0) {
        [void]$lines.Add("_No artifacts were produced._")
    } else {
        [void]$lines.Add("| File | Size | PE | Version | Signature | SHA-256 |")
        [void]$lines.Add("|---|---|---|---|---|---|")
        foreach ($a in $script:artifacts) {
            [void]$lines.Add("| ``$($a.Name)`` | $($a.MB) MB | $($a.Machine) | $($a.Version) | $($a.Signed) | ``$($a.Sha256)`` |")
        }
    }
    [void]$lines.Add("")

    [void]$lines.Add("## What this run does not prove")
    [void]$lines.Add("")
    [void]$lines.Add("* The GUI was **not** launched: the runner has no interactive desktop, so this only proves it compiles, links and packages.")
    [void]$lines.Add("* No USB hardware, no usbipd-win and no usbip-win2 driver on the runner, so device enumeration and attachment are still **unverified** - see ``docs/TESTING.md``.")
    [void]$lines.Add("* The installer is compiled but not executed; ``Invoke-Verify`` on a clean VM is still required.")
    $sigNote = ($script:artifacts | Where-Object { $_.Signed -ne "Valid" -and $_.Signed -ne "n/a" } | Select-Object -First 1)
    if ($sigNote) {
        [void]$lines.Add("* Binaries are **unsigned** (``$($sigNote.Signed)``): SmartScreen will warn on first run until you add a code-signing certificate.")
    }
    [void]$lines.Add("")

    $text = ($lines -join "`n")
    Set-Content -Path $reportPath -Value $text -Encoding utf8

    if ($env:GITHUB_STEP_SUMMARY) {
        Add-Content -Path $env:GITHUB_STEP_SUMMARY -Value $text
    }
    Write-Host ""
    Write-Host "Report: $reportPath" -ForegroundColor Cyan
}

Write-Report

if (-not $script:pipelineOk) {
    Write-Host ""
    Write-Host "Pipeline FAILED - see $reportPath" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Pipeline PASSED - artifacts in $distDir, report in $reportPath" -ForegroundColor Green
exit 0
