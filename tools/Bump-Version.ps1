# File purpose: update the compiled StatusLight app version before publishing.
# Responsibilities:
# 1. Read the current version from src/AppVersion.h.
# 2. Read the latest release tag from Gitee.
# 3. Keep a newer local version, or bump the latest remote patch version by one.
#
# Not responsible for:
# - Building the executable.
# - Creating Gitee releases.
# - Uploading release assets.
#
# Maintenance note:
# Run this before a release build so the executable version and release tag stay aligned.
param(
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$versionFile = Join-Path $repoRoot 'src\AppVersion.h'
$latestReleaseUrl = 'https://gitee.com/api/v5/repos/yuan_yi/codex-status-light/releases/latest'

function ConvertTo-VersionParts {
    param(
        [Parameter(Mandatory = $true)]
        [string]$VersionText
    )

    $normalized = $VersionText.Trim()
    if ($normalized.StartsWith('v')) {
        $normalized = $normalized.Substring(1)
    }

    if ($normalized -notmatch '^(\d+)\.(\d+)\.(\d+)$') {
        throw "Unsupported version format: $VersionText"
    }

    return [PSCustomObject]@{
        Text = $normalized
        Major = [int]$Matches[1]
        Minor = [int]$Matches[2]
        Patch = [int]$Matches[3]
    }
}

function Compare-VersionParts {
    param(
        [Parameter(Mandatory = $true)]
        $Left,
        [Parameter(Mandatory = $true)]
        $Right
    )

    foreach ($part in @('Major', 'Minor', 'Patch')) {
        if ($Left.$part -gt $Right.$part) {
            return 1
        }
        if ($Left.$part -lt $Right.$part) {
            return -1
        }
    }

    return 0
}

function Get-CurrentVersion {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $content = Get-Content -Raw -Encoding utf8 -LiteralPath $Path
    $match = [regex]::Match($content, 'kStatusLightVersion\s*=\s*"([^"]+)"')
    if (!$match.Success) {
        throw "Cannot find kStatusLightVersion in $Path"
    }

    return $match.Groups[1].Value
}

function Get-LatestReleaseTag {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Url
    )

    $response = Invoke-RestMethod -Uri $Url -Headers @{ Accept = 'application/json' } -TimeoutSec 20
    if (!$response.tag_name) {
        throw "Gitee latest release response does not include tag_name"
    }

    return [string]$response.tag_name
}

function Set-AppVersion {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Version
    )

    $content = Get-Content -Raw -Encoding utf8 -LiteralPath $Path
    $content = [regex]::Replace(
        $content,
        'kStatusLightVersion\s*=\s*"[^"]+"',
        "kStatusLightVersion = `"$Version`""
    )
    $content = [regex]::Replace(
        $content,
        'kStatusLightVersionWide\s*=\s*L"[^"]+"',
        "kStatusLightVersionWide = L`"$Version`""
    )

    $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText($Path, $content, $utf8NoBom)
}

$currentVersion = ConvertTo-VersionParts -VersionText (Get-CurrentVersion -Path $versionFile)
$latestVersion = ConvertTo-VersionParts -VersionText (Get-LatestReleaseTag -Url $latestReleaseUrl)
$comparison = Compare-VersionParts -Left $currentVersion -Right $latestVersion

if ($comparison -gt 0) {
    $nextVersion = $currentVersion.Text
    $reason = 'local version is already newer than latest release'
} else {
    $nextVersion = '{0}.{1}.{2}' -f $latestVersion.Major, $latestVersion.Minor, ($latestVersion.Patch + 1)
    $reason = 'local version is not newer than latest release; bumped patch'
}

if (!$DryRun) {
    Set-AppVersion -Path $versionFile -Version $nextVersion
}

[PSCustomObject]@{
    CurrentVersion = $currentVersion.Text
    LatestRelease = 'v' + $latestVersion.Text
    NextVersion = $nextVersion
    Changed = (!$DryRun -and $currentVersion.Text -ne $nextVersion)
    Reason = $reason
    DryRun = [bool]$DryRun
}
