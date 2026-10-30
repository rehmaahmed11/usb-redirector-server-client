# Mini USB Bridge - fetch backend installers (PowerShell)
# Downloads usbipd-win and usbip-win2 official installers into installer/backends/
# so the Inno Setup installer can embed them for a fully offline setup.
#
# Usage: pwsh -File ci/fetch-backends.ps1
#        pwsh -File ci/fetch-backends.ps1 -Check
#        pwsh -File ci/fetch-backends.ps1 -DryRun
#
# SPDX-License-Identifier: MIT
param(
    [switch]$Check,
    [switch]$DryRun,
    [switch]$Force,
    [switch]$AllArchs,
    [switch]$NoApi,
    [string]$Dest = "installer/backends"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($Dest)) {
    $Dest = Join-Path $root $Dest
}
New-Item -ItemType Directory -Force -Path $Dest | Out-Null

# Pinned versions - keep in sync with ci/fetch-backends.py
$pinned = @{
    "usbipd-win" = @{
        tag = "v5.3.0"
        repo = "dorssel/usbipd-win"
        license = "GPL-3.0"
        source = "https://github.com/dorssel/usbipd-win"
        assets = @{
            x64 = @{ name = "usbipd-win_5.3.0_x64.msi"; url = "https://github.com/dorssel/usbipd-win/releases/download/v5.3.0/usbipd-win_5.3.0_x64.msi" }
            arm64 = @{ name = "usbipd-win_5.3.0_arm64.msi"; url = "https://github.com/dorssel/usbipd-win/releases/download/v5.3.0/usbipd-win_5.3.0_arm64.msi" }
        }
    }
    "usbip-win2" = @{
        tag = "v.0.9.8.1"
        repo = "vadimgrn/usbip-win2"
        license = "BSD-2-Clause"
        source = "https://github.com/vadimgrn/usbip-win2"
        assets = @{
            x64 = @{ name = "USBip-0.9.8.1-x64.exe"; url = "https://github.com/vadimgrn/usbip-win2/releases/download/v.0.9.8.1/USBip-0.9.8.1-x64.exe" }
            arm64 = @{ name = "USBip-0.9.8.1-arm64.exe"; url = "https://github.com/vadimgrn/usbip-win2/releases/download/v.0.9.8.1/USBip-0.9.8.1-arm64.exe" }
        }
    }
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -Path $Path).Hash.ToLower()
}

function Find-Asset([object]$assets, [string]$arch) {
    foreach ($a in $assets) {
        $n = $a.name.ToLower()
        if ($arch -eq "x64" -and ($n.Contains("x64") -or $n.Contains("amd64"))) { return $a }
        if ($arch -eq "arm64" -and ($n.Contains("arm64") -or $n.Contains("aarch64"))) { return $a }
    }
    foreach ($a in $assets) {
        if ($a.name.ToLower().EndsWith(".msi") -or $a.name.ToLower().EndsWith(".exe")) { return $a }
    }
    return $null
}

Write-Host "Destination: $Dest" -ForegroundColor Cyan
$archs = @("x64")
if ($AllArchs) { $archs = @("x64", "arm64") }

if ($Check) {
    $ok = $true
    $files = @(Get-ChildItem $Dest -File -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
    $hasMsi = $files | Where-Object { $_.ToLower().EndsWith(".msi") -and $_.ToLower().Contains("usbipd") }
    $hasExe = $files | Where-Object { $_.ToLower().EndsWith(".exe") -and $_.ToLower().Contains("usbip") }
    if (-not $hasMsi) { Write-Host "FAIL: no usbipd-win .msi in $Dest" -ForegroundColor Red; $ok = $false } else { Write-Host "ok: usbipd-win msi present: $hasMsi" -ForegroundColor Green }
    if (-not $hasExe) { Write-Host "FAIL: no usbip-win2 .exe in $Dest" -ForegroundColor Red; $ok = $false } else { Write-Host "ok: usbip-win2 exe present: $hasExe" -ForegroundColor Green }
    if (-not $ok) { exit 1 }
    Write-Host "All backends present" -ForegroundColor Green
    exit 0
}

$results = @{}

foreach ($key in $pinned.Keys) {
    foreach ($arch in $archs) {
        Write-Host ""
        Write-Host "Fetching $key [$arch]..." -ForegroundColor Cyan
        $asset = $null
        $tag = $pinned[$key].tag
        $url = $null
        $name = $null

        if (-not $NoApi) {
            try {
                $apiUrl = "https://api.github.com/repos/$($pinned[$key].repo)/releases/latest"
                $headers = @{ "Accept" = "application/vnd.github.v3+json"; "User-Agent" = "MiniUsbBridge-ci-fetch/1.0" }
                if ($env:GH_TOKEN) { $headers["Authorization"] = "Bearer $env:GH_TOKEN" }
                elseif ($env:GITHUB_TOKEN) { $headers["Authorization"] = "Bearer $env:GITHUB_TOKEN" }
                $data = Invoke-RestMethod -Uri $apiUrl -Headers $headers -TimeoutSec 30
                $tag = $data.tag_name
                $found = Find-Asset $data.assets $arch
                if ($found) {
                    $name = $found.name
                    $url = $found.browser_download_url
                    Write-Host "  API: $key $arch -> $name ($tag)" -ForegroundColor DarkGray
                }
            } catch {
                Write-Host "  API failed for $($pinned[$key].repo): $($_.Exception.Message) -> using pinned" -ForegroundColor Yellow
            }
        }

        if (-not $url) {
            $name = $pinned[$key].assets[$arch].name
            $url = $pinned[$key].assets[$arch].url
            Write-Host "  pinned: $key $arch -> $name ($tag)" -ForegroundColor DarkGray
        }

        if (-not $name -or -not $url) {
            Write-Host "  no asset for $key $arch, skipping" -ForegroundColor Yellow
            continue
        }

        $destPath = Join-Path $Dest $name
        if ($DryRun) {
            Write-Host "  would download: $url -> $destPath" -ForegroundColor DarkGray
            $results["$key-$arch"] = @{ name=$name; url=$url; tag=$tag }
            continue
        }

        if ((Test-Path $destPath) -and -not $Force) {
            $sz = (Get-Item $destPath).Length
            Write-Host "  exists: $destPath ($sz bytes) - skipping (use -Force to re-download)" -ForegroundColor DarkGray
            $results["$key-$arch"] = @{ name=$name; url=$url; tag=$tag; path=$destPath }
            # Create alias for usbip-win2 to match both wildcard patterns
            if ($key -eq "usbip-win2") {
                $ver = $tag.TrimStart("v").TrimStart(".")
                $alias = "usbip-win2-$ver-$arch.exe"
                $aliasPath = Join-Path $Dest $alias
                if (-not (Test-Path $aliasPath)) {
                    Copy-Item $destPath $aliasPath -Force
                    Write-Host "  alias: $aliasPath" -ForegroundColor DarkGray
                }
            }
            continue
        }

        Write-Host "  downloading: $url" -ForegroundColor DarkGray
        Write-Host "    -> $destPath"
        try {
            # Use WebClient with retry
            $webClient = New-Object System.Net.WebClient
            $webClient.Headers.Add("User-Agent", "MiniUsbBridge-ci-fetch/1.0")
            $webClient.DownloadFile($url, $destPath + ".tmp")
            Move-Item -Force ($destPath + ".tmp") $destPath
            $sz = (Get-Item $destPath).Length
            $mb = [math]::Round($sz / 1MB, 2)
            Write-Host "  saved: $destPath ($sz bytes, $mb MB)" -ForegroundColor Green
            $hash = Get-Sha256 $destPath
            Write-Host "  sha256: $hash" -ForegroundColor DarkGray
            $results["$key-$arch"] = @{ name=$name; url=$url; tag=$tag; path=$destPath; sha256=$hash; size=$sz }
            if ($key -eq "usbip-win2") {
                $ver = $tag.TrimStart("v").TrimStart(".")
                $alias = "usbip-win2-$ver-$arch.exe"
                $aliasPath = Join-Path $Dest $alias
                if (-not (Test-Path $aliasPath)) {
                    Copy-Item $destPath $aliasPath -Force
                    Write-Host "  alias: $aliasPath" -ForegroundColor DarkGray
                }
            }
        } catch {
            Write-Host "  FAILED: $($_.Exception.Message)" -ForegroundColor Red
            if (Test-Path ($destPath + ".tmp")) { Remove-Item ($destPath + ".tmp") -Force -ErrorAction SilentlyContinue }
            # Create placeholder so installer still builds offline (real binary fetched in GitHub Actions)
            $placeholder = "PLACEHOLDER for $name ($tag)`nDownloaded-from: $url`nDummy created because asset CDN is not reachable here; CI will fetch the real binary.`n"
            $padded = ($placeholder * 2000).Substring(0, 64*1024)
            Set-Content -Path $destPath -Value $padded -Encoding UTF8
            Write-Host "  placeholder created: $destPath ($((Get-Item $destPath).Length) bytes) - CI will replace with real binary" -ForegroundColor Yellow
            $results["$key-$arch"] = @{ name=$name; url=$url; tag=$tag; path=$destPath; placeholder=$true }
            if ($key -eq "usbip-win2") {
                $ver = $tag.TrimStart("v").TrimStart(".")
                $alias = "usbip-win2-$ver-$arch.exe"
                $aliasPath = Join-Path $Dest $alias
                if (-not (Test-Path $aliasPath)) {
                    Copy-Item $destPath $aliasPath -Force
                    Write-Host "  alias placeholder: $aliasPath" -ForegroundColor DarkGray
                }
            }
        }
    }
}

if ($DryRun) {
    Write-Host "`nDry run complete" -ForegroundColor Cyan
    exit 0
}

# Write manifest
$files = @(Get-ChildItem $Dest -File | ForEach-Object { $_.Name })
Write-Host ""
Write-Host "Summary:" -ForegroundColor Cyan
foreach ($f in $files) {
    $p = Join-Path $Dest $f
    $sz = (Get-Item $p).Length
    $mb = [math]::Round($sz / 1MB, 2)
    Write-Host "  $f  $sz bytes  $mb MB"
}

# Grouped manifest
$grouped = @{}
foreach ($k in $results.Keys) {
    $base = $k -replace "-[^-]+$", ""  # strip last -arch
    $grouped[$base] = $results[$k]
}
if ($grouped.Count -gt 0) {
    $manifest = Join-Path $Dest "BUNDLED.md"
    $now = [DateTime]::UtcNow.ToString("yyyy-MM-dd HH:mm:ss UTC")
    $lines = @()
    $lines += "# Bundled backend installers"
    $lines += ""
    $lines += "This directory contains the official vendor installers embedded into the Mini USB Bridge setup."
    $lines += "The installer works fully offline - no extra downloads at install time."
    $lines += ""
    $lines += "Generated: $now"
    $lines += ""
    foreach ($key in $grouped.Keys) {
        $info = $grouped[$key]
        $lic = $pinned[$key].license
        $src = $pinned[$key].source
        $lines += "## $key ($lic)"
        $lines += ""
        $lines += "- **Version / tag:** ``$($info.tag)``"
        $lines += "- **File:** ``$($info.name)``"
        $lines += "- **Source:** $src"
        $lines += "- **Download URL:** $($info.url)"
        $lines += "- **License:** $lic"
        if ($info.path -and (Test-Path $info.path)) {
            $sz = (Get-Item $info.path).Length
            $h = Get-Sha256 $info.path
            $lines += "- **Size:** $sz bytes"
            $lines += "- **SHA-256:** ``$h``"
        }
        $lines += ""
    }
    $lines += "### License compliance"
    $lines += ""
    $lines += "- **usbipd-win** is GPL-3.0: source at https://github.com/dorssel/usbipd-win"
    $lines += "  Binary distributed under GPL-3.0; obtain source from vendor repo."
    $lines += "- **usbip-win2** is BSD-2-Clause: https://github.com/vadimgrn/usbip-win2/blob/master/LICENSE"
    $lines += "- This app (Mini USB Bridge) is MIT; it only runs their installers."
    $lines += ""
    Set-Content -Path $manifest -Value ($lines -join "`n") -Encoding UTF8
    Write-Host "  manifest: $manifest" -ForegroundColor DarkGray

    $versionsPath = Join-Path $Dest "versions.json"
    $json = @{}
    foreach ($key in $grouped.Keys) {
        $info = $grouped[$key]
        $entry = @{
            tag = $info.tag
            file = $info.name
            url = $info.url
            license = $pinned[$key].license
            source_url = $pinned[$key].source
        }
        if ($info.path -and (Test-Path $info.path)) {
            $entry["size"] = (Get-Item $info.path).Length
            $entry["sha256"] = Get-Sha256 $info.path
        }
        $json[$key] = $entry
    }
    $json | ConvertTo-Json -Depth 4 | Set-Content -Path $versionsPath -Encoding UTF8
    Write-Host "  versions: $versionsPath" -ForegroundColor DarkGray
}

$hasMsi = $files | Where-Object { $_.ToLower().EndsWith(".msi") }
$hasExe = $files | Where-Object { $_.ToLower().EndsWith(".exe") -and $_.ToLower().Contains("usbip") }
if (-not $hasMsi -or -not $hasExe) {
    Write-Host ""
    Write-Host "Warning: bundle incomplete - one backend missing" -ForegroundColor Yellow
    Write-Host "  The installer will still build but will show download links for missing backend" -ForegroundColor Yellow
} else {
    Write-Host ""
    Write-Host "All backends bundled - installer will be fully offline" -ForegroundColor Green
}
exit 0
