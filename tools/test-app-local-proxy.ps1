[CmdletBinding()]
param(
    [string]$HelperPath = (
        Join-Path $PSScriptRoot 'release\_fearvr-release.ps1')
)

$ErrorActionPreference = 'Stop'
. $HelperPath

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'fearvr-proxy-test-' + [Guid]::NewGuid().ToString('N'))
$source = Join-Path $testRoot 'fearvr-d3d9.dll'
$retail = Join-Path $testRoot 'retail'
$foreignRetail = Join-Path $testRoot 'foreign-retail'

try {
    New-Item -ItemType Directory -Path $retail, $foreignRetail |
        Out-Null
    [IO.File]::WriteAllBytes($source, [byte[]](1, 2, 3, 4))

    $record = Install-FearVrAppLocalProxy `
        -SourcePath $source `
        -RetailRoot $retail `
        -PreviousRecord $null
    Assert-True (
        (Get-FearVrAppLocalProxyState $retail $record).Status -eq 'Owned'
    ) 'fresh install must be owned'

    $sameRecord = Install-FearVrAppLocalProxy `
        -SourcePath $source `
        -RetailRoot $retail `
        -PreviousRecord $record
    Assert-True (
        $sameRecord.sha256 -eq $record.sha256
    ) 'idempotent install must retain the same hash'

    [IO.File]::WriteAllBytes($source, [byte[]](5, 6, 7, 8))
    $updatedRecord = Install-FearVrAppLocalProxy `
        -SourcePath $source `
        -RetailRoot $retail `
        -PreviousRecord $record
    Assert-True (
        $updatedRecord.sha256 -ne $record.sha256
    ) 'owned proxy update must replace the old hash'

    $foreignTarget = Join-Path $foreignRetail 'd3d9.dll'
    [IO.File]::WriteAllBytes($foreignTarget, [byte[]](9, 9, 9))
    $conflictRejected = $false
    try {
        Install-FearVrAppLocalProxy `
            -SourcePath $source `
            -RetailRoot $foreignRetail `
            -PreviousRecord $null | Out-Null
    } catch {
        $conflictRejected = $true
    }
    Assert-True $conflictRejected 'foreign d3d9.dll must not be overwritten'
    Assert-True (
        (Get-FileSha256 $foreignTarget) -ne $updatedRecord.sha256
    ) 'foreign file must remain unchanged'

    $dryRun = Remove-FearVrAppLocalProxy `
        -RetailRoot $retail `
        -Record $updatedRecord
    Assert-True (
        $dryRun.Status -eq 'WouldRemove' -and
        (Test-Path -LiteralPath $dryRun.Path -PathType Leaf)
    ) 'dry-run must report and preserve an owned proxy'

    [IO.File]::WriteAllBytes($dryRun.Path, [byte[]](10, 11, 12))
    $tampered = Remove-FearVrAppLocalProxy `
        -RetailRoot $retail `
        -Record $updatedRecord `
        -Apply
    Assert-True (
        $tampered.Status -eq 'Modified' -and
        (Test-Path -LiteralPath $tampered.Path -PathType Leaf)
    ) 'tampered proxy must be preserved'

    [IO.File]::Copy($source, $tampered.Path, $true)
    $removed = Remove-FearVrAppLocalProxy `
        -RetailRoot $retail `
        -Record $updatedRecord `
        -Apply
    Assert-True (
        $removed.Status -eq 'Removed' -and
        -not (Test-Path -LiteralPath $removed.Path)
    ) 'matching owned proxy must be removed'

    Write-Host 'App-local proxy lifecycle tests passed.' -ForegroundColor Green
} finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
