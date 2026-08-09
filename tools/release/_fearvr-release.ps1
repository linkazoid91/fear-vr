# =============================================================================
# Gemeinsame Definitionen für das ausgelieferte F.E.A.R.-VR-Paket.
# Absichtlich unabhängig vom Entwicklungs-Repository: Das Paket bringt nur
# eigene Binaries mit und holt die Public-Tools-Module lokal beim Nutzer.
# Enthält KEINE ausführbare Logik außer Definitionen.
# =============================================================================

$script:FearVrRelease = [ordered]@{
    # Retail-Zielversion. Versionsabhängige Hooks bleiben bei Abweichung aus.
    ExpectedVersion = '1.08.282.0'
    ExpectedSha256  = 'D5EBC38A4F12B772C9112A2811C290ADB6C5052D3BC2F817302D38CF55BB2CBE'

    # Bekannte, tatsächlich getestete FEAR.exe-Builds. Jede andere 1.08-EXE
    # (GOG, Retail-DVD) kann laufen, ist hier aber nicht bestätigt: Alle
    # versionsabhängigen Signaturen dieses Mods liegen in GameOrig.dll aus den
    # Public Tools, nicht in FEAR.exe. Deshalb wird ein unbekannter Hash nur
    # noch gemeldet und nicht mehr als Abbruchgrund behandelt.
    KnownRetailHashes = [ordered]@{
        'D5EBC38A4F12B772C9112A2811C290ADB6C5052D3BC2F817302D38CF55BB2CBE' =
            'Steam, Ultimate Shooter Edition 1.08'
        'D662DCCDB2EBD17D1ACED7C725A8724060010718146E0C0074DA5E8EF89B82B4' =
            'Steam 1.08 + HDTextures4FEAR/XP v2.0.2'
    }

    # Unverändertes VC7.1-GameClient.dll aus den Public Tools 1.08. Dient als
    # Erkennungsmerkmal für ein gültiges Public-Tools-Runtime-Verzeichnis.
    PublicToolsGameClientSha256 =
        'B5F1F1976227FD0E6F1C32BD2BEEDFB117E68A87A07BB42D06BE489DD08A63BA'

    SteamAppId = 21090

    # Module, die aus der lokalen Public-Tools-Installation geholt werden.
    # Schlüssel = Zielname in der Stage, Wert = Quellname im Runtime\Game.
    PublicToolsModules = [ordered]@{
        'GameOrig.dll'    = 'GameClient.dll'
        'GameServer.dll'  = 'GameServer.dll'
        'ClientFx.fxd'    = 'ClientFx.fxd'
        'FEAR.dep'        = 'FEAR.dep'
        'FEARMod.Arch00s' = 'FEARMod.Arch00s'
    }

    # Eigene Module, die im Overlay bereits am endgültigen Ort liegen.
    BundledModules = [ordered]@{
        'GameClient.dll'  = 'game-modules\GameClient.dll'
        'fearvr-d3d9.dll' = 'game-modules\fearvr-d3d9.dll'
    }

    VdxrManifest =
        'C:\Program Files\Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr.json'
}

function Get-FearVrReleaseConfig { return $script:FearVrRelease }

function Get-FileSha256([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    $stream = [IO.File]::OpenRead([IO.Path]::GetFullPath($Path))
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        return [BitConverter]::ToString(
            $sha256.ComputeHash($stream)).Replace('-', '')
    } finally {
        $sha256.Dispose()
        $stream.Dispose()
    }
}

# The native bridge logs hexadecimal session IDs without leading zeroes while
# the PowerShell launcher formats them as fixed-width 64-bit values. Compare
# the numeric value so 0x08DE... and 0x8DE... identify the same launch.
function Test-FearVrProxySessionHeader(
    [string]$Header,
    [uint64]$ExpectedSessionId
) {
    if ([string]::IsNullOrWhiteSpace($Header)) { return $false }

    try {
        $record = $Header | ConvertFrom-Json
        $message = [string]$record.message
        $match = [regex]::Match(
            $message, '(?:^|\s)session=0x([0-9A-Fa-f]+)(?:\s|$)')
        if (-not $match.Success) { return $false }

        $actualSessionId =
            [Convert]::ToUInt64($match.Groups[1].Value, 16)
        return $actualSessionId -eq $ExpectedSessionId
    } catch {
        return $false
    }
}

# Verifiziert eine Retail-FEAR.exe. Die Version 1.08 ist Bedingung — die
# Public-Tools-Module passen zu keiner anderen. Der Hash entscheidet dagegen
# nur noch darüber, ob dieser Build getestet ist: Steam, GOG und die
# Retail-DVD liefern verschiedene 1.08-EXEn, während jede Bytesignatur dieses
# Mods in GameOrig.dll steht.
function Assert-RetailFearExe([string]$RetailRoot) {
    $cfg = Get-FearVrReleaseConfig
    $exe = Join-Path $RetailRoot 'FEAR.exe'
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
        throw "FEAR.exe not found: $exe"
    }
    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($exe).FileVersion
    if ($version -notlike '1.08*') {
        throw ("Wrong FEAR.exe version: '$version' (1.08 required). " +
               "Path: $exe")
    }
    $sha = Get-FileSha256 $exe
    $known = $cfg.KnownRetailHashes[$sha]
    return [pscustomobject]@{
        Path = $exe
        Version = $version
        Sha256 = $sha
        Verified = [bool]$known
        Edition = if ($known) { $known } else { "unknown 1.08 build ($version)" }
    }
}

# Ein nachtraegliches Installieren oder Entfernen des bestaetigten
# HDTextures4FEAR-Patches darf eine vorhandene VR-Installation weiter nutzen.
# Unbekannte Hashwechsel bleiben dagegen ein Integritaetsfehler.
function Test-CompatibleRetailFearHashes(
    [string]$RecordedHash,
    [string]$CurrentHash
) {
    if ($RecordedHash -eq $CurrentHash) { return $true }
    $cfg = Get-FearVrReleaseConfig
    return (
        [bool]$cfg.KnownRetailHashes[$RecordedHash] -and
        [bool]$cfg.KnownRetailHashes[$CurrentHash]
    )
}

# Alle festen lokalen Laufwerke, damit ein Spiel auf D:\ oder E:\ genauso
# gefunden wird wie auf C:\. Netzlaufwerke bleiben draußen: Die Suche würde
# dort spürbar hängen.
function Get-LocalDriveRoots {
    $roots = New-Object Collections.Generic.List[string]
    try {
        foreach ($drive in [IO.DriveInfo]::GetDrives()) {
            if ($drive.DriveType -eq [IO.DriveType]::Fixed -and $drive.IsReady) {
                $roots.Add($drive.Name.TrimEnd('\'))
            }
        }
    } catch { }
    if ($roots.Count -eq 0) { $roots.Add('C:') }
    return $roots
}

# Normalisiert einen Windows-Pfad und fuegt ihn nur einmal hinzu. Steam legt
# Pfade je nach Registrywert bzw. VDF-Datei mit Vorwaerts- oder doppelten
# Rueckwaertsschraegstrichen ab.
function Add-UniqueFearVrPath(
    [Collections.Generic.List[string]]$List,
    [string]$Path
) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return }
    $normalised = $Path.Trim().Trim('"') -replace '/', '\'
    try { $normalised = [IO.Path]::GetFullPath($normalised) } catch { return }

    $pathRoot = [IO.Path]::GetPathRoot($normalised)
    if ($normalised.Length -gt $pathRoot.Length) {
        $normalised = $normalised.TrimEnd('\')
    }
    foreach ($existing in $List) {
        if ([string]::Equals(
                $existing, $normalised,
                [StringComparison]::OrdinalIgnoreCase)) {
            return
        }
    }
    $List.Add($normalised)
}

# Steam kann selbst auf einem anderen Laufwerk liegen. Die Registry ist die
# primaere Quelle; ein laufender Client und die gaengigen Ordner auf lokalen
# Laufwerken decken portable bzw. beschaedigte Registry-Installationen ab.
function Get-SteamInstallRoots {
    $roots = New-Object Collections.Generic.List[string]
    foreach ($key in @(
        'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam',
        'HKLM:\SOFTWARE\Valve\Steam',
        'HKCU:\SOFTWARE\Valve\Steam'
    )) {
        try { $value = Get-ItemProperty $key -ErrorAction Stop } catch { continue }
        foreach ($name in @('InstallPath', 'SteamPath')) {
            if ($value.PSObject.Properties.Name -contains $name -and
                $value.$name) {
                $registeredRoot = [string]$value.$name
                if ((Test-Path -LiteralPath (
                        Join-Path $registeredRoot 'steam.exe') -PathType Leaf) -or
                    (Test-Path -LiteralPath (
                        Join-Path $registeredRoot (
                            'steamapps\libraryfolders.vdf')) -PathType Leaf)) {
                    Add-UniqueFearVrPath $roots $registeredRoot
                }
            }
        }
        if ($value.PSObject.Properties.Name -contains 'SteamExe' -and
            $value.SteamExe) {
            try {
                $registeredExe = ([string]$value.SteamExe).Trim('"')
                if (Test-Path -LiteralPath $registeredExe -PathType Leaf) {
                    Add-UniqueFearVrPath $roots (
                        Split-Path -Parent $registeredExe)
                }
            } catch { }
        }
    }

    if ($roots.Count -eq 0) {
        try {
            foreach ($process in @(Get-Process -Name steam -ErrorAction Stop)) {
                if ($process.Path) {
                    Add-UniqueFearVrPath $roots (Split-Path -Parent $process.Path)
                }
            }
        } catch { }
    }

    if ($roots.Count -eq 0) {
        foreach ($drive in (Get-LocalDriveRoots)) {
            foreach ($relative in @(
                'Program Files (x86)\Steam',
                'Program Files\Steam',
                'Games\Steam',
                'Steam'
            )) {
                $candidate = [IO.Path]::Combine($drive + '\', $relative)
                if ((Test-Path -LiteralPath (Join-Path $candidate 'steam.exe') `
                        -PathType Leaf) -or
                    (Test-Path -LiteralPath (
                        Join-Path $candidate 'steamapps\libraryfolders.vdf') `
                        -PathType Leaf)) {
                    Add-UniqueFearVrPath $roots $candidate
                }
            }
        }
    }
    return $roots
}

function Get-SteamExecutable {
    foreach ($root in (Get-SteamInstallRoots)) {
        $candidate = Join-Path $root 'steam.exe'
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }
    return $null
}

# Liefert die Steam-Wurzel und jede zusaetzliche Bibliothek aus
# libraryfolders.vdf. Der optionale Parameter macht die Pfadsuche mit einer
# synthetischen Steam-Installation testbar.
function Get-SteamLibraryRoots([string[]]$SteamRoots) {
    if ($null -eq $SteamRoots -or $SteamRoots.Count -eq 0) {
        $SteamRoots = @(Get-SteamInstallRoots)
    }
    $libraries = New-Object Collections.Generic.List[string]
    foreach ($steamRoot in $SteamRoots) {
        Add-UniqueFearVrPath $libraries $steamRoot
        $vdf = Join-Path $steamRoot 'steamapps\libraryfolders.vdf'
        if (-not (Test-Path -LiteralPath $vdf -PathType Leaf)) { continue }
        try { $vdfText = [IO.File]::ReadAllText($vdf) } catch { continue }
        foreach ($match in [Text.RegularExpressions.Regex]::Matches(
            $vdfText, '"path"\s*"([^"]+)"')) {
            $path = $match.Groups[1].Value -replace '\\\\', '\'
            Add-UniqueFearVrPath $libraries $path
        }
    }
    return $libraries
}

# SteamVR ist App 250820. Das Appmanifest liefert den tatsaechlichen
# Installationsordner; "SteamVR" bleibt der Rueckfall fuer normale
# Installationen und fehlende/alte Manifeste.
function Get-SteamVrInstallRoots([string[]]$SteamRoots) {
    $installRoots = New-Object Collections.Generic.List[string]
    foreach ($library in @(Get-SteamLibraryRoots $SteamRoots)) {
        $folderNames = New-Object Collections.Generic.List[string]
        $appManifest = Join-Path $library 'steamapps\appmanifest_250820.acf'
        if (Test-Path -LiteralPath $appManifest -PathType Leaf) {
            try { $appText = [IO.File]::ReadAllText($appManifest) } catch {
                $appText = ''
            }
            $match = [Text.RegularExpressions.Regex]::Match(
                $appText, '"installdir"\s*"([^"]+)"')
            if ($match.Success) { $folderNames.Add($match.Groups[1].Value) }
        }
        if ('SteamVR' -notin $folderNames) { $folderNames.Add('SteamVR') }
        foreach ($folderName in $folderNames) {
            Add-UniqueFearVrPath $installRoots (
                Join-Path $library "steamapps\common\$folderName")
        }
    }
    return $installRoots
}

function Find-SteamVrManifest([string[]]$SteamRoots) {
    foreach ($installRoot in @(Get-SteamVrInstallRoots $SteamRoots)) {
        $manifest = Join-Path $installRoot 'steamxr_win64.json'
        if (Test-Path -LiteralPath $manifest -PathType Leaf) {
            return $manifest
        }
    }
    return $null
}

# Installationsorte aus der Uninstall-Registry. Deckt GOG, die alte
# Retail-DVD und jede Neuinstallation an einem ungewöhnlichen Ort ab, ohne
# das Dateisystem durchsuchen zu müssen.
function Get-RegistryInstallLocations([string]$NamePattern) {
    $found = New-Object Collections.Generic.List[string]
    $keys = @(
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games\*'
    )
    foreach ($key in $keys) {
        try { $entries = Get-ItemProperty $key -ErrorAction Stop } catch { continue }
        foreach ($entry in $entries) {
            $names = $entry.PSObject.Properties.Name
            $title = ''
            foreach ($field in @('DisplayName', 'gameName')) {
                if ($names -contains $field -and $entry.$field) {
                    $title = [string]$entry.$field
                    break
                }
            }
            if ($title -notlike $NamePattern) { continue }
            foreach ($field in @('InstallLocation', 'path', 'InstallPath')) {
                if ($names -contains $field -and $entry.$field) {
                    $found.Add(([string]$entry.$field).Trim('"'))
                }
            }
        }
    }
    return $found
}

# Sucht die Retail-Installation: erst Steam-Bibliotheken, dann die
# Uninstall-Registry (GOG, DVD), zuletzt die üblichen Ordnernamen auf allen
# festen Laufwerken. Ein Kandidat mit passendem Hash gewinnt immer; sonst
# kommt der erste Ordner mit einer FEAR.exe zurück, damit
# `Assert-RetailFearExe` die Abweichung benennen kann statt zu schweigen.
function Find-RetailRoot {
    $cfg = Get-FearVrReleaseConfig
    $candidates = New-Object Collections.Generic.List[string]
    $libraries = @(Get-SteamLibraryRoots)
    $gameFolders = @(
        'FEAR Ultimate Shooter Edition',
        'FEAR',
        'F.E.A.R',
        'F.E.A.R.',
        'FEAR Platinum Collection'
    )
    foreach ($library in $libraries) {
        foreach ($folder in $gameFolders) {
            $candidates.Add((Join-Path $library "steamapps\common\$folder"))
        }
    }

    # GOG, DVD-Installation und alles, was sich ordentlich registriert.
    foreach ($location in (Get-RegistryInstallLocations '*F.E.A.R*')) {
        $candidates.Add($location)
    }
    foreach ($location in (Get-RegistryInstallLocations '*FEAR*')) {
        $candidates.Add($location)
    }

    # Die üblichen Ordnernamen auf jedem festen Laufwerk.
    $parents = @(
        'Program Files (x86)',
        'Program Files',
        'Games',
        'GOG Games',
        'SteamLibrary\steamapps\common',
        'Games\steamapps\common',
        ''
    )
    $vendors = @('', 'Sierra\', 'Monolith Productions\', 'Vivendi Games\')
    foreach ($drive in (Get-LocalDriveRoots)) {
        foreach ($parent in $parents) {
            foreach ($vendor in $vendors) {
                foreach ($folder in $gameFolders) {
                    $candidates.Add(
                        [IO.Path]::Combine($drive + '\', $parent, "$vendor$folder"))
                }
            }
        }
    }

    # Ein getesteter Build gewinnt, danach jede andere 1.08-EXE (GOG,
    # Retail-DVD), zuletzt irgendeine FEAR.exe — die lehnt
    # `Assert-RetailFearExe` dann mit Begründung ab.
    $version108 = $null
    $anyExe = $null
    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        $exe = Join-Path $candidate 'FEAR.exe'
        if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { continue }
        if ($cfg.KnownRetailHashes[(Get-FileSha256 $exe)]) { return $candidate }
        if (-not $version108) {
            $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($exe).FileVersion
            if ($version -like '1.08*') { $version108 = $candidate }
        }
        if (-not $anyExe) { $anyExe = $candidate }
    }
    if ($version108) { return $version108 }
    return $anyExe
}

# Wie das Spiel gestartet wird. Steam verlangt den Umweg über
# `steam.exe -applaunch`, weil FEAR.exe dort ohne laufenden Client abbricht.
# GOG- und DVD-Installationen werden direkt gestartet; die Argumente sind
# dieselben, sie gehen nur nicht durch Steam.
function Get-RetailLaunchMode([string]$RetailRoot) {
    if ($RetailRoot -match '(?i)\\steamapps\\common\\' -and
        (Get-SteamExecutable)) {
        return 'steam'
    }
    return 'direct'
}

# Liefert die typischen Installationswurzeln auf allen angegebenen Laufwerken.
# Sierra ist der offizielle Standardpfad älterer Public-Tools-Installer und
# muss deshalb ausdrücklich enthalten sein, nicht nur Monolith Productions.
function Get-PublicToolsDefaultPaths([string[]]$DriveRoots) {
    if ($null -eq $DriveRoots -or $DriveRoots.Count -eq 0) {
        $DriveRoots = @(Get-LocalDriveRoots)
    }
    $roots = New-Object Collections.Generic.List[string]
    $folders = @(
        'Sierra\FEAR Public Tools',
        'Sierra\F.E.A.R. Public Tools',
        'Sierra Entertainment\FEAR Public Tools',
        'Monolith Productions\FEAR Public Tools',
        'FEAR Public Tools',
        'F.E.A.R. Public Tools'
    )
    $parents = @('Program Files (x86)', 'Program Files', 'Games', '')
    foreach ($drive in $DriveRoots) {
        foreach ($parent in $parents) {
            foreach ($folder in $folders) {
                $roots.Add([IO.Path]::Combine($drive + '\', $parent, $folder))
            }
        }
    }
    return $roots
}

# Sucht ein Public-Tools-Runtime-Verzeichnis und verifiziert es über den
# Hash des unveränderten VC7.1-GameClient.dll. Der Installer der Public Tools
# lässt den Zielordner frei wählen, deshalb werden neben den Standardorten
# auch die Registry und die üblichen Ordnernamen je Laufwerk geprüft.
function Find-PublicToolsGame {
    $roots = New-Object Collections.Generic.List[string]
    foreach ($location in (Get-RegistryInstallLocations '*Public Tools*')) {
        $roots.Add($location)
    }
    foreach ($location in (Get-PublicToolsDefaultPaths)) {
        $roots.Add($location)
    }
    foreach ($root in $roots) {
        if ([string]::IsNullOrWhiteSpace($root)) { continue }
        # Ein Nutzer, der den Pfad selbst angibt, zeigt mal auf die
        # Installationswurzel und mal direkt auf Dev\Runtime\Game.
        foreach ($suffix in @('Dev\Runtime\Game', 'Runtime\Game', 'Game', '')) {
            $game = if ($suffix) { Join-Path $root $suffix } else { $root }
            if (Test-PublicToolsGame $game) { return $game }
        }
    }
    return $null
}

# Fragt bei erfolgloser automatischer Suche ausdrücklich nach einem Pfad. Der
# Nutzer darf die Installationswurzel oder direkt Dev\Runtime\Game eingeben;
# akzeptiert wird erst ein Verzeichnis mit der originalen 1.08-GameClient.dll.
function Request-PublicToolsGame([string]$InstallerPath) {
    Write-Host ''
    Write-Host 'F.E.A.R. Public Tools 1.08 wurden nicht automatisch gefunden.' `
        -ForegroundColor Yellow
    Write-Host 'Geprüft wurden Registry-Einträge und Standardpfade auf allen Laufwerken,'
    Write-Host 'einschließlich:'
    Write-Host '  C:\Program Files (x86)\Sierra\FEAR Public Tools'
    if (-not [string]::IsNullOrWhiteSpace($InstallerPath) -and
        (Test-Path -LiteralPath $InstallerPath -PathType Leaf)) {
        Write-Host "Mitgelieferter offizieller Installer: $InstallerPath"
    }
    Write-Host ''
    Write-Host (
        'Gib die Public-Tools-Installationswurzel oder direkt ' +
        'Dev\Runtime\Game an.')
    Write-Host 'Eine leere Eingabe bricht ab.'

    for (;;) {
        $entered = Read-Host 'Pfad zu F.E.A.R. Public Tools 1.08'
        if ([string]::IsNullOrWhiteSpace($entered)) {
            return $null
        }
        $resolved = Resolve-PublicToolsGame $entered
        if (Test-PublicToolsGame $resolved) {
            Write-Host "Public Tools bestätigt: $resolved" `
                -ForegroundColor Green
            return $resolved
        }
        Write-Host (
            'Dort wurde keine unveränderte Public-Tools-1.08-' +
            'GameClient.dll gefunden. Bitte anderen Pfad eingeben.') `
            -ForegroundColor Red
    }
}

# Nimmt eine Nutzereingabe entgegen — Installationswurzel oder direkt das
# Runtime-Verzeichnis — und liefert das verifizierte Dev\Runtime\Game zurück.
function Resolve-PublicToolsGame([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    $trimmed = $Path.Trim().Trim('"')
    foreach ($suffix in @('', 'Dev\Runtime\Game', 'Runtime\Game', 'Game')) {
        $game = if ($suffix) { Join-Path $trimmed $suffix } else { $trimmed }
        if (Test-PublicToolsGame $game) {
            return [IO.Path]::GetFullPath($game)
        }
    }
    return $null
}

function Test-PublicToolsGame([string]$GameDirectory) {
    $cfg = Get-FearVrReleaseConfig
    if ([string]::IsNullOrWhiteSpace($GameDirectory)) { return $false }
    $client = Join-Path $GameDirectory 'GameClient.dll'
    if (-not (Test-Path -LiteralPath $client -PathType Leaf)) { return $false }
    return (Get-FileSha256 $client) -eq $cfg.PublicToolsGameClientSha256
}

# --- OpenXR-Runtime ----------------------------------------------------------
# Umgeschaltet wird über XR_RUNTIME_JSON nur für den Hostprozess. Die
# systemweite Einstellung unter HKLM\...\Khronos\OpenXR\1\ActiveRuntime wird
# nie geschrieben.
function Get-OpenXrRuntimeName([string]$ManifestPath) {
    if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) { return $null }
    try {
        return ([IO.File]::ReadAllText($ManifestPath) |
            ConvertFrom-Json).runtime.name
    } catch { return $null }
}

function Get-OpenXrRuntimeKind([string]$ManifestPath) {
    $name = Get-OpenXrRuntimeName $ManifestPath
    if ($null -eq $name) { return 'other' }
    if ($name -match 'SteamVR') { return 'steamvr' }
    if ($name -match 'VirtualDesktop') { return 'vdxr' }
    return 'other'
}

function Resolve-OpenXrRuntime([string]$Runtime) {
    $cfg = Get-FearVrReleaseConfig
    $activePath = $null
    try {
        $activePath = (Get-ItemProperty 'HKLM:\SOFTWARE\Khronos\OpenXR\1' `
            -ErrorAction Stop).ActiveRuntime
    } catch { }
    if ([string]::IsNullOrWhiteSpace($Runtime) -or $Runtime -eq 'active') {
        if ([string]::IsNullOrWhiteSpace($activePath)) {
            throw @'
Keine aktive OpenXR-Runtime gefunden.
SteamVR oder den Virtual Desktop Streamer starten und dort als OpenXR-Runtime
setzen, oder mit -Runtime steamvr bzw. -Runtime vdxr starten.
'@
        }
        return [pscustomobject]@{
            Path = $null; Name = Get-OpenXrRuntimeName $activePath
            Kind = Get-OpenXrRuntimeKind $activePath; Override = $false
        }
    }
    $manifest = switch ($Runtime) {
        'steamvr' {
            if (-not [string]::IsNullOrWhiteSpace($activePath) -and
                (Test-Path -LiteralPath $activePath -PathType Leaf) -and
                (Get-OpenXrRuntimeKind $activePath) -eq 'steamvr') {
                $activePath
            } else {
                $found = Find-SteamVrManifest
                if ($found) { $found } else { $null }
            }
        }
        'vdxr'    { $cfg.VdxrManifest }
        default   { $Runtime }
    }
    if ([string]::IsNullOrWhiteSpace($manifest) -and $Runtime -eq 'steamvr') {
        throw @'
SteamVR wurde weder als aktive OpenXR-Runtime noch in einer registrierten
Steam-Bibliothek gefunden. Steam einmal starten oder den vollstaendigen Pfad
zu steamxr_win64.json mit -Runtime "<Pfad>" angeben.
'@
    }
    if ([string]::IsNullOrWhiteSpace($manifest) -or
        -not (Test-Path -LiteralPath $manifest -PathType Leaf)) {
        throw "OpenXR-Runtime-Manifest nicht gefunden: $manifest"
    }
    return [pscustomobject]@{
        Path = [IO.Path]::GetFullPath($manifest)
        Name = Get-OpenXrRuntimeName $manifest
        Kind = Get-OpenXrRuntimeKind $manifest
        Override = $true
    }
}
