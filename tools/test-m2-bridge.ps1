<#
.SYNOPSIS
    Testet die M2-D3D9/D3D11-Brücke in einer isolierten Test-Stage.

.DESCRIPTION
    Startet den x64-OpenXR-Host und den synthetischen x86-D3D9-Producer mit
    einer einmaligen Local\-IPC-ID. Der neben dem Producer gestagte d3d9.dll-
    Proxy wird geladen; weder Retail- noch Public-Tools-Dateien werden geändert.

    Der Producer erzeugt wechselnde Framefarben, minimiert/stellt sein Fenster
    wieder her und führt einen D3D9-Reset mit Auflösungswechsel aus. Danach
    werden die strukturierten Host-/Proxy-Logs auf die M2-Gates geprüft.

.PARAMETER AbortHost
    Beendet den von diesem Skript gestarteten Host vorzeitig. Der Producer muss
    danach ohne Hänger bis zum Ende weiterlaufen (Fail-open-Test).

.PARAMETER ClassicD3D9
    Erzeugt das Testgerät über Direct3DCreate9 statt Direct3DCreate9Ex und
    prüft damit den CPU-zu-D3D9Ex-Kompatibilitätspfad des Retail-Spiels.

.PARAMETER Stereo
    Rendert synthetisch ein rotes linkes und ein blaues rechtes Auge und
    prüft zusätzlich den M3-Stereo-Transportvertrag.

.PARAMETER D3D9ExCompat
    Ruft die klassische Direct3DCreate9-Oberfläche auf, erwartet aber ein
    durch Direct3DCreate9Ex gestütztes Gerät und den direkten Shared-Pfad.

.PARAMETER Fullscreen
    Verwendet Retail-konformes exklusives Vollbild mit einer klassischen
    Null-Hertz-Anforderung, die der D3D9Ex-Fassade übersetzt.

.PARAMETER Multisample4
    Erstellt wie Retail einen vierfach multisampleten Backbuffer.

.PARAMETER SwapChainPresent
    Präsentiert über IDirect3DSwapChain9::Present wie die Retail-Engine.

.PARAMETER ImplementationPresent
    Präsentiert über den klassischen, implementation-level Device-Pfad.

.PARAMETER MagentaSurfaceTest
    Füllt die exakt an Present übergebene Oberfläche für ein Bild magenta.

.PARAMETER OffscreenFinal
    Präsentiert einen schwarzen Backbuffer, während Render-Target 0 Farbe
    enthält, und prüft den Final-Composite-Fallback.
#>
[CmdletBinding()]
param(
    [ValidateRange(120, 10000)]
    [int]$Frames = 600,

    [switch]$AbortHost,

    [switch]$ClassicD3D9,

    [switch]$Stereo,

    [switch]$D3D9ExCompat,

    [switch]$Fullscreen,

    [switch]$Multisample4,

    [switch]$SwapChainPresent,

    [switch]$ImplementationPresent,

    [switch]$MagentaSurfaceTest,

    [switch]$OffscreenFinal
)

$ErrorActionPreference = 'Stop'
$milestoneLabel = if ($Stereo) { 'M3' } else { 'M2' }
$milestoneSlug = $milestoneLabel.ToLowerInvariant()
. "$PSScriptRoot\_fearvr-env.ps1"
$cfg = Get-FearVrConfig

function Start-M2Process(
    [string]$FilePath,
    [string[]]$Arguments,
    [string]$WorkingDirectory
) {
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = $Arguments -join ' '
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.UseShellExecute = $false
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "Prozess konnte nicht gestartet werden: $FilePath"
    }
    return $process
}

$hostExe = Assert-UnderProjectRoot (
    Join-Path $cfg.ProjectRoot 'build\x64\src\host64\RelWithDebInfo\fearvr-host.exe'
)
$producerExe = Assert-UnderProjectRoot (
    Join-Path $cfg.ProjectRoot 'build\x86\tests\RelWithDebInfo\d3d9_test_producer.exe'
)
$proxyDll = Assert-UnderProjectRoot (
    Join-Path (Split-Path -Parent $producerExe) 'd3d9.dll'
)
foreach ($required in @($hostExe, $producerExe, $proxyDll)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "M2-Buildartefakt fehlt: $required. Zuerst x86 und x64 bauen."
    }
}

$existingHosts = @(Get-Process -Name 'fearvr-host' -ErrorAction SilentlyContinue)
if ($existingHosts.Count -ne 0) {
    throw "Es läuft bereits fearvr-host.exe (PID: $($existingHosts.Id -join ', '))."
}

$logDirectory = Assert-UnderProjectRoot (
    Join-Path $cfg.ProjectRoot (
        "logs\$milestoneSlug-" +
        (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss')
    )
)
New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null

$sessionId = [uint64]([DateTime]::UtcNow.Ticks)
$sessionId = $sessionId -bxor ([uint64]$PID -shl 32)
if ($sessionId -eq 0) {
    $sessionId = 1
}
$sessionText = '0x{0:X16}' -f $sessionId
$hostFrames = if ($AbortHost) { 100000 } else { $Frames + 300 }
Write-Host "=== $milestoneLabel Bridge-Test (isoliert) ===" `
    -ForegroundColor Cyan
Write-Host "Session:  $sessionText"
Write-Host "Logs:     $logDirectory"
Write-Host "Producer: $producerExe"
Write-Host "Proxy:    $proxyDll"
$testMode = if ($AbortHost) {
    'Host-Abbruch / Fail-open'
} elseif ($D3D9ExCompat -and $SwapChainPresent) {
    'Classic API / D3D9Ex compatibility / swap-chain Present / direct'
} elseif ($D3D9ExCompat -and $ImplementationPresent) {
    'Classic API / D3D9Ex compatibility / implementation Present / direct'
} elseif ($D3D9ExCompat -and $Fullscreen -and $Multisample4) {
    'Classic API / D3D9Ex compatibility / fullscreen / 4x MSAA / direct'
} elseif ($D3D9ExCompat -and $Fullscreen) {
    'Classic API / D3D9Ex compatibility / fullscreen / direct'
} elseif ($Stereo -and $D3D9ExCompat) {
    'M3 Stereo / Classic API / D3D9Ex compatibility / direct'
} elseif ($Stereo -and $ClassicD3D9) {
    'M3 Stereo / Classic D3D9 / CPU-D3D9Ex + Reset'
} elseif ($Stereo) {
    'M3 Stereo / D3D9Ex direkt + Reset + Spielende'
} elseif ($ClassicD3D9) {
    'Classic D3D9 / CPU-D3D9Ex + Reset'
} else {
    'D3D9Ex direkt + Reset + Spielende'
}
Write-Host "Modus:    $testMode"

$hostProcess = $null
$producerProcess = $null
try {
    $hostArguments = @(
        '--ipc-session', $sessionText,
        '--max-frames', $hostFrames,
        '--exit-on-game-disconnect',
        '--log-dir', $logDirectory
    )
    $hostProcess = Start-M2Process `
        -FilePath $hostExe `
        -Arguments $hostArguments `
        -WorkingDirectory (Split-Path -Parent $hostExe)

    $readyDeadline = (Get-Date).AddSeconds(30)
    $hostLog = $null
    do {
        Start-Sleep -Milliseconds 200
        $hostProcess.Refresh()
        if ($hostProcess.HasExited) {
            throw "OpenXR-Host endete vor XR-ready (Exitcode $($hostProcess.ExitCode))."
        }
        $hostLog = Get-ChildItem -LiteralPath $logDirectory `
            -Filter 'host-*.log' -File -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTimeUtc -Descending |
            Select-Object -First 1
        $hostReady = $null -ne $hostLog -and
            (Get-Content -Raw -LiteralPath $hostLog.FullName) -match '"event":"xr_ready"'
    } until ($hostReady -or (Get-Date) -ge $readyDeadline)
    if (-not $hostReady) {
        throw 'OpenXR-Host wurde innerhalb von 30 Sekunden nicht XR-ready.'
    }
    $hostText = Get-Content -Raw -LiteralPath $hostLog.FullName
    $luidMatch = [regex]::Match(
        $hostText,
        'd3d11_adapter.+?luid=0x([0-9A-Fa-f]+):([0-9A-Fa-f]+)'
    )
    if (-not $luidMatch.Success) {
        throw 'OpenXR-Adapter-LUID fehlt im Hostlog.'
    }
    $highPart = [uint32]::Parse(
        $luidMatch.Groups[1].Value,
        [Globalization.NumberStyles]::HexNumber
    )
    $lowPart = [uint32]::Parse(
        $luidMatch.Groups[2].Value,
        [Globalization.NumberStyles]::HexNumber
    )
    $adapterLuid = ([uint64]$highPart -shl 32) -bor [uint64]$lowPart
    $adapterText = '0x{0:X16}' -f $adapterLuid

    $producerArguments = @(
        '-fearvr-session', $sessionText,
        '-fearvr-logdir', $logDirectory,
        '--adapter-luid', $adapterText,
        '--frames', $Frames
    )
    if ($ClassicD3D9) {
        $producerArguments += '--classic-d3d9'
    }
    if ($D3D9ExCompat) {
        $producerArguments += '--classic-d3d9'
        $producerArguments += '-fearvr-d3d9ex-compat'
        $producerArguments += '--expect-ex-device'
        $producerArguments += '--probe-managed-resources'
        $producerArguments += '--scene-boundary'
        if ($SwapChainPresent) {
            $producerArguments += '--swapchain-present'
        } elseif (-not $ImplementationPresent) {
            $producerArguments += '--present-ex'
        }
    }
    if ($ClassicD3D9 -and -not $D3D9ExCompat) {
        $producerArguments += '--scene-boundary'
    }
    if ($Fullscreen) {
        $producerArguments += '--fullscreen-default-refresh'
    }
    if ($Multisample4) {
        $producerArguments += '--multisample4'
    }
    if ($Stereo) {
        $producerArguments += '-fearvr-stereo'
        $producerArguments += '--stereo'
    }
    if ($MagentaSurfaceTest) {
        $producerArguments += '-fearvr-magenta-surface-test'
    }
    if ($OffscreenFinal) {
        $producerArguments += '--offscreen-final'
    }
    $producerProcess = Start-M2Process `
        -FilePath $producerExe `
        -Arguments $producerArguments `
        -WorkingDirectory (Split-Path -Parent $producerExe)

    if ($AbortHost) {
        Start-Sleep -Milliseconds 2500
        $hostProcess.Refresh()
        if (-not $hostProcess.HasExited) {
            Write-Host "Beende ausschließlich Test-Host PID $($hostProcess.Id) ..."
            Stop-Process -Id $hostProcess.Id -Force
            $hostProcess.WaitForExit()
        }
    }

    if (-not $producerProcess.WaitForExit(30000)) {
        throw 'D3D9-Producer hing länger als 30 Sekunden.'
    }
    $producerProcess.WaitForExit()
    $producerProcess.Refresh()
    if ($producerProcess.ExitCode -ne 0) {
        throw "D3D9-Producer endete mit Exitcode $($producerProcess.ExitCode)."
    }

    if (-not $AbortHost) {
        if (-not $hostProcess.WaitForExit(30000)) {
            throw 'OpenXR-Host hing nach Spielende länger als 30 Sekunden.'
        }
        $hostProcess.WaitForExit()
        $hostProcess.Refresh()
        if ($hostProcess.ExitCode -ne 0) {
            throw "OpenXR-Host endete mit Exitcode $($hostProcess.ExitCode)."
        }
    }

    $proxyLog = Get-ChildItem -LiteralPath $logDirectory `
        -Filter 'proxy-*.log' -File |
        Sort-Object LastWriteTimeUtc -Descending |
        Select-Object -First 1
    if ($null -eq $proxyLog) {
        throw 'Kein Proxy-Log: Die isolierte d3d9.dll wurde nicht geladen.'
    }
    $proxyText = Get-Content -Raw -LiteralPath $proxyLog.FullName
    $hostText = Get-Content -Raw -LiteralPath $hostLog.FullName
    if ($proxyText -match '"level":"ERROR"' -or
        $hostText -match '"level":"ERROR"') {
        throw "$milestoneLabel-Gate: Ein Laufzeitfehler wurde protokolliert."
    }
    $requiredProxyEvents = @(
        'ipc_created',
        'host_connected',
        'adapter_match',
        'shared_resources',
        'frame_ready'
    )
    if (-not $Fullscreen) {
        $requiredProxyEvents += 'device_reset_begin'
        $requiredProxyEvents += 'device_reset_complete'
    }
    if ($D3D9ExCompat -and -not $Fullscreen) {
        $requiredProxyEvents += 'd3d9ex_reset_redirected'
    }
    if ($D3D9ExCompat -or $ClassicD3D9) {
        if ($D3D9ExCompat) {
            $requiredProxyEvents += 'd3d9ex_managed_index_create_details'
            $requiredProxyEvents += 'd3d9ex_managed_index_lock'
            $requiredProxyEvents += 'd3d9ex_managed_index_set'
        }
        $requiredProxyEvents += 'app_local_present_detour'
        $requiredProxyEvents += 'd3d9_end_scene_activity'
        $requiredProxyEvents += 'd3d9_present_source'
        $requiredProxyEvents += 'd3d9_pixel_probe'
    }
    if ($SwapChainPresent) {
        $requiredProxyEvents += 'swapchain_present_hooked'
    }
    if ($Stereo) {
        $requiredProxyEvents += 'stereo_frame_staged'
    }
    if ($MagentaSurfaceTest) {
        $requiredProxyEvents += 'magenta_surface_test'
    }
    if ($OffscreenFinal) {
        $requiredProxyEvents += 'capture_source_fallback'
    }
    foreach ($event in $requiredProxyEvents) {
        if ($proxyText -notmatch ('"event":"' + [regex]::Escape($event) + '"')) {
            throw "Proxy-Gate fehlt im Log: $event"
        }
    }
    foreach ($event in @(
        'ipc_connected',
        'adapter_match',
        'ipc_frame',
        'pixel_probe'
    )) {
        if ($hostText -notmatch ('"event":"' + [regex]::Escape($event) + '"')) {
            throw "Host-Gate fehlt im Log: $event"
        }
    }
    if ($Stereo -and
        $hostText -notmatch '"event":"symmetric_stereo_fov"') {
        Write-Warning (
            'Symmetrisches FOV noch nicht live bestätigt: Die OpenXR-Session ' +
            'benötigt dafür Headset-Fokus und gültige Views. Der ' +
            'Stereo-Transport selbst ist bestätigt.'
        )
    }
    if ($hostText -notmatch '"event":"pixel_probe".+nonzero_samples=[1-9][0-9]*') {
        throw (
            "$milestoneLabel-Gate: Shared frames contained only black pixels."
        )
    }
    if ($Stereo) {
        $leftStereoProbe = $false
        $rightStereoProbe = $false
        foreach ($line in ($hostText -split "\r?\n")) {
            if ($line -notmatch '"event":"pixel_probe"') {
                continue
            }
            try {
                $entry = $line | ConvertFrom-Json
            } catch {
                continue
            }
            $match = [regex]::Match(
                [string]$entry.message,
                'eye=(\d+).+avg_bgr=(\d+),(\d+),(\d+)'
            )
            if (-not $match.Success) {
                continue
            }
            $eye = [int]$match.Groups[1].Value
            $blue = [int]$match.Groups[2].Value
            $red = [int]$match.Groups[4].Value
            if ($eye -eq 0 -and $red -gt ($blue + 64)) {
                $leftStereoProbe = $true
            }
            if ($eye -eq 1 -and $blue -gt ($red + 64)) {
                $rightStereoProbe = $true
            }
        }
        if (-not $leftStereoProbe -or -not $rightStereoProbe) {
            throw (
                'M3-Gate: expected distinct red-left and blue-right ' +
                'shared surfaces were not observed.'
            )
        }
    }
    if ($MagentaSurfaceTest -and
        $proxyText -notmatch (
            '"event":"d3d9_pixel_probe".+' +
            'surface=presented_magenta.+' +
            'nonzero_samples=[1-9][0-9]*.+' +
            'avg_bgr=255,0,255'
        )) {
        throw (
            'Magenta-Gate: the exact presented surface did not read back ' +
            'as magenta.'
        )
    }
    if ($AbortHost -and
        $proxyText -notmatch '"event":"host_disconnected"') {
        throw 'Fail-open-Gate fehlt: Proxy erkannte den Host-Abbruch nicht.'
    }
    if ($D3D9ExCompat -and
        $proxyText -notmatch 'path=direct') {
        throw 'D3D9Ex-Kompatibilitäts-Gate fehlt: path=direct.'
    }
    if ($D3D9ExCompat -and
        $proxyText -match 'path=cpu_d3d9ex') {
        throw 'D3D9Ex-Kompatibilitäts-Gate: CPU-Fallback wurde aktiviert.'
    }
    $expectedPresentPath = if ($SwapChainPresent) {
        'swapchain_present'
    } elseif ($D3D9ExCompat -and -not $ImplementationPresent) {
        'device_present_ex'
    } else {
        'app_local_device_present'
    }
    if (($D3D9ExCompat -or $ClassicD3D9) -and
        $proxyText -notmatch (
            '"event":"d3d9_present_source".+path=' +
            [regex]::Escape($expectedPresentPath)
        )) {
        throw (
            'Present-Pfad-Gate fehlt: erwartete exakte Quelle für path=' +
            $expectedPresentPath
        )
    }

    $passed = if ($Stereo) {
        'M3-Stereo-Transporttest bestanden.'
    } else {
        'M2-Bridge-Test bestanden.'
    }
    Write-Host $passed -ForegroundColor Green
    Write-Host "Hostlog:  $($hostLog.FullName)"
    Write-Host "Proxylog: $($proxyLog.FullName)"
} finally {
    foreach ($process in @($producerProcess, $hostProcess)) {
        if ($null -ne $process) {
            $process.Refresh()
            if (-not $process.HasExited) {
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            }
        }
    }
}
