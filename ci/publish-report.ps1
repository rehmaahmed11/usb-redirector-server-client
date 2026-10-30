# Mini USB Bridge - publish the CI tool chain report where a human (or an agent
# without access to the Actions log viewer) can read it.
#
#   * on a pull request  -> create or update one PR comment (found by marker)
#   * otherwise          -> create a check run whose output text is the report
#
# Never fails the job: a broken reporter must not turn a green build red.
#
# Env: GH_TOKEN, GITHUB_REPOSITORY, GITHUB_SHA, GITHUB_EVENT_NAME,
#      GITHUB_EVENT_PATH, GITHUB_RUN_ID, GITHUB_SERVER_URL, GITHUB_JOB
# SPDX-License-Identifier: MIT
param(
    [string]$Marker = "<!-- mub-ci-report -->",
    [string]$Title = "tool chain report",
    [string]$Directory = "dist/ci"
)

$ErrorActionPreference = "Stop"

function Get-ReportBody {
    $parts = New-Object System.Collections.ArrayList
    [void]$parts.Add($Marker)
    [void]$parts.Add("### $Title")
    [void]$parts.Add("")
    if ($env:GITHUB_RUN_ID) {
        [void]$parts.Add("Run: $($env:GITHUB_SERVER_URL)/$($env:GITHUB_REPOSITORY)/actions/runs/$($env:GITHUB_RUN_ID) - auto-posted by ``ci/publish-report.ps1``.")
        [void]$parts.Add("")
    }

    $syntax = Join-Path $Directory "00-syntax.txt"
    if (Test-Path $syntax) {
        $text = Get-Content $syntax -Raw
        if ($text.Trim().Length -gt 0) {
            [void]$parts.Add("#### Script syntax check")
            [void]$parts.Add('```')
            [void]$parts.Add($text.Trim())
            [void]$parts.Add('```')
            [void]$parts.Add("")
        }
    }

    $reports = @()
    if (Test-Path $Directory) {
        $reports = @(Get-ChildItem $Directory -Filter "toolchain-report*.md" -ErrorAction SilentlyContinue |
                     Sort-Object Name)
    }
    if ($reports.Count -eq 0) {
        [void]$parts.Add("**No tool chain report was produced.** The build script failed before it could write one.")
        [void]$parts.Add("")
    } else {
        foreach ($r in $reports) {
            [void]$parts.Add((Get-Content $r.FullName -Raw))
            [void]$parts.Add("")
        }
    }

    if (Test-Path (Join-Path $Directory "logs")) {
        $logs = @(Get-ChildItem (Join-Path $Directory "logs") -File -ErrorAction SilentlyContinue | Sort-Object Name)
        if ($logs.Count -gt 0) {
            [void]$parts.Add("#### Logs captured")
            [void]$parts.Add("")
            [void]$parts.Add("| Log | Size |")
            [void]$parts.Add("|---|---|")
            foreach ($l in $logs) {
                [void]$parts.Add("| ``$($l.Name)`` | $([math]::Round($l.Length / 1KB, 1)) KB |")
            }
            [void]$parts.Add("")
        }
    }

    $text = ($parts -join "`n")
    if ($text.Length -gt 60000) { $text = $text.Substring(0, 60000) + "`n`n_(truncated)_" }
    return $text
}

try {
    $body = Get-ReportBody
    $repo = $env:GITHUB_REPOSITORY
    if (-not $repo) { Write-Host "no GITHUB_REPOSITORY - nothing to publish"; exit 0 }

    # Pull request number, when this run came from one.
    $prNumber = $null
    if ($env:GITHUB_EVENT_PATH -and (Test-Path $env:GITHUB_EVENT_PATH)) {
        try {
            $event = Get-Content $env:GITHUB_EVENT_PATH -Raw | ConvertFrom-Json
            if ($event.pull_request -and $event.pull_request.number) { $prNumber = $event.pull_request.number }
            elseif ($event.issue -and $event.issue.pull_request) { $prNumber = $event.issue.number }
        } catch { }
    }

    $payloadFile = Join-Path ([System.IO.Path]::GetTempPath()) "mub-report.json"
    if ($prNumber) {
        $existing = $null
        try {
            $existing = (& gh api "repos/$repo/issues/$prNumber/comments?per_page=100" --paginate `
                    --jq ".[] | select(.body | contains(`"$Marker`")) | .id" 2>$null | Select-Object -First 1)
        } catch { }
        @{ body = $body } | ConvertTo-Json -Depth 5 | Set-Content $payloadFile -Encoding utf8
        if ($existing) {
            & gh api -X PATCH "repos/$repo/issues/comments/$existing" --input $payloadFile | Out-Null
            Write-Host "updated PR comment $existing"
        } else {
            & gh api -X POST "repos/$repo/issues/$prNumber/comments" --input $payloadFile | Out-Null
            Write-Host "created a PR comment on #$prNumber"
        }
        exit 0
    }

    # No PR (merge to main): attach the report to the commit as a check run instead,
    # which keeps it readable through the API without adding noise anywhere.
    $conclusion = "success"
    if ($env:GITHUB_JOB -and $env:MUB_CI_FAILED -eq "1") { $conclusion = "failure" }
    $payload = @{
        name       = "tool chain report"
        head_sha   = $env:GITHUB_SHA
        status     = "completed"
        conclusion = $conclusion
        output     = @{ title = $Title; summary = $body.Substring(0, [Math]::Min(60000, $body.Length)) }
    }
    $payload | ConvertTo-Json -Depth 6 | Set-Content $payloadFile -Encoding utf8
    & gh api -X POST "repos/$repo/check-runs" --input $payloadFile | Out-Null
    Write-Host "published a check run with the report"
    exit 0
} catch {
    Write-Host "could not publish the report: $($_.Exception.Message)"
    exit 0
}
