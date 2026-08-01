<#
.SYNOPSIS
    Removes the F.E.A.R. VR installation.

.DESCRIPTION
    Removes installer-owned stage contents, the desktop shortcut, and the
    app-local d3d9.dll proxy recorded by the installer. Unknown paths and
    reparse points are preserved. The proxy is removed only when its
    current SHA-256 still proves that it belongs to this installation.
    FEAR.exe and retail game data are never modified.

    Saved games and profiles live in <InstallDir>\userdata and are kept
    unless -IncludeUserData is given.

    Without -Apply the run is a dry run.

.PARAMETER InstallDir
    Install folder to remove. Default: %USERPROFILE%\FearVR

    Example: -InstallDir "D:\Games\FearVR"

.PARAMETER IncludeUserData
    Also deletes <InstallDir>\userdata, i.e. saved games, profiles and
    screenshots.

.PARAMETER Apply
    Actually deletes. Without it nothing is changed.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\release\uninstall.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\release\uninstall.ps1 -Apply
#>
[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:USERPROFILE 'FearVR'),

    [switch]$IncludeUserData,

    [switch]$Apply
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_fearvr-release.ps1"
$packageRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

$mode = if ($Apply) { 'APPLY' } else { 'DRY RUN' }
Write-Host "=== F.E.A.R. VR - Uninstall ($mode) ===" -ForegroundColor Cyan

$InstallDir = [IO.Path]::GetFullPath($InstallDir)
Assert-FearVrSafeInstallDirectory `
    -InstallDir $InstallDir `
    -ProtectedRoots @($packageRoot) | Out-Null
if (-not (Test-Path -LiteralPath $InstallDir -PathType Container)) {
    Write-Host "No installation in '$InstallDir'."
    Write-Host 'If it was installed elsewhere, pass -InstallDir "<path>",'
    Write-Host '  for example: -InstallDir "D:\Games\FearVR"'
    return
}
$installMutex = Enter-FearVrInstallMutex $InstallDir
try {
Assert-FearVrRecognizedInstallDirectory $InstallDir

if ($Apply) {
    $running = @(Get-Process -Name 'FEAR' -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) {
        throw ("F.E.A.R. is running (PID $($running.Id -join ', ')). " +
               'Close the game before uninstalling the app-local proxy.')
    }
}

$transaction = Resolve-FearVrProxyTransaction `
    -InstallDir $InstallDir `
    -Apply:$Apply
$rollbackBeforeUninstall = -not $Apply -and
    $transaction.Status -eq 'WouldRollback'
if ($transaction.Status -ne 'None') {
    if ($Apply) {
        Write-Host "  * recovered proxy transaction: $($transaction.Status)"
    } else {
        Write-Host (
            "  * recover proxy transaction first: $($transaction.Status)")
    }
}

$deploymentPath = Join-Path $InstallDir 'deployment.json'
$deployment = Read-FearVrOwnedDeployment `
    $InstallDir -AllowMissing -AllowLegacyProxyless
if ($null -eq $deployment) {
    Write-Host (
        'No owned deployment manifest remains. The installer will not ' +
        'recursively remove this folder.') -ForegroundColor Yellow
    return
}
$deploymentHash = Get-FileSha256 $deploymentPath
$retailRoot = [string]$deployment.retailRoot
Assert-FearVrSafeInstallDirectory `
    -InstallDir $InstallDir `
    -ProtectedRoots @(
        $packageRoot,
        $retailRoot,
        [string]$deployment.publicToolsGame) | Out-Null

# A committed move between Retail roots records cleanup in deployment.json.
# Resolve it before removing the current proxy so an interrupted cleanup is
# recoverable without trusting files that are no longer recorded.
if ($deployment -and
    $deployment.PSObject.Properties.Name -contains
        'pendingD3d9ProxyCleanup') {
    $pendingCleanup = Invoke-FearVrPendingProxyCleanup `
        -DeploymentPath $deploymentPath `
        -Deployment $deployment `
        -Apply:$Apply
    Write-Host (
        "  * previous proxy cleanup: $($pendingCleanup.ProxyStatus) " +
        "$($pendingCleanup.Path)")
    if ($Apply -and $pendingCleanup.Status -eq 'Deferred') {
        throw (
            'Cannot safely identify the previous Retail proxy target. ' +
            'Its cleanup record was preserved; restore the original path ' +
            'mapping before uninstalling again.')
    }
    if ($Apply) {
        $deployment = Read-FearVrOwnedDeployment `
            $InstallDir -AllowLegacyProxyless
        $deploymentHash = Get-FileSha256 $deploymentPath
        $retailRoot = $deployment.retailRoot
    }
}
$retailBefore = $null
if ($retailRoot -and (Test-Path -LiteralPath $retailRoot -PathType Container)) {
    $retailBefore = Assert-RetailFearExe $retailRoot
}

function Get-SizeMb([string]$Path) {
    $bytes = (Get-ChildItem -LiteralPath $Path -Recurse -File `
        -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum).Sum
    return [math]::Round(($bytes / 1MB), 1)
}

# Consume the ownership record before deployment.json is removed. A changed
# or foreign d3d9.dll is always preserved.
if ($rollbackBeforeUninstall) {
    Write-Host (
        '  * roll back the unfinished proxy update, then re-check and ' +
        'remove only the deployment-owned proxy')
} elseif ($deployment -and
    $deployment.PSObject.Properties.Name -contains 'd3d9Proxy') {
    $proxyRemoval = Remove-FearVrAppLocalProxy `
        -RetailRoot ([string]$retailRoot) `
        -Record $deployment.d3d9Proxy
    switch ($proxyRemoval.Status) {
        'WouldRemove' {
            Write-Host "  * remove app-local proxy $($proxyRemoval.Path)"
            if ($Apply) {
                $removed = Remove-FearVrAppLocalProxy `
                    -RetailRoot ([string]$retailRoot) `
                    -Record $deployment.d3d9Proxy `
                    -Apply
                if ($removed.Status -ne 'Removed') {
                    throw 'The app-local proxy changed during uninstall.'
                }
            }
        }
        'Modified' {
            Write-Host (
                '  * keeping modified/foreign app-local d3d9.dll: ' +
                $proxyRemoval.Path) -ForegroundColor Yellow
        }
        'InvalidRecord' {
            Write-Host (
                '  * keeping app-local d3d9.dll because proxy metadata is ' +
                'invalid') -ForegroundColor Yellow
        }
        'NoRecord' {
            Write-Host (
                '  * keeping app-local d3d9.dll because ownership metadata ' +
                'is absent') -ForegroundColor Yellow
        }
        'Missing' {
            Write-Host '  * app-local F.E.A.R. VR proxy is already absent'
        }
        default {
            throw "Unexpected app-local proxy state: $($proxyRemoval.Status)"
        }
    }
}

# Remove only manifest-recorded files whose current hash still matches.
# Unknown and modified files remain in place. deployment.json is deliberately
# excluded here and removed last so an interrupted uninstall is resumable.
$moduleDirectory = [string]$deployment.moduleDirectory
$installPhysical = (Get-FearVrPhysicalPath $InstallDir).TrimEnd('\')
$modulePhysical = (Get-FearVrPhysicalPath $moduleDirectory).TrimEnd('\')
if (-not $modulePhysical.StartsWith(
        $installPhysical + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The module directory escapes the owned install folder.'
}

function Remove-RecordedFile(
    [string]$Path,
    [string]$Sha256,
    [string]$Label
) {
    $currentHash = Get-FileSha256 $Path
    if (-not $currentHash) {
        Write-Host "  * $Label is already absent"
        return
    }
    if ($currentHash -ne $Sha256) {
        Write-Host "  * keeping modified/unrecognized $Label" `
            -ForegroundColor Yellow
        return
    }
    Write-Host "  * remove $Label"
    if (-not $Apply) { return }
    $parentPhysical = Get-FearVrPhysicalPath ([IO.Path]::GetDirectoryName($Path))
    if (-not $parentPhysical.TrimEnd('\').StartsWith(
            $installPhysical + '\',
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove an owned file outside InstallDir: $Path"
    }
    $status = Remove-FearVrFileByOwnedHash `
        -Path $Path `
        -ExpectedSha256 $Sha256 `
        -ExpectedPhysicalParent $parentPhysical
    if ($status -notin @('Removed', 'Missing')) {
        throw "Owned file changed during uninstall ($status): $Path"
    }
}

foreach ($record in @($deployment.files)) {
    Remove-RecordedFile `
        -Path (Join-Path $moduleDirectory ([string]$record.name)) `
        -Sha256 ([string]$record.sha256) `
        -Label ("module " + [string]$record.name)
}
foreach ($record in @($deployment.generatedFiles)) {
    Remove-RecordedFile `
        -Path (Join-Path $moduleDirectory ([string]$record.relativePath)) `
        -Sha256 ([string]$record.sha256) `
        -Label ("generated file " + [string]$record.relativePath)
}
if ($deployment.PSObject.Properties.Name -contains 'archiveConfigSha256' -and
    [string]$deployment.archiveConfigSha256 -match '^[0-9A-Fa-f]{64}$') {
    Remove-RecordedFile `
        -Path ([string]$deployment.archiveConfig) `
        -Sha256 ([string]$deployment.archiveConfigSha256) `
        -Label 'archive configuration'
} else {
    Write-Host '  * keeping archive configuration (ownership hash is absent)' `
        -ForegroundColor Yellow
}

if (Test-Path -LiteralPath ([string]$deployment.userDirectory) `
        -PathType Container) {
    if ($IncludeUserData) {
        Write-Host '  * remove userdata (explicitly requested)'
        if ($Apply) {
            Remove-FearVrOwnedStagePath `
                -InstallDir $InstallDir `
                -Path ([string]$deployment.userDirectory) `
                -Recurse
        }
    } else {
        Write-Host ("  * keeping userdata (saved games, " +
                    "$(Get-SizeMb ([string]$deployment.userDirectory)) MB)")
    }
}

if (Test-Path -LiteralPath ([string]$deployment.logDirectory) `
        -PathType Container) {
    Write-Host '  * keeping logs (mutable files are not hash-owned)'
}

# Only remove the shortcut that points at the folder being uninstalled. A
# second installation elsewhere keeps its own shortcut.
$shortcut = Join-Path ([Environment]::GetFolderPath('Desktop')) 'F.E.A.R. VR.lnk'
if (Test-Path -LiteralPath $shortcut -PathType Leaf) {
    $arguments = ''
    try {
        $arguments = (New-Object -ComObject WScript.Shell).CreateShortcut(
            $shortcut).Arguments
    } catch { }
    $installArgument = [Text.RegularExpressions.Regex]::Match(
        $arguments, '(?i)(?:^|\s)-InstallDir\s+"([^"]+)"')
    $shortcutOwned = $installArgument.Success -and
        (Test-FearVrSamePhysicalPath `
            $installArgument.Groups[1].Value $InstallDir)
    if ($shortcutOwned) {
        Write-Host '  * remove desktop shortcut'
        if ($Apply) { Remove-Item -LiteralPath $shortcut -Force }
    } else {
        Write-Host '  * keeping desktop shortcut (points elsewhere)'
    }
}

if ($retailBefore) {
    $retailAfter = Assert-RetailFearExe $retailRoot
    if ($retailBefore.Sha256 -ne $retailAfter.Sha256) {
        throw 'SAFETY ABORT: the retail FEAR.exe was modified.'
    }
    Write-Host ''
    Write-Host 'FEAR.exe and retail game data are unchanged.'
}

Write-Host '  * remove deployment ownership manifest last'
if ($Apply) {
    $manifestStatus = Remove-FearVrFileByOwnedHash `
        -Path $deploymentPath `
        -ExpectedSha256 $deploymentHash `
        -ExpectedPhysicalParent $installPhysical
    if ($manifestStatus -notin @('Removed', 'Missing')) {
        throw "Deployment manifest changed during uninstall: $manifestStatus"
    }

    # Empty directories are removed non-recursively, so an unexpected child
    # or reparse point is preserved rather than followed.
    foreach ($directory in @(
        (Join-Path $moduleDirectory 'fearvr'),
        (Join-Path $moduleDirectory 'chars\materials'),
        (Join-Path $moduleDirectory 'chars'),
        $moduleDirectory,
        [string]$deployment.logDirectory,
        [string]$deployment.userDirectory,
        $InstallDir)) {
        if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
            continue
        }
        $item = Get-Item -LiteralPath $directory -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            continue
        }
        if (@(Get-ChildItem -LiteralPath $directory -Force).Count -eq 0) {
            [IO.Directory]::Delete($directory, $false)
        }
    }
    if (Test-Path -LiteralPath $InstallDir -PathType Container) {
        Write-Host (
            "  Note: '$InstallDir' is kept because it still contains " +
            'userdata, logs, or unrecognized files.')
    }
}

Write-Host ''
if ($Apply) {
    Write-Host 'Uninstall complete.' -ForegroundColor Green
} else {
    Write-Host 'Dry run finished; nothing was changed.' -ForegroundColor Yellow
    Write-Host 'Re-run with -Apply to actually remove.'
}
} finally {
    Exit-FearVrInstallMutex $installMutex
}
