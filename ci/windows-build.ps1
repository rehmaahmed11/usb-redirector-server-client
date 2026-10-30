# Mini USB Bridge - CI build script (Windows)
#
# One script, one report. Runs every stage of the tool chain:
#   checkout -> CMake configure (MSVC) -> build -> unit tests -> dist\app
#            -> Inno Setup (ISCC) -> setup.exe -> SHA-256 inventory
#
# Each stage is timed and its output is written to dist\ci\logs\<nn>-<stage>.log.
# At the end it writes dist\ci\toolchain-report.md, which records what ran, in
# which order, with which tool versions, what each stage exited with, the tail of
# any failing log, and the hash of every artifact.
#
# The report is written from a `finally` block, so it exists even when a stage
# blows up or the script itself throws - a red build must explain itself.
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
$script:osText = "unknown"
$script:stageIndex = 0

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------
function Write-Section([string]$Text) {
    Write-Host ""
    Write-Host ("=" * 78) -ForegroundColor DarkGray
    Write-Host " $Text" -ForegroundColor Cyan
    Write-Host ("=" * 78) -ForegroundColor DarkGray
}

function Add-Tool([string]$Name, $Version) {
    $text = ""
    if ($null -ne $Version) { $text = [string]$Version }
    $text = ($text -replace "\r?\n", " ").Trim()
    if ($text.Length -gt 140) { $text = $text.Substring(0, 140) + "..." }
    if ($text.Length -eq 0) { $text = "(unknown)" }
    [void]$script:toolVersions.Add([pscustomobject]@{ Name = $Name; Version = $text })
    Write-Host ("  {0,-22} {1}" -f $Name, $text)
}

# Runs a native executable and tees its output into a per-stage log file.
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

# One pipeline stage: timed, its exit code recorded, its log named deterministically.
function Invoke-Stage {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$Command,
        [Parameter(Mandatory)][scriptblock]$Action,
        [Parameter(Mandatory)][string]$LogPath
    )
    $script:stageIndex++
    Write-Section ("{0}. {1}" -f $script:stageIndex, $Name)
    Add-Content -Path $LogPath -Value ("### stage: {0} | command: {1} | started: {2}" -f $Name, $Command, (Get-Date -Format o))
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $code = 0
    $note = ""
    $global:LASTEXITCODE = 0
    try {
        $result = & $Action
        if ($null -ne $result -and $result -is [int]) { $code = [int]$result }
        elseif ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) { $code = [int]$LASTEXITCODE }
    } catch {
        $code = 1
        $note = $_.Exception.Message
        Write-Host "  FAILED: $note" -ForegroundColor Red
        Add-Content -Path $LogPath -Value ("EXCEPTION: {0}" -f $note)
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
# tool versions - every probe is guarded, none of them may kill the pipeline
# ---------------------------------------------------------------------------
function Collect-ToolVersions {
    Write-Section "Tool chain versions"

    try {
        $script:osText = [System.Environment]::OSVersion.VersionString
        if ($env:ImageOS) { $script:osText = "$($script:osText)  (runner image: $env:ImageOS $env:ImageVersion)" }
    } catch { }
    Add-Tool "Operating system" $script:osText
    Add-Tool "PowerShell" $PSVersionTable.PSVersion

    try { Add-Tool "git" (& git --version 2>&1 | Select-Object -First 1) } catch { Add-Tool "git" "not available" }

    $script:cmakeExe = $null
    try {
        $cmake = Get-Command cmake -ErrorAction SilentlyContinue
        if ($cmake) {
            $script:cmakeExe = $cmake.Source
            Add-Tool "CMake" (& cmake --version 2>&1 | Select-Object -First 1)
        } else {
            Add-Tool "CMake" "NOT FOUND"
        }
    } catch { Add-Tool "CMake" "probe failed: $($_.Exception.Message)" }

    try {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path $vswhere) {
            $vsVersion = (& $vswhere -latest -products * -property installationVersion 2>&1 | Select-Object -First 1)
            $vsPath = (& $vswhere -latest -products * -property installationPath 2>&1 | Select-Object -First 1)
            Add-Tool "Visual Studio" "MSVC $vsVersion"
            if ($vsPath -and (Test-Path (Join-Path $vsPath "VC\Tools\MSVC"))) {
                $toolset = (Get-ChildItem (Join-Path $vsPath "VC\Tools\MSVC") -Directory |
                            Sort-Object Name -Descending | Select-Object -First 1)
                if ($toolset) { Add-Tool "MSVC toolset" $toolset.Name }
                $hostCl = Get-ChildItem (Join-Path $vsPath "VC\Tools\MSVC") -Recurse -Filter cl.exe -ErrorAction SilentlyContinue |
                          Where-Object { $_.FullName -match 'Hostx64\\x64' } | Select-Object -First 1
                if ($hostCl) { Add-Tool "cl.exe" (Get-Item $hostCl.FullName).VersionInfo.FileVersion }
            }
        } else {
            Add-Tool "Visual Studio" "vswhere not found"
        }
    } catch { Add-Tool "Visual Studio" "probe failed: $($_.Exception.Message)" }

    try {
        $sdkRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\Include"
        if (Test-Path $sdkRoot) {
            $sdks = ((Get-ChildItem $sdkRoot -Directory | Sort-Object Name -Descending |
                      Select-Object -First 3 | ForEach-Object { $_.Name }) -join ", ")
            Add-Tool "Windows SDK" $sdks
        }
    } catch { Add-Tool "Windows SDK" "probe failed: $($_.Exception.Message)" }

    try {
        $isccEarly = Find-Iscc
        if ($isccEarly) {
            $v = (Get-Item $isccEarly).VersionInfo
            $ver = $v.ProductVersion
            if (-not $ver) { $ver = $v.FileVersion }
            Add-Tool "Inno Setup (ISCC)" "$ver - $isccEarly"
        } else {
            Add-Tool "Inno Setup (ISCC)" "not installed yet (the installer stage installs it)"
        }
    } catch { Add-Tool "Inno Setup (ISCC)" "probe failed: $($_.Exception.Message)" }

    try { Add-Tool "python" (& python --version 2>&1 | Select-Object -First 1) } catch { Add-Tool "python" "not available" }
    try { Add-Tool "Commit" (& git log -1 --pretty="%h %s" 2>&1) } catch { }
}

# ---------------------------------------------------------------------------
# the pipeline
# ---------------------------------------------------------------------------
function Invoke-Pipeline {

    # 1. configure
    $configureLog = Join-Path $logDir "10-configure.log"
    $script:pipelineOk = Invoke-Stage -Name "CMake configure" `
        -Command "cmake -S . -B build -A x64 -DMUB_BUILD_TESTS=ON" -LogPath $configureLog -Action {
        if (-not $script:cmakeExe) { throw "cmake is not installed on this runner" }
        Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
        $code = Run-Native $script:cmakeExe @("-S", $root, "-B", $buildDir, "-A", "x64", "-DMUB_BUILD_TESTS=ON") $configureLog
        if ($code -ne 0) {
            # Some images default to Ninja, where -A is invalid; retry with the plain default generator.
            Add-Content -Path $configureLog -Value "--- retrying without -A x64 (the default generator did not accept an architecture) ---"
            Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
            $code = Run-Native $script:cmakeExe @("-S", $root, "-B", $buildDir, "-DMUB_BUILD_TESTS=ON") $configureLog
        }
        return $code
    }
    if (-not $script:pipelineOk) { return }

    # 2. build
    $buildLog = Join-Path $logDir "20-build.log"
    $script:pipelineOk = Invoke-Stage -Name "Build (MSVC, $Configuration)" `
        -Command "cmake --build build --config $Configuration --parallel" -LogPath $buildLog -Action {
        return (Run-Native $script:cmakeExe @("--build", $buildDir, "--config", $Configuration, "--parallel") $buildLog)
    }
    if (-not $script:pipelineOk) { return }

    # 3. unit tests
    $testLog = Join-Path $logDir "30-tests.log"
    $script:pipelineOk = Invoke-Stage -Name "Unit tests (portable core)" -Command "test_core.exe" -LogPath $testLog -Action {
        $testExe = Get-ChildItem $buildDir -Recurse -Filter test_core.exe -ErrorAction SilentlyContinue |
                   Where-Object { $_.FullName -notmatch 'CMakeFiles' } | Select-Object -First 1
        if (-not $testExe) { throw "test_core.exe was not produced by the build" }
        return (Run-Native $testExe.FullName @() $testLog)
    }
    if (-not $script:pipelineOk) { return }

    # 4. stage the directory the installer expects
    $stageLog = Join-Path $logDir "40-stage.log"
    $script:pipelineOk = Invoke-Stage -Name "Stage dist\app" -Command "copy MiniUsbBridge.exe + mubb-relay.exe" -LogPath $stageLog -Action {
        $exe = Get-ChildItem $buildDir -Recurse -Filter MiniUsbBridge.exe -ErrorAction SilentlyContinue |
               Where-Object { $_.FullName -notmatch 'CMakeFiles' } | Select-Object -First 1
        $relay = Get-ChildItem $buildDir -Recurse -Filter mubb-relay.exe -ErrorAction SilentlyContinue |
                 Where-Object { $_.FullName -notmatch 'CMakeFiles' } | Select-Object -First 1
        if (-not $exe) { throw "MiniUsbBridge.exe was not produced by the build" }
        Copy-Item $exe.FullName (Join-Path $appDir "MiniUsbBridge.exe") -Force
        Add-Content -Path $stageLog -Value "MiniUsbBridge.exe <- $($exe.FullName)"
        if ($relay) {
            Copy-Item $relay.FullName (Join-Path $appDir "mubb-relay.exe") -Force
            Add-Content -Path $stageLog -Value "mubb-relay.exe <- $($relay.FullName)"
        } else {
            Add-Content -Path $stageLog -Value "WARNING: mubb-relay.exe was not produced"
        }
        $sizeMb = [math]::Round((Get-Item (Join-Path $appDir "MiniUsbBridge.exe")).Length / 1MB, 2)
        Add-Content -Path $stageLog -Value "MiniUsbBridge.exe = $sizeMb MB, machine = $(Get-PeMachine (Join-Path $appDir 'MiniUsbBridge.exe'))"
        return 0
    }
    if (-not $script:pipelineOk) { return }

    # 4b. fetch bundled backends (makes installer fully offline)
    $fetchLog = Join-Path $logDir "42-fetch-backends.log"
    $script:pipelineOk = Invoke-Stage -Name "Fetch backends (offline bundle)" -Command "pwsh ci/fetch-backends.ps1" -LogPath $fetchLog -Action {
        $fetchScript = Join-Path $root "ci/fetch-backends.ps1"
        if (-not (Test-Path $fetchScript)) {
            Add-Content -Path $fetchLog -Value "fetch script not found: $fetchScript (skipping, installer will show download links)"
            return 0
        }
        # Run the fetch script; it never fails the build - a missing backend just means the installer shows download links
        $code = Run-Native "pwsh" @("-NoProfile", "-File", $fetchScript) $fetchLog
        # Also try python fallback if pwsh fetch left nothing
        $backendMsi = @(Get-ChildItem (Join-Path $root "installer/backends") -Filter "*.msi" -ErrorAction SilentlyContinue)
        $backendExe = @(Get-ChildItem (Join-Path $root "installer/backends") -Filter "*.exe" -ErrorAction SilentlyContinue)
        if ($backendMsi.Count -eq 0 -or $backendExe.Count -eq 0) {
            Add-Content -Path $fetchLog -Value "pwsh fetch did not produce both backends (msi=$($backendMsi.Count) exe=$($backendExe.Count)), trying python fallback"
            $py = Get-Command python -ErrorAction SilentlyContinue
            if ($py) {
                $pyFetch = Join-Path $root "ci/fetch-backends.py"
                if (Test-Path $pyFetch) {
                    $code2 = Run-Native $py.Source @($pyFetch) $fetchLog
                    Add-Content -Path $fetchLog -Value "python fallback exit code: $code2"
                }
            }
        }
        $backendMsi = @(Get-ChildItem (Join-Path $root "installer/backends") -Filter "*.msi" -ErrorAction SilentlyContinue)
        $backendExe = @(Get-ChildItem (Join-Path $root "installer/backends") -Filter "*.exe" -ErrorAction SilentlyContinue)
        Add-Content -Path $fetchLog -Value "bundled after fetch: msi=$($backendMsi.Count) exe=$($backendExe.Count)"
        foreach ($f in $backendMsi + $backendExe) {
            $mb = [math]::Round($f.Length / 1MB, 2)
            Add-Content -Path $fetchLog -Value "  $($f.Name)  $($f.Length) bytes  $mb MB"
        }
        if ($backendMsi.Count -eq 0 -or $backendExe.Count -eq 0) {
            Add-Content -Path $fetchLog -Value "WARNING: bundle incomplete - installer will show download links for missing backend"
            Add-Content -Path $fetchLog -Value "This is not a fatal error; the installer still builds but is not fully offline"
        } else {
            Add-Content -Path $fetchLog -Value "All backends bundled - installer will be fully offline"
        }
        return 0
    }
    if (-not $script:pipelineOk) { return }

    # 5. installer
    if (-not $SkipInstaller) {
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

            # Pre-flight: every [Files] source must exist before the compiler runs.
            $lintLog = Join-Path $logDir "45-iss-sources.log"
            $lintCode = Run-Native "python" @((Join-Path $root "ci\check_iss_sources.py"), (Join-Path $root "installer\MiniUsbBridge.iss")) $lintLog
            if ($lintCode -ne 0) { throw "installer\MiniUsbBridge.iss references files that do not exist (see 45-iss-sources.log)" }

            return (Run-Native $iscc @((Join-Path $root "installer\MiniUsbBridge.iss")) $isccLog)
        }
        if (-not $script:pipelineOk) { return }
    }

    # 6. verify artifacts
    $verifyLog = Join-Path $logDir "60-verify.log"
    $script:pipelineOk = Invoke-Stage -Name "Verify artifacts" -Command "hash + PE + version checks" -LogPath $verifyLog -Action {
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
            $machine = "-"
            if ($f.Extension -eq ".exe") { $machine = Get-PeMachine $f.FullName }
            $version = ""
            try { $version = (Get-Item $f.FullName).VersionInfo.FileVersion } catch { }
            $sig = "n/a"
            if ($f.Extension -eq ".exe") {
                try { $sig = (Get-AuthenticodeSignature -FilePath $f.FullName).Status.ToString() } catch { $sig = "unknown" }
            }
            [void]$script:artifacts.Add([pscustomobject]@{
                    Name = $f.Name; MB = [math]::Round($f.Length / 1MB, 2); Sha256 = $hash
                    Machine = $machine; Version = $version; Signed = $sig
                })
            $sumLines += "$hash  $($f.Name)"
            Add-Content -Path $verifyLog -Value ("{0}  {1,10} bytes  {2}  {3}  {4}" -f $hash, $f.Length, $machine, $version, $sig)
        }
        Set-Content -Path $sumsPath -Value ($sumLines -join "`n")
        return 0
    }
}

# ---------------------------------------------------------------------------
# the report - written from a finally block, so it always exists
# ---------------------------------------------------------------------------
function Write-Report {
    $lines = New-Object System.Collections.ArrayList
    $trigger = "local"
    if ($env:GITHUB_EVENT_NAME) { $trigger = $env:GITHUB_EVENT_NAME }
    $resultText = if ($script:pipelineOk) { "**PASS** - app, tests and installer built" } else { "**FAIL**" }
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
    [void]$lines.Add("| Runner | $($script:osText) |")
    [void]$lines.Add("| Generated | $((Get-Date).ToUniversalTime().ToString('yyyy-MM-dd HH:mm:ss')) UTC |")
    [void]$lines.Add("")

    [void]$lines.Add("## Tool chain")
    [void]$lines.Add("")
    [void]$lines.Add('```')
    [void]$lines.Add("git checkout")
    [void]$lines.Add("  -> CMake configure   (Visual Studio generator, x64)")
    [void]$lines.Add("  -> MSVC build        (Release, /W4, static core library)")
    [void]$lines.Add("  -> test_core.exe     (crypto, pairing, protocol, USB/IP wire)")
    [void]$lines.Add("  -> dist\app          (MiniUsbBridge.exe + mubb-relay.exe)")
    [void]$lines.Add("  -> check_iss_sources (every [Files] source must exist)")
    [void]$lines.Add("  -> ISCC.exe          (Inno Setup 6 -> setup.exe)")
    [void]$lines.Add("  -> verify + SHA-256  (PE machine, version, signature status)")
    [void]$lines.Add('```')
    [void]$lines.Add("")

    [void]$lines.Add("## Stages")
    [void]$lines.Add("")
    if ($script:stages.Count -eq 0) {
        [void]$lines.Add("_No stage was reached - the script failed before the pipeline started._")
    } else {
        [void]$lines.Add("| # | Stage | Command | Result | Time | Log |")
        [void]$lines.Add("|---|---|---|---|---|---|")
        $i = 0
        foreach ($s in $script:stages) {
            $i++
            $status = "ok"
            if ($s.Code -ne 0) { $status = "**FAILED (exit $($s.Code))**" }
            [void]$lines.Add("| $i | $($s.Name) | ``$($s.Command)`` | $status | $($s.Seconds)s | ``$($s.Log)`` |")
        }
    }
    [void]$lines.Add("")

    if ($failedStage) {
        [void]$lines.Add("## Why it failed")
        [void]$lines.Add("")
        [void]$lines.Add("Stage **$($failedStage.Name)** exited with code $($failedStage.Code).")
        if ($failedStage.Note) {
            [void]$lines.Add("")
            [void]$lines.Add("Exception: ``$($failedStage.Note)``")
        }
        $tailLog = Join-Path $logDir $failedStage.Log
        if ($failedStage.Log -and (Test-Path $tailLog)) {
            # Every diagnostic line first - the tail alone hides earlier errors.
            $pattern = 'error C\d+|error LNK\d+|error MSB\d+|CMake Error|fatal error|FAILED:|^Error:'
            $allErrors = @(Select-String -Path $tailLog -Pattern $pattern -ErrorAction SilentlyContinue)
            if ($allErrors.Count -gt 0) {
                [void]$lines.Add("")
                [void]$lines.Add("### All errors reported ($($allErrors.Count) line(s))")
                [void]$lines.Add("")
                [void]$lines.Add('```')
                $seen = @{}
                foreach ($e in $allErrors) {
                    $line = $e.Line.Trim()
                    if (-not $line -or $seen.ContainsKey($line)) { continue }
                    $seen[$line] = $true
                    [void]$lines.Add($line)
                }
                [void]$lines.Add('```')
            }
            [void]$lines.Add("")
            [void]$lines.Add("Last 40 lines of ``$($failedStage.Log)``:")
            [void]$lines.Add("")
            [void]$lines.Add('```')
            foreach ($l in (Get-Content $tailLog -Tail 40)) { [void]$lines.Add($l) }
            [void]$lines.Add('```')
        }
        [void]$lines.Add("")
    }

    [void]$lines.Add("## Tool versions")
    [void]$lines.Add("")
    if ($script:toolVersions.Count -eq 0) {
        [void]$lines.Add("_No tool version could be collected._")
    } else {
        [void]$lines.Add("| Tool | Version |")
        [void]$lines.Add("|---|---|")
        foreach ($t in $script:toolVersions) { [void]$lines.Add("| $($t.Name) | $($t.Version) |") }
    }
    [void]$lines.Add("")

    [void]$lines.Add("## Artifacts")
    [void]$lines.Add("")
    if ($script:artifacts.Count -eq 0) {
        [void]$lines.Add("_No artifact was verified._")
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
    [void]$lines.Add("* The GUI was **not** launched - the runner has no interactive desktop, so this proves it compiles, links and packages, nothing more.")
    [void]$lines.Add("* No USB hardware, no usbipd-win service and no usbip-win2 driver on the runner: device enumeration and attachment stay **unverified** (``docs/TESTING.md``).")
    [void]$lines.Add("* The installer is compiled but not executed; a clean-VM install is still required.")
    $unsigned = ($script:artifacts | Where-Object { $_.Signed -ne "Valid" -and $_.Signed -ne "n/a" } | Select-Object -First 1)
    if ($unsigned) {
        [void]$lines.Add("* Binaries are **unsigned** (``$($unsigned.Signed)``): SmartScreen will warn on first run until a code-signing certificate is added.")
    }
    [void]$lines.Add("")

    $text = ($lines -join "`n")
    Set-Content -Path $reportPath -Value $text -Encoding utf8
    Write-Host ""
    Write-Host "Report: $reportPath" -ForegroundColor Cyan
}

# ---------------------------------------------------------------------------
# run
# ---------------------------------------------------------------------------
try {
    Collect-ToolVersions
    Invoke-Pipeline
} catch {
    $msg = $_.Exception.Message
    Write-Host ""
    Write-Host "UNHANDLED: $msg" -ForegroundColor Red
    Write-Host ($_.ScriptStackTrace) -ForegroundColor DarkRed
    [void]$script:stages.Add([pscustomobject]@{
            Name = "Unhandled script error"; Command = "-"; Code = 1; Seconds = 0
            Note = $msg; Log = "99-unhandled.log"
        })
    Set-Content -Path (Join-Path $logDir "99-unhandled.log") -Value ("$msg`n`n$($_.ScriptStackTrace)")
    $script:pipelineOk = $false
} finally {
    try { Write-Report } catch { Write-Host "could not write the report: $($_.Exception.Message)" -ForegroundColor Red }
}

if (-not $script:pipelineOk) {
    Write-Host "Pipeline FAILED - see $reportPath" -ForegroundColor Red
    exit 1
}
Write-Host "Pipeline PASSED - artifacts in $distDir, report in $reportPath" -ForegroundColor Green
exit 0
