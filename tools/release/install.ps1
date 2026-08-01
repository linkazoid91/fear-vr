<#
.SYNOPSIS
    Sets up F.E.A.R. VR on this machine.

.DESCRIPTION
    Builds an isolated game stage in a folder of its own. FEAR.exe and retail
    game data remain unchanged. One reversible, hash-tracked app-local
    d3d9.dll proxy is installed beside FEAR.exe so interception happens before
    the game creates its Direct3D device.

    Updating is the same command: run this script from the new package. An
    existing installation is detected, its paths are reused, its modules are
    replaced, and saved games under userdata are kept. Uninstalling first is
    not necessary.

    The package ships our own MIT-licensed modules only. The five Public
    Tools modules are copied from this machine's local Public Tools
    installation; they are deliberately not part of the package.

    Both the game folder and the Public Tools folder are detected
    automatically. If a path cannot be found, the script asks for it and
    shows examples.

.PARAMETER InstallDir
    Target folder. Default: %USERPROFILE%\FearVR

    Do NOT install below %LOCALAPPDATA%: the LithTech engine fails to load
    its archive configuration there and aborts with "Failed to initialize
    client - unable to load game resources". Measured on 2026-07-25 with a
    byte-identical archcfg in different locations; only the location makes
    the difference. The script therefore rejects such targets.

    Example: -InstallDir "D:\Games\FearVR"

.PARAMETER RetailRoot
    F.E.A.R. installation folder, i.e. the folder that contains FEAR.exe.
    Detected automatically when omitted.

    Example: -RetailRoot "C:\Program Files (x86)\Steam\steamapps\common\FEAR Ultimate Shooter Edition"

.PARAMETER PublicToolsGame
    Public Tools 1.08 folder. Either the installation root or its
    Dev\Runtime\Game subfolder. Detected automatically when omitted and
    verified against the hash of the stock GameClient.dll.

    Example: -PublicToolsGame "C:\Program Files (x86)\Monolith Productions\FEAR Public Tools"

.PARAMETER LaunchMode
    How the game is started later on.

    auto    (default) steam for a copy under steamapps\common, otherwise
            direct
    steam   through steam.exe -applaunch 21090
    direct  FEAR.exe is started directly, with the same arguments. This is
            the mode for GOG and retail-disc installations.

.PARAMETER NoShortcut
    Does not create a desktop shortcut.

.PARAMETER NonInteractive
    Never prompts. A path that cannot be detected becomes an error instead
    of a question — use this for unattended installs.

.PARAMETER Clean
    Only meaningful when updating: removes stale installer-owned modules and
    old logs after the replacement deployment commits. Userdata is kept.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\release\install.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\release\install.ps1 `
        -InstallDir "D:\Games\FearVR" `
        -RetailRoot "D:\SteamLibrary\steamapps\common\FEAR Ultimate Shooter Edition"
#>
[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:USERPROFILE 'FearVR'),

    [string]$RetailRoot,

    [string]$PublicToolsGame,

    [ValidateSet('auto', 'steam', 'direct')]
    [string]$LaunchMode = 'auto',

    [switch]$NoShortcut,

    [switch]$NonInteractive,

    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_fearvr-release.ps1"
$cfg = Get-FearVrReleaseConfig
$packageRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

$script:CanPrompt = -not $NonInteractive -and [Environment]::UserInteractive

# Asks for a path and keeps asking until the validator accepts it. Empty
# input aborts with the same guidance the non-interactive run would print,
# so nobody ends up in a loop they cannot leave.
function Request-Path {
    param(
        [Parameter(Mandatory)][string]$Title,
        [Parameter(Mandatory)][string]$Explanation,
        [Parameter(Mandatory)][string[]]$Examples,
        [Parameter(Mandatory)][scriptblock]$Validate,
        [Parameter(Mandatory)][string]$ParameterName,
        [string]$Hint
    )

    $guidance = @()
    $guidance += $Explanation
    if ($Hint) { $guidance += '', $Hint }
    $guidance += '', 'Examples:'
    foreach ($example in $Examples) { $guidance += "  $example" }
    $guidanceText = ($guidance -join [Environment]::NewLine)

    if (-not $script:CanPrompt) {
        throw ($guidanceText + [Environment]::NewLine + [Environment]::NewLine +
            "Re-run with -$ParameterName ""<path>"".")
    }

    Write-Host ''
    Write-Host "  $Title" -ForegroundColor Yellow
    foreach ($line in $guidance) { Write-Host "  $line" }
    Write-Host ''

    while ($true) {
        $answer = Read-Host '  Path (empty to abort)'
        if ([string]::IsNullOrWhiteSpace($answer)) {
            throw ($guidanceText + [Environment]::NewLine + [Environment]::NewLine +
                "Aborted. Re-run with -$ParameterName ""<path>"".")
        }
        # Paths pasted from Explorer or from a shell often carry quotes.
        $answer = $answer.Trim().Trim('"')
        $resolved = & $Validate $answer
        if ($resolved) { return $resolved }
        Write-Host '  That path does not work. Please try again.' -ForegroundColor Yellow
    }
}

# --- 0. Check the target folder ---------------------------------------------
# With the archive configuration below %LOCALAPPDATA%, the engine aborts on
# start with "unable to load game resources". Proven with a byte-identical
# archcfg in different locations: only the location decides.
# %LOCALAPPDATA%\Temp works, other subfolders do not.
$localAppData = [IO.Path]::GetFullPath($env:LOCALAPPDATA)
while ($true) {
    $installFull = [IO.Path]::GetFullPath($InstallDir)
    if (-not $installFull.StartsWith($localAppData, [StringComparison]::OrdinalIgnoreCase)) {
        break
    }
    $InstallDir = Request-Path `
        -Title 'Unsuitable target folder.' `
        -ParameterName 'InstallDir' `
        -Explanation @"
$installFull lies below %LOCALAPPDATA%. There the LithTech engine fails to
load its archive configuration ("Failed to initialize client - unable to
load game resources"). Please pick another location.
"@ `
        -Examples @(
            "$env:USERPROFILE\FearVR",
            'D:\Games\FearVR',
            'C:\FearVR'
        ) `
        -Validate {
            param($value)
            $full = [IO.Path]::GetFullPath($value)
            if ($full.StartsWith($localAppData, [StringComparison]::OrdinalIgnoreCase)) {
                return $null
            }
            return $full
        }
}
$InstallDir = [IO.Path]::GetFullPath($InstallDir)
Assert-FearVrSafeInstallDirectory `
    -InstallDir $InstallDir `
    -ProtectedRoots @($packageRoot) | Out-Null
$installMutex = Enter-FearVrInstallMutex $InstallDir
try {
Assert-FearVrRecognizedInstallDirectory $InstallDir

# --- Existing installation --------------------------------------------------
# Updating is the same command as installing. An earlier deployment.json
# supplies the paths that were used last time, so an update needs no arguments
# at all, and saved games under userdata are never touched.
$transactionPath = Get-FearVrProxyTransactionPath $InstallDir
if (Test-Path -LiteralPath $transactionPath -PathType Leaf) {
    $running = @(Get-Process -Name 'FEAR' -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) {
        throw ("F.E.A.R. is running (PID $($running.Id -join ', ')). " +
               'Close the game before recovering the interrupted update.')
    }
    $recovery = Resolve-FearVrProxyTransaction -InstallDir $InstallDir -Apply
    Write-Host "  [OK] Interrupted proxy transaction: $($recovery.Status)"
}

$previous = $null
$previousPath = Join-Path $InstallDir 'deployment.json'
if (Test-Path -LiteralPath $previousPath -PathType Leaf) {
    $previous = Read-FearVrOwnedDeployment `
        $InstallDir -AllowLegacyProxyless
}

Write-Host (
    '=== F.E.A.R. VR - ' +
    $(if ($previous) { 'Update' } else { 'Installation' }) +
    ' ===') -ForegroundColor Cyan

if ($previous) {
    Write-Host ("  ..   Existing installation found in $InstallDir " +
        "(package $($previous.packageVersion))")
    # The running game holds the staged modules open; replacing them would
    # fail halfway through and leave a mixed module set behind.
    $running = @(Get-Process -Name 'FEAR' -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) {
        throw ("F.E.A.R. is running (PID $($running.Id -join ', ')). " +
               'Close the game, then run this again.')
    }
    Assert-FearVrSafeInstallDirectory `
        -InstallDir $InstallDir `
        -ProtectedRoots @(
            [string]$previous.retailRoot,
            [string]$previous.publicToolsGame,
            $packageRoot) | Out-Null
    if ($previous.PSObject.Properties.Name -contains
        'pendingD3d9ProxyCleanup') {
        $cleanup = Invoke-FearVrPendingProxyCleanup `
            -DeploymentPath $previousPath `
            -Deployment $previous `
            -Apply
        Write-Host (
            "  [OK] Previous proxy cleanup: $($cleanup.ProxyStatus)")
        $previous = Read-FearVrOwnedDeployment `
            $InstallDir -AllowLegacyProxyless
    }
    if ([string]::IsNullOrWhiteSpace($RetailRoot) -and $previous.retailRoot) {
        $RetailRoot = $previous.retailRoot
    }
    if ([string]::IsNullOrWhiteSpace($PublicToolsGame) -and
        $previous.publicToolsGame) {
        $PublicToolsGame = $previous.publicToolsGame
    }
    if ($LaunchMode -eq 'auto' -and $previous.launchMode) {
        $LaunchMode = $previous.launchMode
    }
}

# --- 1. Locate and verify the retail installation ---------------------------
if ([string]::IsNullOrWhiteSpace($RetailRoot)) {
    Write-Host '  ..   Looking for the F.E.A.R. installation'
    $RetailRoot = Find-RetailRoot
}
if ([string]::IsNullOrWhiteSpace($RetailRoot) -or
    -not (Test-Path -LiteralPath (Join-Path $RetailRoot 'FEAR.exe') -PathType Leaf)) {
    $RetailRoot = Request-Path `
        -Title 'F.E.A.R. installation not found.' `
        -ParameterName 'RetailRoot' `
        -Explanation @'
Please enter the folder that contains FEAR.exe. The mod requires F.E.A.R.
1.08; older versions are rejected because the Public Tools modules do not
match them. Steam, GOG and the retail disc all work.
'@ `
        -Examples @(
            'C:\Program Files (x86)\Steam\steamapps\common\FEAR Ultimate Shooter Edition',
            'D:\SteamLibrary\steamapps\common\FEAR Ultimate Shooter Edition',
            'C:\GOG Games\FEAR',
            'C:\Program Files (x86)\Sierra\FEAR'
        ) `
        -Validate {
            param($value)
            if (Test-Path -LiteralPath (Join-Path $value 'FEAR.exe') -PathType Leaf) {
                return [IO.Path]::GetFullPath($value)
            }
            return $null
        }
}
$retail = Assert-RetailFearExe $RetailRoot
Write-Host "  [OK] F.E.A.R. $($retail.Version) - $($retail.Edition)"
Write-Host "       $RetailRoot"
if (-not $retail.Verified) {
    # This is not fatal: version-dependent signatures live in the Public
    # Tools GameOrig.dll, not in FEAR.exe.
    Write-Host @"
  [!]  This FEAR.exe build has not been tested with the mod.
       Only the Steam Ultimate Shooter Edition 1.08 is confirmed; other 1.08
       builds (GOG, retail disc) are expected to work but are unverified.
       Please report success or failure so the build can be listed.
       SHA-256: $($retail.Sha256)
"@ -ForegroundColor Yellow
}

# Steam refuses to hand FEAR.exe its command line without the client running,
# so a Steam copy is launched through steam.exe -applaunch. Any other copy is
# started directly, with the same arguments.
$launchMode = if ($LaunchMode -eq 'auto') {
    Get-RetailLaunchMode $RetailRoot
} else {
    $LaunchMode
}
if ($launchMode -eq 'steam' -and -not (Get-SteamExecutable)) {
    throw @'
Launch mode "steam" was requested, but steam.exe was not found. Install
Steam, or use -LaunchMode direct to start FEAR.exe directly (GOG, retail
disc).
'@
}
Write-Host "  [OK] Launch mode: $launchMode"

$retailArchCfg = Join-Path $RetailRoot 'Default.archcfg'
if (-not (Test-Path -LiteralPath $retailArchCfg -PathType Leaf)) {
    throw "Retail archive configuration is missing: $retailArchCfg"
}

# --- 2. Locate and verify the Public Tools ----------------------------------
if ([string]::IsNullOrWhiteSpace($PublicToolsGame)) {
    Write-Host '  ..   Looking for the Public Tools 1.08 installation'
    $PublicToolsGame = Find-PublicToolsGame
} else {
    $PublicToolsGame = Resolve-PublicToolsGame $PublicToolsGame
}
if (-not (Test-PublicToolsGame $PublicToolsGame)) {
    $PublicToolsGame = Request-Path `
        -Title 'Public Tools 1.08 not found.' `
        -ParameterName 'PublicToolsGame' `
        -Explanation @'
The five modules GameClient.dll, GameServer.dll, ClientFx.fxd, FEAR.dep and
FEARMod.Arch00s are proprietary and must not ship with this package. They
come from the official installer "fear_publictools_108.exe", which is part
of the Ultimate Shooter Edition under extras\.

Please enter either the Public Tools installation folder or its
Dev\Runtime\Game subfolder.
'@ `
        -Hint @'
Note on installing the Public Tools: the installer reads
HKLM\SOFTWARE\WOW6432Node\Monolith Productions\FEAR\1.00.0000\Patch and
expects the value 8, while Steam sets 10. Set it to 8 for the installation
and back to 10 afterwards.
'@ `
        -Examples @(
            'C:\Program Files (x86)\Monolith Productions\FEAR Public Tools',
            'C:\Program Files (x86)\Monolith Productions\FEAR Public Tools\Dev\Runtime\Game',
            'D:\FEAR Public Tools'
        ) `
        -Validate {
            param($value)
            return (Resolve-PublicToolsGame $value)
        }
}
Write-Host '  [OK] Public Tools 1.08'
Write-Host "       $PublicToolsGame"

# --- 3. Check our own modules in the package --------------------------------
$manifestPath = Join-Path $packageRoot 'release-manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Package manifest is missing: $manifestPath"
}
$package = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
foreach ($entry in $package.files) {
    $path = Join-Path $packageRoot $entry.path
    $actual = Get-FileSha256 $path
    if ($actual -ne $entry.sha256) {
        throw "Package file is missing or was modified: $($entry.path)"
    }
}
Write-Host "  [OK] Package $($package.version) ($($package.gitCommit)) unmodified"

# --- 4. Build the stage -----------------------------------------------------
foreach ($target in $cfg.PublicToolsModules.Keys) {
    $source = Join-Path $PublicToolsGame $cfg.PublicToolsModules[$target]
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Public Tools module is missing: $source"
    }
}
foreach ($asset in @(
    'chars\skins\player_new_d.dds',
    'chars\models\player.Model00p'
)) {
    $source = Join-Path $PublicToolsGame $asset
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Public Tools body source is missing: $source"
    }
}
$RetailRoot = [IO.Path]::GetFullPath($RetailRoot)
$PublicToolsGame = [IO.Path]::GetFullPath($PublicToolsGame)
Assert-FearVrSafeInstallDirectory `
    -InstallDir $InstallDir `
    -ProtectedRoots @($packageRoot, $RetailRoot, $PublicToolsGame) | Out-Null

# Refuse a foreign wrapper before any existing stage content changes. The
# completed stage and durable transaction journal are still prepared later.
$proxySource = Join-Path $packageRoot $cfg.BundledModules['fearvr-d3d9.dll']
$previousProxy = if ($previous -and
    $previous.PSObject.Properties.Name -contains 'd3d9Proxy') {
    $previous.d3d9Proxy
} else {
    $null
}
$proxyPlan = Get-FearVrAppLocalProxyInstallPlan `
    -SourcePath $proxySource `
    -RetailRoot $RetailRoot `
    -PreviousRecord $previousProxy
$proxyRecord = $proxyPlan.Record
$cleanPreviousStage = [bool]($previous -and $Clean)

$stageTransactionPath = Get-FearVrStageTransactionPath $InstallDir
$stageTransaction = Read-FearVrStageTransaction $InstallDir
if ($null -eq $stageTransaction) {
    $stageTransaction = [pscustomobject][ordered]@{
        schemaVersion = 1
        transactionId = [Guid]::NewGuid().ToString('N')
        installDir = $InstallDir
        physicalInstallDir = Get-FearVrPhysicalPath $InstallDir
        packageVersion = [string]$package.version
    }
    Write-FearVrJsonAtomically $stageTransactionPath $stageTransaction 4
    $stageTransaction = Read-FearVrStageTransaction $InstallDir
    if ($null -eq $stageTransaction) {
        throw 'The durable prepared-stage marker could not be verified.'
    }
} else {
    Write-Host '  [OK] Resuming a recorded interrupted stage attempt'
}

$moduleDirectory = Join-Path $InstallDir 'game-modules'
$userDirectory = Join-Path $InstallDir 'userdata'
$logDirectory = Join-Path $InstallDir 'logs'
$archiveConfig = Join-Path $InstallDir 'fearvr.archcfg'
$installPhysical = (Get-FearVrPhysicalPath $InstallDir).TrimEnd('\')
foreach ($stageDirectory in @(
    $moduleDirectory, $userDirectory, $logDirectory)) {
    $stagePhysical = (Get-FearVrPhysicalPath $stageDirectory).TrimEnd('\')
    if (-not $stagePhysical.StartsWith(
            $installPhysical + '\',
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "A stage directory escapes InstallDir: $stageDirectory"
    }
    if ((Test-Path -LiteralPath $stageDirectory -PathType Container) -and
        (Test-FearVrTreeContainsReparsePoint $stageDirectory)) {
        throw "A stage directory contains a reparse point: $stageDirectory"
    }
}
if (Test-Path -LiteralPath $archiveConfig) {
    $archiveItem = Get-Item -LiteralPath $archiveConfig -Force
    if (($archiveItem.Attributes -band
            [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "The archive configuration is a reparse point: $archiveConfig"
    }
}
foreach ($directory in @($InstallDir, $moduleDirectory, $userDirectory, $logDirectory)) {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}

$staged = [ordered]@{}
foreach ($target in $cfg.PublicToolsModules.Keys) {
    $source = Join-Path $PublicToolsGame $cfg.PublicToolsModules[$target]
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Public Tools module is missing: $source"
    }
    Copy-Item -LiteralPath $source -Destination (Join-Path $moduleDirectory $target) -Force
    $staged[$target] = 'public-tools'
}
foreach ($target in $cfg.BundledModules.Keys) {
    $source = Join-Path $packageRoot $cfg.BundledModules[$target]
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Package module is missing: $source"
    }
    Copy-Item -LiteralPath $source -Destination (Join-Path $moduleDirectory $target) -Force
    $staged[$target] = 'fearvr'
}

# The derived texture is generated here from the owner's Public Tools
# installation and therefore is not part of the release package.
& (Join-Path $PSScriptRoot 'new-body-assets.ps1') `
    -SourceGame $PublicToolsGame `
    -DestinationGame $moduleDirectory
$generatedFiles = foreach ($relativePath in @(
    'fearvr\player_body_d.dds',
    'fearvr\player_body.Mat00')) {
    $generatedPath = Join-Path $moduleDirectory $relativePath
    [ordered]@{
        relativePath = $relativePath
        sha256 = Get-FileSha256 $generatedPath
        bytes = (Get-Item -LiteralPath $generatedPath).Length
    }
}

# Only a prior manifest can prove that a no-longer-used module is ours. A
# changed or unrecorded file is preserved.
$staleModuleRecords = @(
    if ($previous) {
        foreach ($oldRecord in @($previous.files)) {
            $oldName = [string]$oldRecord.name
            if ($staged.Contains($oldName)) { continue }
            $oldPath = Join-Path $moduleDirectory $oldName
            $oldHash = [string]$oldRecord.sha256
            if ((Get-FileSha256 $oldPath) -eq $oldHash) {
                [pscustomobject]@{ Path = $oldPath; Sha256 = $oldHash }
            } elseif (Test-Path -LiteralPath $oldPath) {
                Write-Host (
                    "  [!] Kept modified stale module $oldName") `
                    -ForegroundColor Yellow
            }
        }
    }
)
Write-Host "  [OK] $($staged.Count) modules in $moduleDirectory"

# --- 5. Archive configuration -----------------------------------------------
# The loose archcfg layer is the official way to load a module set of our
# own without replacing Retail's executable or game data.
$archiveLines = @(
    Get-Content -LiteralPath $retailArchCfg -Encoding Default |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
)
$archiveLines += $moduleDirectory
[IO.File]::WriteAllLines($archiveConfig, $archiveLines, [Text.Encoding]::ASCII)

# --- 6. Validate the completed stage ----------------------------------------
$records = foreach ($name in $staged.Keys) {
    $path = Join-Path $moduleDirectory $name
    [ordered]@{
        name = $name
        origin = $staged[$name]
        sha256 = Get-FileSha256 $path
        bytes = (Get-Item -LiteralPath $path).Length
    }
}

$retailAfter = Assert-RetailFearExe $RetailRoot
if ($retail.Sha256 -ne $retailAfter.Sha256) {
    throw 'SAFETY ABORT: the retail FEAR.exe was modified.'
}

# --- 7. Transactional app-local Direct3D proxy ------------------------------
# Every fallible package and stage operation above finishes before the retail
# proxy changes. The journal, rollback copies, and complete next manifest are
# durable before the proxy is replaced.
$running = @(Get-Process -Name 'FEAR' -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    throw ("F.E.A.R. is running (PID $($running.Id -join ', ')). " +
           'Close the game before installing the app-local proxy.')
}
$pendingCleanup = $null
if ($previous -and $previousProxy -and $previous.retailRoot) {
    $oldTarget = [IO.Path]::GetFullPath(
        (Join-Path ([string]$previous.retailRoot) 'd3d9.dll'))
    if (-not (Test-FearVrSamePhysicalPath `
            $oldTarget $proxyPlan.TargetPath)) {
        $pendingCleanup = [ordered]@{
            retailRoot = [IO.Path]::GetFullPath(
                [string]$previous.retailRoot)
            physicalRetailRoot = Get-FearVrPhysicalPath `
                ([string]$previous.retailRoot)
            record = $previousProxy
        }
    }
}

$transactionId = [Guid]::NewGuid().ToString('N')
$deployment = Join-Path $InstallDir 'deployment.json'
$deploymentData = [ordered]@{
    installedUtc = (Get-Date).ToUniversalTime().ToString('s') + 'Z'
    packageVersion = $package.version
    packageGitCommit = $package.gitCommit
    packageRoot = $packageRoot
    installDir = $InstallDir
    retailRoot = $RetailRoot
    runtimeExe = $retail.Path
    runtimeSha256 = $retail.Sha256
    runtimeVerified = $retail.Verified
    runtimeEdition = $retail.Edition
    launchMode = $launchMode
    steamAppId = $cfg.SteamAppId
    publicToolsGame = $PublicToolsGame
    moduleDirectory = $moduleDirectory
    archiveConfig = $archiveConfig
    archiveConfigSha256 = Get-FileSha256 $archiveConfig
    userDirectory = $userDirectory
    logDirectory = $logDirectory
    files = @($records)
    generatedFiles = @($generatedFiles)
    d3d9Proxy = $proxyRecord
    d3d9ExCompatibility = $true
    proxyTransactionId = $transactionId
}
if ($pendingCleanup) {
    $deploymentData['pendingD3d9ProxyCleanup'] = $pendingCleanup
}

$journal = [pscustomobject][ordered]@{
    schemaVersion = 1
    transactionId = $transactionId
    phase = 'Prepared'
    installDir = $InstallDir
    target = [ordered]@{
        retailRoot = $proxyPlan.RetailRoot
        physicalRetailRoot = $proxyPlan.PhysicalRetailRoot
        path = $proxyPlan.TargetPath
        wasPresent = $proxyPlan.TargetWasPresent
        priorSha256 = $proxyPlan.PriorSha256
        installedSha256 = $proxyPlan.SourceSha256
        backupPath = Join-Path $InstallDir `
            ".fearvr-d3d9-$transactionId.rollback"
        temporaryPath = Join-Path $proxyPlan.RetailRoot `
            ".fearvr-d3d9-$transactionId.tmp"
    }
    manifest = [ordered]@{
        path = $deployment
        previousExisted = Test-Path -LiteralPath $deployment -PathType Leaf
        previousSha256 = Get-FileSha256 $deployment
        previousBackupPath = Join-Path $InstallDir `
            ".fearvr-deployment-$transactionId.rollback.json"
        newPath = Join-Path $InstallDir `
            ".fearvr-deployment-$transactionId.new.json"
        temporaryPath = Join-Path $InstallDir `
            ".fearvr-deployment-$transactionId.tmp"
    }
    newProxyRecord = $proxyRecord
}
$transactionPath = Get-FearVrProxyTransactionPath $InstallDir
try {
    Write-FearVrJsonAtomically $transactionPath $journal 8
    $journal = Read-FearVrProxyTransaction $InstallDir
    if ($null -eq $journal) {
        throw 'The durable proxy transaction journal could not be verified.'
    }
} catch {
    Remove-Item -LiteralPath $transactionPath -Force `
        -ErrorAction SilentlyContinue
    throw
}

try {
    if ([bool]$journal.manifest.previousExisted) {
        Copy-FearVrFileDurably `
            -SourcePath $deployment `
            -DestinationPath ([string]$journal.manifest.previousBackupPath)
        if ((Get-FileSha256 $journal.manifest.previousBackupPath) -ne
            [string]$journal.manifest.previousSha256) {
            throw 'The previous deployment manifest backup failed verification.'
        }
    }
    Write-FearVrJsonFileDurably `
        -Path ([string]$journal.manifest.newPath) `
        -Value $deploymentData `
        -Depth 8
    $preparedDeployment =
        Get-Content -Raw -LiteralPath $journal.manifest.newPath |
            ConvertFrom-Json
    if (-not (Test-FearVrProxyTransactionCommitted `
            $journal $preparedDeployment)) {
        throw 'The staged deployment manifest failed transaction validation.'
    }

    if ([bool]$journal.target.wasPresent) {
        if ((Get-FileSha256 $journal.target.path) -ne
            [string]$journal.target.priorSha256) {
            throw 'The app-local proxy changed before its rollback copy was made.'
        }
        Copy-FearVrFileDurably `
            -SourcePath ([string]$journal.target.path) `
            -DestinationPath ([string]$journal.target.backupPath)
        if ((Get-FileSha256 $journal.target.backupPath) -ne
            [string]$journal.target.priorSha256) {
            throw 'The previous app-local proxy backup failed verification.'
        }
    }

    Install-FearVrAppLocalProxy `
        -Plan $proxyPlan `
        -TemporaryPath ([string]$journal.target.temporaryPath) | Out-Null
    Set-FearVrFileAtomically `
        -SourcePath ([string]$journal.manifest.newPath) `
        -TargetPath $deployment `
        -TemporaryPath ([string]$journal.manifest.temporaryPath)

    $committedDeployment = Get-Content -Raw -LiteralPath $deployment |
        ConvertFrom-Json
    if (-not (Test-FearVrProxyTransactionCommitted `
            $journal $committedDeployment)) {
        throw 'The deployment manifest did not commit the proxy transaction.'
    }
    $journal.phase = 'ManifestCommitted'
    Write-FearVrJsonAtomically $transactionPath $journal 8
    $journal = Read-FearVrProxyTransaction $InstallDir
    if ([string]$journal.phase -ne 'ManifestCommitted') {
        throw 'The committed proxy transaction journal could not be verified.'
    }

    if ($committedDeployment.PSObject.Properties.Name -contains
        'pendingD3d9ProxyCleanup') {
        $oldCleanup = Invoke-FearVrPendingProxyCleanup `
            -DeploymentPath $deployment `
            -Deployment $committedDeployment `
            -Apply
        if ($oldCleanup.ProxyStatus -eq 'Removed') {
            Write-Host "  [OK] Removed previous proxy: $($oldCleanup.Path)"
        } elseif ($oldCleanup.ProxyStatus -in @(
                'Modified', 'InvalidRecord', 'NoRecord')) {
            Write-Host (
                '  [!] Previous app-local d3d9.dll was not owned and was kept: ' +
                $oldCleanup.Path) -ForegroundColor Yellow
        } elseif ($oldCleanup.ProxyStatus -in @(
                'PhysicalIdentityChanged',
                'PhysicalIdentityUnverified')) {
            Write-Host (
                '  [!] Previous app-local d3d9.dll identity could not be ' +
                'proven and was kept: ' + $oldCleanup.Path) `
                -ForegroundColor Yellow
        }
    }
    Remove-FearVrProxyTransactionArtifacts $InstallDir $journal
} catch {
    $transactionFailure = $_
    try {
        $recovery = Resolve-FearVrProxyTransaction `
            -InstallDir $InstallDir `
            -Apply
        Write-Host "  [!] Proxy transaction recovery: $($recovery.Status)" `
            -ForegroundColor Yellow
    } catch {
        throw (
            "Proxy transaction failed and automatic recovery also failed. " +
            "Keep '$transactionPath' for the next install or uninstall. " +
            "Original error: $($transactionFailure.Exception.Message) " +
            "Recovery error: $($_.Exception.Message)")
    }
    throw $transactionFailure
}

# Destructive stage cleanup is intentionally post-commit. Proxy preflight runs
# before staging, and stale files survive any interrupted proxy transaction.
# Reparse points are preserved instead of followed.
foreach ($staleRecord in $staleModuleRecords) {
    try {
        $staleStatus = Remove-FearVrFileByOwnedHash `
            -Path $staleRecord.Path `
            -ExpectedSha256 $staleRecord.Sha256
        if ($staleStatus -eq 'Removed') {
            Write-Host (
                '  [OK] Removed stale module ' +
                [IO.Path]::GetFileName($staleRecord.Path))
        } elseif ($staleStatus -notin @('Missing')) {
            Write-Host (
                "  [!] Kept stale module ($staleStatus): " +
                $staleRecord.Path) -ForegroundColor Yellow
        }
    } catch {
        Write-Host (
            "  [!] Kept unsafe stale module path: $($staleRecord.Path) " +
            "($($_.Exception.Message))") -ForegroundColor Yellow
    }
}
if ($cleanPreviousStage) {
    Write-Host (
        '  [OK] Clean mode used record-scoped post-commit cleanup; ' +
        'userdata, logs, unknown files, and reparse points were kept')
}
Write-Host "  [OK] App-local Direct3D proxy: $($proxyRecord.path)"

# --- 8. Shortcut ------------------------------------------------------------
$playScript = Join-Path $PSScriptRoot 'play.ps1'
if (-not $NoShortcut) {
    $shortcut = Join-Path ([Environment]::GetFolderPath('Desktop')) 'F.E.A.R. VR.lnk'
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut($shortcut)
    $link.TargetPath =
        'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe'
    $link.Arguments =
        "-NoProfile -ExecutionPolicy Bypass -File `"$playScript`" " +
        "-InstallDir `"$InstallDir`""
    $link.WorkingDirectory = $packageRoot
    $link.IconLocation = "$($retail.Path),0"
    $link.Description = 'Start F.E.A.R. VR'
    $link.Save()
    Write-Host "  [OK] Shortcut: $shortcut"
}

$retailFinal = Assert-RetailFearExe $RetailRoot
if ($retail.Sha256 -ne $retailFinal.Sha256) {
    throw 'SAFETY ABORT: the retail FEAR.exe was modified.'
}

Write-Host ''
if ($previous) {
    Write-Host ("Update complete ($($previous.packageVersion) -> " +
        "$($package.version)); FEAR.exe unchanged.") -ForegroundColor Green
    Write-Host 'Saved games and profiles were kept.'
} else {
    Write-Host 'Installation complete; FEAR.exe unchanged.' -ForegroundColor Green
}
Write-Host 'The reversible app-local d3d9.dll proxy is hash-tracked.'
Write-Host "Install folder: $InstallDir"
Write-Host ''
Write-Host 'Play:'
Write-Host "  desktop shortcut 'F.E.A.R. VR'"
Write-Host "  or: powershell -ExecutionPolicy Bypass -File `"$playScript`""
Write-Host ''
Write-Host 'Uninstall:'
Write-Host ("  powershell -ExecutionPolicy Bypass -File " +
            "`"$(Join-Path $PSScriptRoot 'uninstall.ps1')`" -Apply")
} finally {
    Exit-FearVrInstallMutex $installMutex
}
