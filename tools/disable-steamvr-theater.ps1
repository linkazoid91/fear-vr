<#
.SYNOPSIS
    Deaktiviert SteamVRs automatischen Theatermodus für Desktopspiele.

.DESCRIPTION
    Setzt ausschließlich steamvr.autoShowGameTheater in der persönlichen
    SteamVR-Konfiguration auf false. Vor der ersten Änderung wird eine
    datierte Sicherung unter dem Projekt-Logverzeichnis angelegt.
#>
[CmdletBinding()]
param(
    [string]$SettingsPath =
        'C:\Program Files (x86)\Steam\config\steamvr.vrsettings',

    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_fearvr-env.ps1"

if (-not (Test-Path -LiteralPath $SettingsPath -PathType Leaf)) {
    throw "SteamVR-Konfiguration fehlt: $SettingsPath"
}

$text = [IO.File]::ReadAllText($SettingsPath)
$sectionPattern =
    '(?ms)(^\s*"steamvr"\s*:\s*\{\s*\r?\n)(.*?)(^\s*\})'
$sectionMatch = [Text.RegularExpressions.Regex]::Match(
    $text,
    $sectionPattern
)
if (-not $sectionMatch.Success) {
    throw 'Abschnitt "steamvr" fehlt in steamvr.vrsettings.'
}

$sectionText = $sectionMatch.Value
$valuePattern =
    '(?m)("autoShowGameTheater"\s*:\s*)(true|false)'
$changed = $false

if ([Text.RegularExpressions.Regex]::IsMatch($sectionText, $valuePattern)) {
    $updatedSection = [Text.RegularExpressions.Regex]::Replace(
        $sectionText,
        $valuePattern,
        '${1}false'
    )
    $changed = $updatedSection -cne $sectionText
} else {
    $updatedSection = [Text.RegularExpressions.Regex]::Replace(
        $sectionText,
        '(?m)(^\s*"steamvr"\s*:\s*\{\s*\r?\n)',
        '${1}      "autoShowGameTheater" : false,' + [Environment]::NewLine,
        1
    )
    $changed = $true
}

if ($changed) {
    $updated =
        $text.Substring(0, $sectionMatch.Index) +
        $updatedSection +
        $text.Substring($sectionMatch.Index + $sectionMatch.Length)

    # Der Sicherungsort kommt aus dem eigenen Skriptort, nicht aus einer
    # Projektkonfiguration.
    #
    # Vorher stand hier `$cfg.ProjectRoot`. Im Repo gibt es das; im
    # ausgelieferten Paket bringt `_fearvr-release.ps1` aber gar keine
    # Projektwurzel mit, `$cfg.ProjectRoot` war dort also $null und
    # `Join-Path` brach mit "Cannot bind argument to parameter 'Path'" ab.
    # Aufgefallen erst bei einem Nutzer, weil dieser Zweig nur laeuft, wenn
    # der Theatermodus tatsaechlich noch an ist — wer ihn ohnehin aus hatte,
    # kam nie hierher. `..\logs` trifft in beiden Faellen: im Repo die
    # Projektwurzel, im Paket dessen eigenes Verzeichnis.
    $backupDirectory = [IO.Path]::GetFullPath(
        (Join-Path $PSScriptRoot '..\logs\steamvr-settings-backups')
    )
    # Die Wurzelpruefung aus §12 gilt nur im Repo; im Paket gibt es sie nicht.
    if (Get-Command Assert-UnderProjectRoot -ErrorAction SilentlyContinue) {
        $backupDirectory = Assert-UnderProjectRoot $backupDirectory
    }
    New-Item -ItemType Directory -Force -Path $backupDirectory |
        Out-Null
    $backupPath = Join-Path $backupDirectory (
        'steamvr-' +
        (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss') +
        '.vrsettings'
    )
    Copy-Item -LiteralPath $SettingsPath -Destination $backupPath
    $utf8WithoutBom = New-Object Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($SettingsPath, $updated, $utf8WithoutBom)
}

$verified =
    [IO.File]::ReadAllText($SettingsPath) |
    ConvertFrom-Json
if ($verified.steamvr.autoShowGameTheater -ne $false) {
    throw 'SteamVR-Theatermodus konnte nicht deaktiviert werden.'
}

if (-not $Quiet) {
    Write-Host 'SteamVR: automatischer Theatermodus ist deaktiviert.'
}
