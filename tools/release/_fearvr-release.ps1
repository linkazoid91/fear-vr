# =============================================================================
# Shared definitions for the distributed F.E.A.R. VR package.
# Intentionally independent of the development repository: the package ships
# only its own binaries and obtains Public Tools modules from the user's PC.
# Contains no executable actions beyond function and data definitions.
# =============================================================================

$script:FearVrRelease = [ordered]@{
    # Target Retail version. Version-dependent hooks stay off on mismatch.
    ExpectedVersion = '1.08.282.0'
    ExpectedSha256  = 'D5EBC38A4F12B772C9112A2811C290ADB6C5052D3BC2F817302D38CF55BB2CBE'

    # Known, actually tested FEAR.exe builds. Other 1.08 executables (GOG or
    # retail disc) may work but are unconfirmed. Every version-dependent
    # signature used by this mod lives in the Public Tools GameOrig.dll, not
    # FEAR.exe, so an unknown hash is reported rather than rejected.
    KnownRetailHashes = [ordered]@{
        'D5EBC38A4F12B772C9112A2811C290ADB6C5052D3BC2F817302D38CF55BB2CBE' =
            'Steam, Ultimate Shooter Edition 1.08'
        'D662DCCDB2EBD17D1ACED7C725A8724060010718146E0C0074DA5E8EF89B82B4' =
            'Steam 1.08 + HDTextures4FEAR/XP v2.0.2'
    }

    # Unmodified VC7.1 GameClient.dll from Public Tools 1.08. Used to identify
    # a valid Public Tools runtime directory.
    PublicToolsGameClientSha256 =
        'B5F1F1976227FD0E6F1C32BD2BEEDFB117E68A87A07BB42D06BE489DD08A63BA'

    SteamAppId = 21090

    # Modules copied from the local Public Tools installation.
    # Key = staged target name; value = source name under Runtime\Game.
    PublicToolsModules = [ordered]@{
        'GameOrig.dll'    = 'GameClient.dll'
        'GameServer.dll'  = 'GameServer.dll'
        'ClientFx.fxd'    = 'ClientFx.fxd'
        'FEAR.dep'        = 'FEAR.dep'
        'FEARMod.Arch00s' = 'FEARMod.Arch00s'
    }

    # Modules supplied by this package.
    BundledModules = [ordered]@{
        'GameClient.dll'  = 'bin\x86\GameClient.dll'
        'fearvr-d3d9.dll' = 'bin\x86\fearvr-d3d9.dll'
    }

    SteamVrManifest =
        'C:\Program Files (x86)\Steam\steamapps\common\SteamVR\steamxr_win64.json'
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

# App-local proxy ownership is proven by both the recorded path and hash.
# This prevents install/update/uninstall from overwriting another wrapper.
function Test-FearVrSamePath([string]$Left, [string]$Right) {
    if ([string]::IsNullOrWhiteSpace($Left) -or
        [string]::IsNullOrWhiteSpace($Right)) {
        return $false
    }
    try {
        return [IO.Path]::GetFullPath($Left).Equals(
            [IO.Path]::GetFullPath($Right),
            [StringComparison]::OrdinalIgnoreCase)
    } catch {
        return $false
    }
}

# Destructive operations must compare the filesystem objects behind path
# aliases, not only their spelling. This resolves junctions, SUBST drives, and
# short (8.3) names through a handle opened without FILE_FLAG_OPEN_REPARSE_POINT.
function Initialize-FearVrNativePathSupport {
    if ('FearVr.Release.NativePath' -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace FearVr.Release {
    public static class NativePath {
        private const uint FileFlagBackupSemantics = 0x02000000;
        private const uint FileFlagOpenReparsePoint = 0x00200000;
        private const uint GenericRead = 0x80000000;
        private const uint DeleteAccess = 0x00010000;
        private const uint VolumeNameGuid = 0x1;
        private const uint FileAttributeReparsePoint = 0x00000400;
        private const int FileDispositionInfo = 4;

        [StructLayout(LayoutKind.Sequential)]
        private struct ByHandleFileInformation {
            internal uint FileAttributes;
            internal System.Runtime.InteropServices.ComTypes.FILETIME CreationTime;
            internal System.Runtime.InteropServices.ComTypes.FILETIME LastAccessTime;
            internal System.Runtime.InteropServices.ComTypes.FILETIME LastWriteTime;
            internal uint VolumeSerialNumber;
            internal uint FileSizeHigh;
            internal uint FileSizeLow;
            internal uint NumberOfLinks;
            internal uint FileIndexHigh;
            internal uint FileIndexLow;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct FileDispositionInformation {
            [MarshalAs(UnmanagedType.U1)]
            internal bool DeleteFile;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode,
            SetLastError = true)]
        private static extern SafeFileHandle CreateFileW(
            string fileName, uint desiredAccess, FileShare shareMode,
            IntPtr securityAttributes, FileMode creationDisposition,
            uint flagsAndAttributes, IntPtr templateFile);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode,
            SetLastError = true)]
        private static extern uint GetFinalPathNameByHandleW(
            SafeFileHandle file, StringBuilder path, uint pathLength,
            uint flags);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetFileInformationByHandle(
            SafeFileHandle file, out ByHandleFileInformation information);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool SetFileInformationByHandle(
            SafeFileHandle file, int informationClass,
            ref FileDispositionInformation information,
            uint bufferSize);

        private static string ResolveHandle(
            SafeFileHandle handle, string path) {
            int capacity = 1024;
            while (capacity <= 32768) {
                StringBuilder result = new StringBuilder(capacity);
                uint length = GetFinalPathNameByHandleW(
                    handle, result, (uint)result.Capacity,
                    VolumeNameGuid);
                if (length == 0) {
                    throw new Win32Exception(Marshal.GetLastWin32Error(),
                        "Cannot resolve physical path: " + path);
                }
                if (length < result.Capacity) {
                    return result.ToString();
                }
                capacity = checked((int)length + 1);
            }
            throw new PathTooLongException(
                "Resolved path is too long: " + path);
        }

        public static string Resolve(string path) {
            using (SafeFileHandle handle = CreateFileW(
                path, 0,
                FileShare.Read | FileShare.Write | FileShare.Delete,
                IntPtr.Zero, FileMode.Open, FileFlagBackupSemantics,
                IntPtr.Zero)) {
                if (handle.IsInvalid) {
                    throw new Win32Exception(Marshal.GetLastWin32Error(),
                        "Cannot open path for physical identity: " + path);
                }
                return ResolveHandle(handle, path);
            }
        }

        public static string DeleteIfSha256Matches(
            string path, string expectedSha256,
            string expectedPhysicalParent) {
            SafeFileHandle handle = CreateFileW(
                path, GenericRead | DeleteAccess,
                FileShare.Read | FileShare.Delete,
                IntPtr.Zero, FileMode.Open, FileFlagOpenReparsePoint,
                IntPtr.Zero);
            if (handle.IsInvalid) {
                int error = Marshal.GetLastWin32Error();
                handle.Dispose();
                if (error == 2 || error == 3) {
                    return "Missing";
                }
                throw new Win32Exception(error,
                    "Cannot open owned file for removal: " + path);
            }
            using (handle) {
                ByHandleFileInformation fileInformation;
                if (!GetFileInformationByHandle(handle, out fileInformation)) {
                    throw new Win32Exception(Marshal.GetLastWin32Error(),
                        "Cannot inspect owned file: " + path);
                }
                if ((fileInformation.FileAttributes &
                        FileAttributeReparsePoint) != 0) {
                    return "ReparsePoint";
                }
                if (!String.IsNullOrEmpty(expectedPhysicalParent)) {
                    string physicalPath = ResolveHandle(handle, path);
                    int separator = physicalPath.LastIndexOf('\\');
                    string physicalParent = separator > 0
                        ? physicalPath.Substring(0, separator)
                        : String.Empty;
                    if (!physicalParent.Equals(
                            expectedPhysicalParent.TrimEnd('\\'),
                            StringComparison.OrdinalIgnoreCase)) {
                        return "PhysicalIdentityChanged";
                    }
                }
                using (FileStream stream = new FileStream(
                    handle, FileAccess.Read, 1048576, false)) {
                    string actual;
                    using (SHA256 sha256 = SHA256.Create()) {
                        actual = BitConverter.ToString(
                            sha256.ComputeHash(stream)).Replace("-", "");
                    }
                    if (!actual.Equals(expectedSha256,
                            StringComparison.OrdinalIgnoreCase)) {
                        return "Modified";
                    }
                    FileDispositionInformation disposition =
                        new FileDispositionInformation { DeleteFile = true };
                    if (!SetFileInformationByHandle(
                            stream.SafeFileHandle, FileDispositionInfo,
                            ref disposition,
                            (uint)Marshal.SizeOf(
                                typeof(FileDispositionInformation)))) {
                        throw new Win32Exception(Marshal.GetLastWin32Error(),
                            "Cannot remove owned file: " + path);
                    }
                }
            }
            return "Removed";
        }
    }
}
'@
}

function Get-FearVrPhysicalPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw 'A physical path cannot be empty.'
    }
    Initialize-FearVrNativePathSupport
    $full = [IO.Path]::GetFullPath($Path)
    $probe = $full
    $missing = New-Object Collections.Generic.List[string]
    while (-not (Test-Path -LiteralPath $probe)) {
        $leaf = [IO.Path]::GetFileName($probe.TrimEnd('\'))
        if ([string]::IsNullOrWhiteSpace($leaf)) {
            throw "Cannot resolve a physical ancestor for '$full'."
        }
        $missing.Insert(0, $leaf)
        $parent = [IO.Path]::GetDirectoryName($probe.TrimEnd('\'))
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $probe) {
            throw "Cannot resolve a physical ancestor for '$full'."
        }
        $probe = $parent
    }

    $nativeType = 'FearVr.Release.NativePath' -as [type]
    $resolved = [string]$nativeType.GetMethod(
        'Resolve',
        [Reflection.BindingFlags]'Static, Public').Invoke(
            $null, @($probe))
    foreach ($leaf in $missing) {
        $resolved = $resolved.TrimEnd('\') + '\' + $leaf
    }
    return $resolved
}

function Test-FearVrSamePhysicalPath([string]$Left, [string]$Right) {
    $leftPhysical = (Get-FearVrPhysicalPath $Left).TrimEnd('\')
    $rightPhysical = (Get-FearVrPhysicalPath $Right).TrimEnd('\')
    return $leftPhysical.Equals(
        $rightPhysical, [StringComparison]::OrdinalIgnoreCase)
}

function Remove-FearVrFileByOwnedHash(
    [string]$Path,
    [string]$ExpectedSha256,
    [string]$ExpectedPhysicalParent
) {
    if ($ExpectedSha256 -notmatch '^[0-9A-Fa-f]{64}$') {
        throw "Refusing removal with an invalid ownership hash: $Path"
    }
    Initialize-FearVrNativePathSupport
    $nativeType = 'FearVr.Release.NativePath' -as [type]
    return [string]$nativeType.GetMethod(
        'DeleteIfSha256Matches',
        [Reflection.BindingFlags]'Static, Public').Invoke(
            $null, @(
                [IO.Path]::GetFullPath($Path),
                $ExpectedSha256,
                [string]$ExpectedPhysicalParent))
}

function Test-FearVrPhysicalPathOverlap([string]$Left, [string]$Right) {
    $leftPhysical = (Get-FearVrPhysicalPath $Left).TrimEnd('\')
    $rightPhysical = (Get-FearVrPhysicalPath $Right).TrimEnd('\')
    if ($leftPhysical.Equals(
            $rightPhysical, [StringComparison]::OrdinalIgnoreCase)) {
        return $true
    }
    return $leftPhysical.StartsWith(
        $rightPhysical + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $rightPhysical.StartsWith(
            $leftPhysical + '\', [StringComparison]::OrdinalIgnoreCase)
}

function Enter-FearVrInstallMutex([string]$InstallDir) {
    $identity = (Get-FearVrPhysicalPath $InstallDir).ToUpperInvariant()
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $digest = ([BitConverter]::ToString(
            $sha256.ComputeHash(
                [Text.Encoding]::UTF8.GetBytes($identity)))).Replace('-', '')
    } finally {
        $sha256.Dispose()
    }
    $mutex = [Threading.Mutex]::new(
        $false, 'Local\FearVr.Release.' + $digest.Substring(0, 32))
    $acquired = $false
    try {
        $acquired = $mutex.WaitOne(0)
    } catch [Threading.AbandonedMutexException] {
        $acquired = $true
    }
    if (-not $acquired) {
        $mutex.Dispose()
        throw "Another install, recovery, or uninstall is active for '$InstallDir'."
    }
    return $mutex
}

function Exit-FearVrInstallMutex($Mutex) {
    if ($null -eq $Mutex) { return }
    try { $Mutex.ReleaseMutex() } catch [ApplicationException] { }
    $Mutex.Dispose()
}

function Assert-FearVrSafeInstallDirectory(
    [string]$InstallDir,
    [string[]]$ProtectedRoots = @()
) {
    $install = [IO.Path]::GetFullPath($InstallDir)
    $lexicalRoot = [IO.Path]::GetPathRoot($install)
    if (Test-FearVrSamePath $install $lexicalRoot) {
        throw "Refusing to use a filesystem root as InstallDir: $install"
    }

    $physical = (Get-FearVrPhysicalPath $install).TrimEnd('\')
    if ($physical -match '^\\\\\?\\Volume\{[0-9A-Fa-f-]+\}$' -or
        $physical -match '^\\\\\?\\UNC\\[^\\]+\\[^\\]+$' -or
        $physical -match '^[A-Za-z]:$' -or
        $physical -match '^\\\\[^\\]+\\[^\\]+$') {
        throw "Refusing to use a physical volume or share root as InstallDir: $install"
    }

    foreach ($protected in $ProtectedRoots) {
        if ([string]::IsNullOrWhiteSpace($protected)) { continue }
        if (Test-FearVrPhysicalPathOverlap $install $protected) {
            throw (
                "InstallDir must not overlap a protected source or Retail " +
                "folder: '$install' and '$protected'.")
        }
    }
    return $install
}

function Read-FearVrOwnedDeployment(
    [string]$InstallDir,
    [switch]$AllowMissing,
    [switch]$AllowLegacyProxyless
) {
    $install = [IO.Path]::GetFullPath($InstallDir)
    $path = Join-Path $install 'deployment.json'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        if ($AllowMissing) { return $null }
        throw "No owned F.E.A.R. VR deployment manifest exists in '$install'."
    }
    try {
        $deployment = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
    } catch {
        throw "The deployment manifest is unreadable and will not be trusted: $path"
    }
    if ($null -eq $deployment) {
        throw "The deployment manifest is empty: $path"
    }

    $required = @(
        'packageVersion', 'retailRoot', 'publicToolsGame',
        'moduleDirectory', 'archiveConfig', 'userDirectory', 'logDirectory',
        'files')
    $names = @($deployment.PSObject.Properties.Name)
    foreach ($name in $required) {
        if ($names -notcontains $name) {
            throw "The deployment manifest lacks required ownership data: $name"
        }
    }
    if ($names -contains 'installDir' -and
        -not (Test-FearVrSamePath ([string]$deployment.installDir) $install)) {
        throw "The deployment manifest belongs to a different install folder: $path"
    }
    if (-not (Test-FearVrSamePath `
            ([string]$deployment.moduleDirectory) `
            (Join-Path $install 'game-modules')) -or
        -not (Test-FearVrSamePath `
            ([string]$deployment.archiveConfig) `
            (Join-Path $install 'fearvr.archcfg')) -or
        -not (Test-FearVrSamePath `
            ([string]$deployment.userDirectory) `
            (Join-Path $install 'userdata')) -or
        -not (Test-FearVrSamePath `
            ([string]$deployment.logDirectory) `
            (Join-Path $install 'logs'))) {
        throw "The deployment manifest contains unsafe stage paths: $path"
    }

    if ([string]::IsNullOrWhiteSpace([string]$deployment.packageVersion)) {
        throw "The deployment manifest lacks a package version: $path"
    }
    if ($names -contains 'd3d9Proxy') {
        $proxyNames = @($deployment.d3d9Proxy.PSObject.Properties.Name)
        $proxyHash = [string]$deployment.d3d9Proxy.sha256
        $physicalRoot = [string]$deployment.d3d9Proxy.physicalRetailRoot
        $retailRoot = [IO.Path]::GetFullPath([string]$deployment.retailRoot)
        if ($proxyNames -notcontains 'path' -or
            $proxyNames -notcontains 'physicalRetailRoot' -or
            $proxyNames -notcontains 'sha256' -or
            $proxyHash -notmatch '^[0-9A-Fa-f]{64}$' -or
            [string]::IsNullOrWhiteSpace($physicalRoot) -or
            -not (Test-FearVrSamePath `
                ([string]$deployment.d3d9Proxy.path) `
                (Join-Path $retailRoot 'd3d9.dll')) -or
            -not (Get-FearVrPhysicalPath $retailRoot).Equals(
                $physicalRoot, [StringComparison]::OrdinalIgnoreCase)) {
            throw "The deployment manifest contains invalid proxy ownership: $path"
        }
    } elseif (-not $AllowLegacyProxyless) {
        throw "The deployment manifest lacks proxy ownership data: $path"
    }

    $fileRecords = @($deployment.files)
    if ($fileRecords.Count -eq 0) {
        throw "The deployment manifest has no owned file records: $path"
    }
    $fileNamesSeen = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($file in $fileRecords) {
        $fileNames = @($file.PSObject.Properties.Name)
        $fileName = [string]$file.name
        if ($fileNames -notcontains 'name' -or
            $fileNames -notcontains 'sha256' -or
            [string]::IsNullOrWhiteSpace($fileName) -or
            [IO.Path]::GetFileName($fileName) -ne $fileName -or
            -not $fileNamesSeen.Add($fileName) -or
            [string]$file.sha256 -notmatch '^[0-9A-Fa-f]{64}$') {
            throw "The deployment manifest contains an invalid file record: $path"
        }
    }
    if ($names -contains 'generatedFiles') {
        $generatedSeen = [Collections.Generic.HashSet[string]]::new(
            [StringComparer]::OrdinalIgnoreCase)
        foreach ($file in @($deployment.generatedFiles)) {
            $recordNames = @($file.PSObject.Properties.Name)
            $relativePath = [string]$file.relativePath
            $fullGenerated = [IO.Path]::GetFullPath(
                (Join-Path (Join-Path $install 'game-modules') $relativePath))
            if ($recordNames -notcontains 'relativePath' -or
                $recordNames -notcontains 'sha256' -or
                [string]::IsNullOrWhiteSpace($relativePath) -or
                [IO.Path]::IsPathRooted($relativePath) -or
                -not $fullGenerated.StartsWith(
                    (Join-Path $install 'game-modules').TrimEnd('\') + '\',
                    [StringComparison]::OrdinalIgnoreCase) -or
                -not $generatedSeen.Add($relativePath) -or
                [string]$file.sha256 -notmatch '^[0-9A-Fa-f]{64}$') {
                throw "The deployment manifest contains an invalid generated file: $path"
            }
        }
    }
    return $deployment
}

function Assert-FearVrRecognizedInstallDirectory([string]$InstallDir) {
    $install = [IO.Path]::GetFullPath($InstallDir)
    if (-not (Test-Path -LiteralPath $install -PathType Container)) { return }
    $entries = @(Get-ChildItem -LiteralPath $install -Force)
    if ($entries.Count -eq 0) { return }
    $deploymentPath = Join-Path $install 'deployment.json'
    $transactionPath = Get-FearVrProxyTransactionPath $install
    $stageTransactionPath = Get-FearVrStageTransactionPath $install
    if (Test-Path -LiteralPath $deploymentPath -PathType Leaf) {
        Read-FearVrOwnedDeployment `
            $install -AllowLegacyProxyless | Out-Null
        return
    }
    if (Test-Path -LiteralPath $transactionPath -PathType Leaf) {
        Read-FearVrProxyTransaction $install | Out-Null
        return
    }
    if (Test-Path -LiteralPath $stageTransactionPath -PathType Leaf) {
        Read-FearVrStageTransaction $install | Out-Null
        return
    }
    throw (
        "Refusing to use non-empty, unrecognized InstallDir '$install'. " +
        'Choose an empty folder or the folder recorded by F.E.A.R. VR.')
}

function Test-FearVrTreeContainsReparsePoint([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        return $true
    }
    if (-not $item.PSIsContainer) { return $false }
    $pending = New-Object Collections.Generic.Stack[string]
    $pending.Push($item.FullName)
    while ($pending.Count -gt 0) {
        $directory = $pending.Pop()
        foreach ($child in Get-ChildItem -LiteralPath $directory -Force) {
            if (($child.Attributes -band
                    [IO.FileAttributes]::ReparsePoint) -ne 0) {
                return $true
            }
            if ($child.PSIsContainer) { $pending.Push($child.FullName) }
        }
    }
    return $false
}

function Remove-FearVrOwnedStagePath(
    [string]$InstallDir,
    [string]$Path,
    [switch]$Recurse
) {
    $install = [IO.Path]::GetFullPath($InstallDir).TrimEnd('\')
    $target = [IO.Path]::GetFullPath($Path)
    if (-not $target.StartsWith(
            $install + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a path outside InstallDir: $target"
    }
    if (-not (Test-Path -LiteralPath $target)) { return }
    if (-not (Get-FearVrPhysicalPath $target).StartsWith(
            (Get-FearVrPhysicalPath $install).TrimEnd('\') + '\',
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a path that escapes InstallDir: $target"
    }
    if (Test-FearVrTreeContainsReparsePoint $target) {
        throw "Refusing to recursively remove a tree containing a reparse point: $target"
    }
    $item = Get-Item -LiteralPath $target -Force
    if ($item.PSIsContainer -and -not $Recurse) {
        throw "Recursive removal was not authorized for directory: $target"
    }
    Remove-Item -LiteralPath $target -Force -Recurse:$Recurse
}

function Copy-FearVrFileDurably(
    [string]$SourcePath,
    [string]$DestinationPath
) {
    $source = [IO.Path]::GetFullPath($SourcePath)
    $destination = [IO.Path]::GetFullPath($DestinationPath)
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Source file is missing: $source"
    }
    if (Test-Path -LiteralPath $destination) {
        throw "Durable copy destination already exists: $destination"
    }

    $input = $null
    $output = $null
    $createdDestination = $false
    try {
        $input = [IO.File]::Open(
            $source, [IO.FileMode]::Open, [IO.FileAccess]::Read,
            [IO.FileShare]::Read)
        $output = [IO.FileStream]::new(
            $destination, [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write, [IO.FileShare]::None,
            1048576, [IO.FileOptions]::WriteThrough)
        $createdDestination = $true
        $input.CopyTo($output)
        $output.Flush($true)
    } catch {
        if ($output) { $output.Dispose(); $output = $null }
        if ($input) { $input.Dispose(); $input = $null }
        if ($createdDestination) {
            Remove-Item -LiteralPath $destination -Force `
                -ErrorAction SilentlyContinue
        }
        throw
    } finally {
        if ($output) { $output.Dispose() }
        if ($input) { $input.Dispose() }
    }
}

function Write-FearVrJsonFileDurably(
    [string]$Path,
    $Value,
    [int]$Depth = 8
) {
    $full = [IO.Path]::GetFullPath($Path)
    if (Test-Path -LiteralPath $full) {
        throw "Durable JSON destination already exists: $full"
    }
    $json = ($Value | ConvertTo-Json -Depth $Depth) +
        [Environment]::NewLine
    $encoding = [Text.UTF8Encoding]::new($false)
    $bytes = $encoding.GetBytes($json)
    $stream = $null
    $createdDestination = $false
    try {
        $stream = [IO.FileStream]::new(
            $full, [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write, [IO.FileShare]::None,
            65536, [IO.FileOptions]::WriteThrough)
        $createdDestination = $true
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    } catch {
        if ($stream) { $stream.Dispose(); $stream = $null }
        if ($createdDestination) {
            Remove-Item -LiteralPath $full -Force `
                -ErrorAction SilentlyContinue
        }
        throw
    } finally {
        if ($stream) { $stream.Dispose() }
    }
}

function Set-FearVrFileAtomically(
    [string]$SourcePath,
    [string]$TargetPath,
    [string]$TemporaryPath
) {
    $source = [IO.Path]::GetFullPath($SourcePath)
    $target = [IO.Path]::GetFullPath($TargetPath)
    $temporary = [IO.Path]::GetFullPath($TemporaryPath)
    if (-not (Test-FearVrSamePath `
            ([IO.Path]::GetDirectoryName($target)) `
            ([IO.Path]::GetDirectoryName($temporary)))) {
        throw 'Atomic replacement temporary file must share the target directory.'
    }
    if (Test-Path -LiteralPath $temporary) {
        throw "Atomic replacement temporary file already exists: $temporary"
    }

    Copy-FearVrFileDurably $source $temporary
    $sourceHash = Get-FileSha256 $source
    if ((Get-FileSha256 $temporary) -ne $sourceHash) {
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
        throw "Atomic replacement staging verification failed: $temporary"
    }
    try {
        if (Test-Path -LiteralPath $target -PathType Leaf) {
            [IO.File]::Replace($temporary, $target, $null, $true)
        } else {
            [IO.File]::Move($temporary, $target)
        }
    } finally {
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    }
    if ((Get-FileSha256 $target) -ne $sourceHash) {
        throw "Atomic replacement verification failed: $target"
    }
}

function Write-FearVrJsonAtomically(
    [string]$Path,
    $Value,
    [int]$Depth = 8
) {
    $full = [IO.Path]::GetFullPath($Path)
    $directory = [IO.Path]::GetDirectoryName($full)
    if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
    }
    $temporary = Join-Path $directory (
        '.' + [IO.Path]::GetFileName($full) + '.' +
        [Guid]::NewGuid().ToString('N') + '.tmp')
    Write-FearVrJsonFileDurably $temporary $Value $Depth
    try {
        if (Test-Path -LiteralPath $full -PathType Leaf) {
            [IO.File]::Replace($temporary, $full, $null, $true)
        } else {
            [IO.File]::Move($temporary, $full)
        }
    } finally {
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    }
}

function Get-FearVrAppLocalProxyState(
    [string]$RetailRoot,
    $Record
) {
    $target = [IO.Path]::GetFullPath((Join-Path $RetailRoot 'd3d9.dll'))
    if ($null -eq $Record) {
        return [pscustomobject]@{
            Status = 'NoRecord'
            Path = $target
            Sha256 = Get-FileSha256 $target
        }
    }

    $recordPath = [string]$Record.path
    $recordHash = [string]$Record.sha256
    $recordPhysicalRoot = [string]$Record.physicalRetailRoot
    if ([string]::IsNullOrWhiteSpace($recordPath) -or
        $recordHash -notmatch '^[0-9A-Fa-f]{64}$' -or
        [string]::IsNullOrWhiteSpace($recordPhysicalRoot)) {
        return [pscustomobject]@{
            Status = 'InvalidRecord'
            Path = $target
            Sha256 = Get-FileSha256 $target
        }
    }

    if (-not (Test-FearVrSamePath $recordPath $target) -or
        -not (Get-FearVrPhysicalPath $RetailRoot).Equals(
            $recordPhysicalRoot,
            [StringComparison]::OrdinalIgnoreCase)) {
        return [pscustomobject]@{
            Status = 'InvalidRecord'
            Path = $target
            Sha256 = Get-FileSha256 $target
        }
    }

    $currentHash = Get-FileSha256 $target
    if (-not $currentHash) {
        return [pscustomobject]@{
            Status = 'Missing'
            Path = $target
            Sha256 = $null
        }
    }
    return [pscustomobject]@{
        Status = if ($currentHash -eq $recordHash) { 'Owned' } else { 'Modified' }
        Path = $target
        Sha256 = $currentHash
    }
}

function Get-FearVrAppLocalProxyInstallPlan(
    [string]$SourcePath,
    [string]$RetailRoot,
    $PreviousRecord
) {
    $source = [IO.Path]::GetFullPath($SourcePath)
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "F.E.A.R. VR proxy is missing: $source"
    }
    if (-not (Test-Path -LiteralPath $RetailRoot -PathType Container)) {
        throw "Retail folder is missing: $RetailRoot"
    }

    $root = [IO.Path]::GetFullPath($RetailRoot)
    $physicalRoot = Get-FearVrPhysicalPath $root
    $target = [IO.Path]::GetFullPath((Join-Path $root 'd3d9.dll'))
    $sourceHash = Get-FileSha256 $source
    $currentHash = Get-FileSha256 $target
    if ($currentHash -and $currentHash -ne $sourceHash) {
        $previousState =
            Get-FearVrAppLocalProxyState $root $PreviousRecord
        if ($previousState.Status -ne 'Owned') {
            throw @"
Refusing to overwrite an existing app-local Direct3D wrapper:
  $target

Its SHA-256 is not owned by this F.E.A.R. VR installation. It may belong to
ReShade, DXVK, another mod, or a manual backup. Remove or chain that wrapper
explicitly before installing F.E.A.R. VR.
"@
        }
    }

    return [pscustomobject]@{
        SourcePath = $source
        RetailRoot = $root
        TargetPath = $target
        PhysicalRetailRoot = $physicalRoot
        SourceSha256 = $sourceHash
        PriorSha256 = $currentHash
        TargetWasPresent = [bool]$currentHash
        NeedsInstall = $currentHash -ne $sourceHash
        Record = [ordered]@{
            path = $target
            physicalRetailRoot = $physicalRoot
            sha256 = $sourceHash
            bytes = (Get-Item -LiteralPath $source).Length
            owner = 'fearvr'
        }
    }
}

function Install-FearVrAppLocalProxy(
    $Plan,
    [string]$TemporaryPath
) {
    if (-not (Get-FearVrPhysicalPath ([string]$Plan.RetailRoot)).Equals(
            [string]$Plan.PhysicalRetailRoot,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The Retail folder physical identity changed during installation.'
    }
    if ((Get-FileSha256 ([string]$Plan.SourcePath)) -ne
        [string]$Plan.SourceSha256) {
        throw 'The staged app-local proxy changed after validation.'
    }
    $currentHash = Get-FileSha256 ([string]$Plan.TargetPath)
    if ($currentHash -ne $Plan.PriorSha256) {
        throw 'The app-local d3d9.dll changed while the update was prepared.'
    }
    if ([bool]$Plan.NeedsInstall) {
        Set-FearVrFileAtomically `
            -SourcePath ([string]$Plan.SourcePath) `
            -TargetPath ([string]$Plan.TargetPath) `
            -TemporaryPath $TemporaryPath
    }
    if ((Get-FileSha256 ([string]$Plan.TargetPath)) -ne
        [string]$Plan.SourceSha256) {
        throw "App-local proxy verification failed: $($Plan.TargetPath)"
    }
    return $Plan.Record
}

function Remove-FearVrAppLocalProxy(
    [string]$RetailRoot,
    $Record,
    [switch]$Apply
) {
    $state = Get-FearVrAppLocalProxyState $RetailRoot $Record
    if ($state.Status -ne 'Owned') { return $state }
    if (-not $Apply) {
        return [pscustomobject]@{
            Status = 'WouldRemove'
            Path = $state.Path
            Sha256 = $state.Sha256
        }
    }
    $result = Remove-FearVrFileByOwnedHash `
        -Path $state.Path `
        -ExpectedSha256 ([string]$Record.sha256) `
        -ExpectedPhysicalParent ([string]$Record.physicalRetailRoot)
    return [pscustomobject]@{
        Status = $result
        Path = $state.Path
        Sha256 = [string]$Record.sha256
    }
}

function Get-FearVrProxyTransactionPath([string]$InstallDir) {
    return Join-Path ([IO.Path]::GetFullPath($InstallDir)) `
        '.fearvr-proxy-transaction.json'
}

function Get-FearVrStageTransactionPath([string]$InstallDir) {
    return Join-Path ([IO.Path]::GetFullPath($InstallDir)) `
        '.fearvr-stage-transaction.json'
}

function Read-FearVrStageTransaction([string]$InstallDir) {
    $install = [IO.Path]::GetFullPath($InstallDir)
    $path = Get-FearVrStageTransactionPath $install
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    try {
        $marker = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
    } catch {
        throw "The prepared-stage marker is unreadable: $path"
    }
    $names = @($marker.PSObject.Properties.Name)
    foreach ($name in @(
        'schemaVersion', 'transactionId', 'installDir',
        'physicalInstallDir')) {
        if ($names -notcontains $name) {
            throw "The prepared-stage marker lacks '$name': $path"
        }
    }
    if (-not ($marker.schemaVersion -is [int]) -or
        [int]$marker.schemaVersion -ne 1 -or
        [string]$marker.transactionId -notmatch '^[0-9A-Fa-f]{32}$' -or
        -not (Test-FearVrSamePath ([string]$marker.installDir) $install) -or
        -not (Get-FearVrPhysicalPath $install).Equals(
            [string]$marker.physicalInstallDir,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "The prepared-stage marker is invalid: $path"
    }
    return $marker
}

# The journal uses only deterministic paths derived from its transaction ID.
# Validate every one before recovery can copy or remove a file.
function Read-FearVrProxyTransaction([string]$InstallDir) {
    $install = [IO.Path]::GetFullPath($InstallDir)
    $journalPath = Get-FearVrProxyTransactionPath $install
    if (-not (Test-Path -LiteralPath $journalPath -PathType Leaf)) {
        return $null
    }
    try {
        $journal = Get-Content -Raw -LiteralPath $journalPath |
            ConvertFrom-Json
    } catch {
        throw "Proxy transaction journal is unreadable: $journalPath"
    }

    if ($null -eq $journal -or $null -eq $journal.target -or
        $null -eq $journal.manifest -or
        $null -eq $journal.newProxyRecord) {
        throw "Proxy transaction journal is incomplete: $journalPath"
    }
    $requiredRoot = @(
        'schemaVersion', 'transactionId', 'phase', 'installDir',
        'target', 'manifest', 'newProxyRecord')
    $requiredTarget = @(
        'retailRoot', 'physicalRetailRoot', 'path',
        'wasPresent', 'priorSha256',
        'installedSha256', 'backupPath', 'temporaryPath')
    $requiredManifest = @(
        'path', 'previousExisted', 'previousSha256',
        'previousBackupPath', 'newPath', 'temporaryPath')
    $requiredRecord = @('path', 'physicalRetailRoot', 'sha256')
    $validationSets = @(
        [pscustomobject]@{ Object = $journal; Names = $requiredRoot },
        [pscustomobject]@{ Object = $journal.target; Names = $requiredTarget },
        [pscustomobject]@{ Object = $journal.manifest; Names = $requiredManifest },
        [pscustomobject]@{ Object = $journal.newProxyRecord; Names = $requiredRecord })
    foreach ($requiredSet in $validationSets) {
        $object = $requiredSet.Object
        $names = @($object.PSObject.Properties.Name)
        foreach ($name in @($requiredSet.Names)) {
            if ($names -notcontains $name) {
                throw "Proxy transaction journal lacks required field '$name': $journalPath"
            }
        }
    }
    if (-not ($journal.target.wasPresent -is [bool]) -or
        -not ($journal.manifest.previousExisted -is [bool])) {
        throw "Proxy transaction journal contains invalid Boolean fields: $journalPath"
    }

    $id = [string]$journal.transactionId
    if ([int]$journal.schemaVersion -ne 1 -or
        $id -notmatch '^[0-9A-Fa-f]{32}$' -or
        [string]$journal.phase -notin @('Prepared', 'ManifestCommitted') -or
        -not (Test-FearVrSamePath ([string]$journal.installDir) $install)) {
        throw "Proxy transaction journal is invalid: $journalPath"
    }
    $retailRoot = [IO.Path]::GetFullPath([string]$journal.target.retailRoot)
    $physicalRetailRoot = Get-FearVrPhysicalPath $retailRoot
    if (-not $physicalRetailRoot.Equals(
            [string]$journal.target.physicalRetailRoot,
            [StringComparison]::OrdinalIgnoreCase) -or
        -not $physicalRetailRoot.Equals(
            [string]$journal.newProxyRecord.physicalRetailRoot,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Proxy transaction Retail identity changed: $journalPath"
    }
    $expectedTarget = Join-Path $retailRoot 'd3d9.dll'
    $expectedProxyTemp = Join-Path $retailRoot ".fearvr-d3d9-$id.tmp"
    $expectedManifest = Join-Path $install 'deployment.json'
    $expectedProxyBackup = Join-Path $install ".fearvr-d3d9-$id.rollback"
    $expectedOldManifest =
        Join-Path $install ".fearvr-deployment-$id.rollback.json"
    $expectedNewManifest =
        Join-Path $install ".fearvr-deployment-$id.new.json"
    $expectedManifestTemp =
        Join-Path $install ".fearvr-deployment-$id.tmp"
    if (-not (Test-FearVrSamePath `
            ([string]$journal.target.path) $expectedTarget) -or
        -not (Test-FearVrSamePath `
            ([string]$journal.target.temporaryPath) $expectedProxyTemp) -or
        -not (Test-FearVrSamePath `
            ([string]$journal.target.backupPath) $expectedProxyBackup) -or
        -not (Test-FearVrSamePath `
            ([string]$journal.manifest.path) $expectedManifest) -or
        -not (Test-FearVrSamePath `
            ([string]$journal.manifest.previousBackupPath) `
            $expectedOldManifest) -or
        -not (Test-FearVrSamePath `
            ([string]$journal.manifest.newPath) $expectedNewManifest) -or
        -not (Test-FearVrSamePath `
            ([string]$journal.manifest.temporaryPath) `
            $expectedManifestTemp) -or
        -not (Test-FearVrSamePath `
            ([string]$journal.newProxyRecord.path) $expectedTarget) -or
        [string]$journal.newProxyRecord.sha256 -ne
            [string]$journal.target.installedSha256) {
        throw "Proxy transaction journal contains unsafe paths: $journalPath"
    }
    $wasPresent = $journal.target.wasPresent
    $priorHash = [string]$journal.target.priorSha256
    $previousExisted = $journal.manifest.previousExisted
    $previousHash = [string]$journal.manifest.previousSha256
    if ([string]$journal.target.installedSha256 -notmatch
            '^[0-9A-Fa-f]{64}$' -or
        [string]$journal.newProxyRecord.sha256 -notmatch
            '^[0-9A-Fa-f]{64}$' -or
        ($wasPresent -and $priorHash -notmatch
            '^[0-9A-Fa-f]{64}$') -or
        (-not $wasPresent -and
            -not [string]::IsNullOrWhiteSpace($priorHash)) -or
        ($previousExisted -and $previousHash -notmatch
            '^[0-9A-Fa-f]{64}$') -or
        (-not $previousExisted -and
            -not [string]::IsNullOrWhiteSpace($previousHash))) {
        throw "Proxy transaction journal contains invalid hashes: $journalPath"
    }
    return $journal
}

# deployment.json is the commit marker. Its transaction ID distinguishes an
# atomic manifest commit from an interruption that still needs rollback.
function Test-FearVrProxyTransactionCommitted($Journal, $Deployment) {
    if ($null -eq $Deployment) { return $false }
    $names = $Deployment.PSObject.Properties.Name
    if ($names -notcontains 'proxyTransactionId' -or
        $names -notcontains 'd3d9Proxy') {
        return $false
    }
    return (
        [string]$Deployment.proxyTransactionId -eq
            [string]$Journal.transactionId) -and
        (Test-FearVrSamePath `
            ([string]$Deployment.d3d9Proxy.path) `
            ([string]$Journal.newProxyRecord.path)) -and
        ([string]$Deployment.d3d9Proxy.sha256 -eq
            [string]$Journal.newProxyRecord.sha256)
}

# A Retail-root move is committed before the old proxy is removed. Keeping the
# cleanup record in deployment.json makes an interrupted removal retryable.
function Invoke-FearVrPendingProxyCleanup(
    [string]$DeploymentPath,
    $Deployment,
    [switch]$Apply
) {
    if ($null -eq $Deployment -or
        $Deployment.PSObject.Properties.Name -notcontains
            'pendingD3d9ProxyCleanup' -or
        $null -eq $Deployment.pendingD3d9ProxyCleanup) {
        return [pscustomobject]@{ Status = 'None'; Path = $null }
    }

    $pending = $Deployment.pendingD3d9ProxyCleanup
    $pendingNames = @($pending.PSObject.Properties.Name)
    $deploymentNames = @($Deployment.PSObject.Properties.Name)
    if ($pendingNames -notcontains 'retailRoot' -or
        $pendingNames -notcontains 'record' -or
        $deploymentNames -notcontains 'retailRoot' -or
        $deploymentNames -notcontains 'd3d9Proxy' -or
        $null -eq $pending.record -or
        $null -eq $Deployment.d3d9Proxy) {
        throw 'The pending app-local proxy cleanup record is incomplete.'
    }

    $pendingTarget = Join-Path ([string]$pending.retailRoot) 'd3d9.dll'
    $currentTarget = Join-Path ([string]$Deployment.retailRoot) 'd3d9.dll'
    if ($pendingNames -notcontains 'physicalRetailRoot') {
        return [pscustomobject]@{
            Status = 'Deferred'
            ProxyStatus = 'PhysicalIdentityUnverified'
            Path = $pendingTarget
        }
    }
    $expectedPhysicalRoot = [string]$pending.physicalRetailRoot
    $actualPhysicalRoot = Get-FearVrPhysicalPath ([string]$pending.retailRoot)
    if ([string]::IsNullOrWhiteSpace($expectedPhysicalRoot) -or
        -not $actualPhysicalRoot.Equals(
            $expectedPhysicalRoot,
            [StringComparison]::OrdinalIgnoreCase)) {
        return [pscustomobject]@{
            Status = 'Deferred'
            ProxyStatus = 'PhysicalIdentityChanged'
            Path = $pendingTarget
        }
    }
    $samePhysicalTarget =
        (Test-FearVrSamePhysicalPath `
            ([string]$pending.retailRoot) `
            ([string]$Deployment.retailRoot)) -or
        (Test-FearVrSamePhysicalPath $pendingTarget $currentTarget)
    $state = if ($samePhysicalTarget) {
        [pscustomobject]@{
            Status = 'CurrentTarget'
            Path = $pendingTarget
            Sha256 = Get-FileSha256 $pendingTarget
        }
    } else {
        Remove-FearVrAppLocalProxy `
            -RetailRoot ([string]$pending.retailRoot) `
            -Record $pending.record
    }
    if (-not $Apply) {
        return [pscustomobject]@{
            Status = 'WouldResolve'
            ProxyStatus = $state.Status
            Path = $state.Path
        }
    }

    $resolved = $state
    if ($state.Status -eq 'WouldRemove') {
        $resolved = Remove-FearVrAppLocalProxy `
            -RetailRoot ([string]$pending.retailRoot) `
            -Record $pending.record `
            -Apply
    }
    if ($resolved.Status -notin @(
            'Removed', 'Missing', 'Modified', 'InvalidRecord', 'NoRecord',
            'CurrentTarget')) {
        throw "Unexpected pending proxy cleanup state: $($resolved.Status)"
    }

    $Deployment.PSObject.Properties.Remove('pendingD3d9ProxyCleanup')
    Write-FearVrJsonAtomically $DeploymentPath $Deployment 8
    $verified = Get-Content -Raw -LiteralPath $DeploymentPath |
        ConvertFrom-Json
    if ($verified.PSObject.Properties.Name -contains
        'pendingD3d9ProxyCleanup') {
        throw 'The pending app-local proxy cleanup record did not clear.'
    }
    return [pscustomobject]@{
        Status = 'Resolved'
        ProxyStatus = $resolved.Status
        Path = $resolved.Path
    }
}

function Remove-FearVrProxyTransactionArtifacts(
    [string]$InstallDir,
    $Journal
) {
    foreach ($path in @(
        [string]$Journal.target.temporaryPath,
        [string]$Journal.target.backupPath,
        [string]$Journal.manifest.previousBackupPath,
        [string]$Journal.manifest.newPath,
        [string]$Journal.manifest.temporaryPath
    )) {
        if (-not [string]::IsNullOrWhiteSpace($path) -and
            (Test-Path -LiteralPath $path)) {
            Remove-Item -LiteralPath $path -Force
        }
    }
    $journalPath = Get-FearVrProxyTransactionPath $InstallDir
    if (Test-Path -LiteralPath $journalPath) {
        Remove-Item -LiteralPath $journalPath -Force
    }
}

function Resolve-FearVrProxyTransaction(
    [string]$InstallDir,
    [switch]$Apply
) {
    $install = [IO.Path]::GetFullPath($InstallDir)
    $journal = Read-FearVrProxyTransaction $install
    if ($null -eq $journal) {
        return [pscustomobject]@{ Status = 'None'; Path = $null }
    }
    Assert-FearVrSafeInstallDirectory `
        -InstallDir $install `
        -ProtectedRoots @([string]$journal.target.retailRoot) | Out-Null

    $deployment = $null
    $deploymentPath = [string]$journal.manifest.path
    if (Test-Path -LiteralPath $deploymentPath -PathType Leaf) {
        try {
            $deployment = Get-Content -Raw -LiteralPath $deploymentPath |
                ConvertFrom-Json
        } catch {
            $deployment = $null
        }
    }
    $committed = Test-FearVrProxyTransactionCommitted $journal $deployment
    if (-not $Apply) {
        return [pscustomobject]@{
            Status = if ($committed) {
                'WouldFinalizeCommitted'
            } else {
                'WouldRollback'
            }
            Path = Get-FearVrProxyTransactionPath $install
        }
    }

    if ($committed) {
        $deployment = Read-FearVrOwnedDeployment $install
        # Ownership is durable. Finish only deferred cleanup and journal
        # removal; never roll back a proxy recorded by the committed manifest.
        if ($deployment.PSObject.Properties.Name -contains
            'pendingD3d9ProxyCleanup') {
            Invoke-FearVrPendingProxyCleanup `
                -DeploymentPath $deploymentPath `
                -Deployment $deployment `
                -Apply | Out-Null
        }
        Remove-FearVrProxyTransactionArtifacts $install $journal
        return [pscustomobject]@{
            Status = 'FinalizedCommitted'
            Path = $deploymentPath
        }
    }

    # The old manifest is still authoritative, so restore its proxy state.
    $currentManifestHash = Get-FileSha256 $deploymentPath
    $previousManifestExpected = [bool]$journal.manifest.previousExisted
    $previousManifestHash = [string]$journal.manifest.previousSha256
    if ($previousManifestExpected) {
        if ($currentManifestHash -and
            $currentManifestHash -ne $previousManifestHash) {
            throw 'The deployment manifest conflicts with an unfinished proxy transaction.'
        }
    } elseif ($currentManifestHash) {
        throw 'An unexpected deployment manifest conflicts with the proxy transaction.'
    }

    $proxyTemp = [string]$journal.target.temporaryPath
    if (Test-Path -LiteralPath $proxyTemp) {
        Remove-Item -LiteralPath $proxyTemp -Force
    }
    $target = [string]$journal.target.path
    if (-not (Get-FearVrPhysicalPath `
            ([string]$journal.target.retailRoot)).Equals(
            [string]$journal.target.physicalRetailRoot,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The Retail folder physical identity changed before rollback.'
    }
    $currentProxyHash = Get-FileSha256 $target
    $priorProxyHash = [string]$journal.target.priorSha256
    $installedProxyHash = [string]$journal.target.installedSha256
    if ([bool]$journal.target.wasPresent) {
        if ($currentProxyHash -eq $priorProxyHash) {
            # The previous proxy is already authoritative.
        } elseif ($currentProxyHash -eq $installedProxyHash -or
            -not $currentProxyHash) {
            $backup = [string]$journal.target.backupPath
            if ((Get-FileSha256 $backup) -ne $priorProxyHash) {
                throw 'The prior app-local proxy backup is unavailable for rollback.'
            }
            Set-FearVrFileAtomically `
                -SourcePath $backup `
                -TargetPath $target `
                -TemporaryPath $proxyTemp
        } else {
            throw 'The app-local proxy changed during rollback; recovery data was kept.'
        }
    } elseif ($currentProxyHash) {
        if ($currentProxyHash -ne $installedProxyHash) {
            throw 'A foreign app-local proxy appeared during rollback; it was kept.'
        }
        $removal = Remove-FearVrFileByOwnedHash `
            -Path $target `
            -ExpectedSha256 $installedProxyHash `
            -ExpectedPhysicalParent `
                ([string]$journal.target.physicalRetailRoot)
        if ($removal -notin @('Removed', 'Missing')) {
            throw "The installed proxy could not be rolled back safely: $removal"
        }
    }

    if ($previousManifestExpected -and -not $currentManifestHash) {
        $manifestBackup = [string]$journal.manifest.previousBackupPath
        if ((Get-FileSha256 $manifestBackup) -ne $previousManifestHash) {
            throw 'The prior deployment manifest backup is unavailable for rollback.'
        }
        $manifestTemp = [string]$journal.manifest.temporaryPath
        if (Test-Path -LiteralPath $manifestTemp) {
            Remove-Item -LiteralPath $manifestTemp -Force
        }
        Set-FearVrFileAtomically `
            -SourcePath $manifestBackup `
            -TargetPath $deploymentPath `
            -TemporaryPath $manifestTemp
    }
    Remove-FearVrProxyTransactionArtifacts $install $journal
    return [pscustomobject]@{
        Status = 'RolledBack'
        Path = $deploymentPath
    }
}

# Verifies a Retail FEAR.exe. Version 1.08 is required because the Public
# Tools modules match no other version. The hash determines only whether this
# build has been tested: Steam, GOG, and retail-disc releases have different
# 1.08 executables, while this mod's byte signatures all live in GameOrig.dll.
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

# Installing or removing the confirmed HDTextures4FEAR patch later may keep
# using an existing VR installation. Unknown hash changes remain an integrity
# error.
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

# Scan every fixed local drive so installations on D:\ or E:\ are found just
# like those on C:\. Network drives are excluded because the search can stall.
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

# Read installation paths from the uninstall registry. This covers GOG, the
# original retail disc, and custom locations without scanning the filesystem.
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

# Finds the Retail installation: Steam libraries first, then uninstall
# registry entries (GOG and disc), followed by common folder names on every
# fixed drive. A known hash always wins; otherwise return the first FEAR.exe
# folder so Assert-RetailFearExe can report the exact mismatch.
function Find-RetailRoot {
    $cfg = Get-FearVrReleaseConfig
    $candidates = New-Object Collections.Generic.List[string]
    $steamRoot = $null
    foreach ($key in @(
        'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam',
        'HKLM:\SOFTWARE\Valve\Steam',
        'HKCU:\SOFTWARE\Valve\Steam'
    )) {
        try {
            $value = (Get-ItemProperty $key -ErrorAction Stop)
            foreach ($name in @('InstallPath', 'SteamPath')) {
                if ($value.PSObject.Properties.Name -contains $name -and
                    $value.$name) {
                    $steamRoot = $value.$name
                    break
                }
            }
        } catch { }
        if ($steamRoot) { break }
    }
    if (-not $steamRoot) { $steamRoot = 'C:\Program Files (x86)\Steam' }

    $libraries = New-Object Collections.Generic.List[string]
    $libraries.Add($steamRoot)
    $vdf = Join-Path $steamRoot 'steamapps\libraryfolders.vdf'
    if (Test-Path -LiteralPath $vdf -PathType Leaf) {
        foreach ($match in [Text.RegularExpressions.Regex]::Matches(
            [IO.File]::ReadAllText($vdf), '"path"\s*"([^"]+)"')) {
            # Parentheses are required because a method call would otherwise
            # parse the comma in -replace as an argument separator.
            $libraries.Add(($match.Groups[1].Value -replace '\\\\', '\'))
        }
    }
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

    # GOG, retail-disc installs, and anything registered conventionally.
    foreach ($location in (Get-RegistryInstallLocations '*F.E.A.R*')) {
        $candidates.Add($location)
    }
    foreach ($location in (Get-RegistryInstallLocations '*FEAR*')) {
        $candidates.Add($location)
    }

    # Common folder names on every fixed drive.
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

    # Prefer a tested build, then any other 1.08 executable (GOG or retail
    # disc), and finally any FEAR.exe so Assert-RetailFearExe can explain why
    # it is rejected.
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

# Selects how the game starts. Steam requires steam.exe -applaunch because
# its FEAR.exe exits without the client. GOG and retail-disc installs start
# directly with the same arguments.
function Get-RetailLaunchMode([string]$RetailRoot) {
    if ($RetailRoot -match '(?i)\\steamapps\\common\\' -and
        (Get-SteamExecutable)) {
        return 'steam'
    }
    return 'direct'
}

function Get-SteamExecutable {
    foreach ($key in @(
        'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam',
        'HKLM:\SOFTWARE\Valve\Steam',
        'HKCU:\SOFTWARE\Valve\Steam'
    )) {
        try { $value = Get-ItemProperty $key -ErrorAction Stop } catch { continue }
        foreach ($name in @('InstallPath', 'SteamPath')) {
            if ($value.PSObject.Properties.Name -contains $name -and $value.$name) {
                $exe = Join-Path $value.$name 'steam.exe'
                if (Test-Path -LiteralPath $exe -PathType Leaf) { return $exe }
            }
        }
    }
    $default = Join-Path ${env:ProgramFiles(x86)} 'Steam\steam.exe'
    if (Test-Path -LiteralPath $default -PathType Leaf) { return $default }
    return $null
}

# Finds a Public Tools runtime directory and verifies the hash of its stock
# VC7.1 GameClient.dll. Because the Public Tools installer permits arbitrary
# destinations, search the registry and common per-drive folders too.
function Find-PublicToolsGame {
    $roots = New-Object Collections.Generic.List[string]
    foreach ($location in (Get-RegistryInstallLocations '*Public Tools*')) {
        $roots.Add($location)
    }
    $folders = @(
        'Monolith Productions\FEAR Public Tools',
        'FEAR Public Tools',
        'F.E.A.R. Public Tools'
    )
    $parents = @('Program Files (x86)', 'Program Files', 'Games', '')
    foreach ($drive in (Get-LocalDriveRoots)) {
        foreach ($parent in $parents) {
            foreach ($folder in $folders) {
                $roots.Add([IO.Path]::Combine($drive + '\', $parent, $folder))
            }
        }
    }
    foreach ($root in $roots) {
        if ([string]::IsNullOrWhiteSpace($root)) { continue }
        # A user-provided path may point at either the installation root or
        # Dev\Runtime\Game directly.
        foreach ($suffix in @('Dev\Runtime\Game', '')) {
            $game = if ($suffix) { Join-Path $root $suffix } else { $root }
            if (Test-PublicToolsGame $game) { return $game }
        }
    }
    return $null
}

# Accepts either an installation root or runtime directory and returns the
# verified Dev\Runtime\Game path.
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

# --- OpenXR runtime ----------------------------------------------------------
# XR_RUNTIME_JSON overrides the runtime only for the host process. The
# system-wide HKLM\...\Khronos\OpenXR\1\ActiveRuntime value is never written.
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
    if ([string]::IsNullOrWhiteSpace($Runtime) -or $Runtime -eq 'active') {
        $path = $null
        try {
            $path = (Get-ItemProperty 'HKLM:\SOFTWARE\Khronos\OpenXR\1' `
                -ErrorAction Stop).ActiveRuntime
        } catch { }
        if ([string]::IsNullOrWhiteSpace($path)) {
            throw @'
Keine aktive OpenXR-Runtime gefunden.
SteamVR oder den Virtual Desktop Streamer starten und dort als OpenXR-Runtime
setzen, oder mit -Runtime steamvr bzw. -Runtime vdxr starten.
'@
        }
        return [pscustomobject]@{
            Path = $null; Name = Get-OpenXrRuntimeName $path
            Kind = Get-OpenXrRuntimeKind $path; Override = $false
        }
    }
    $manifest = switch ($Runtime) {
        'steamvr' { $cfg.SteamVrManifest }
        'vdxr'    { $cfg.VdxrManifest }
        default   { $Runtime }
    }
    if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) {
        throw "OpenXR-Runtime-Manifest nicht gefunden: $manifest"
    }
    return [pscustomobject]@{
        Path = [IO.Path]::GetFullPath($manifest)
        Name = Get-OpenXrRuntimeName $manifest
        Kind = Get-OpenXrRuntimeKind $manifest
        Override = $true
    }
}
