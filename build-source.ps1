# SPDX-License-Identifier: GPL-3.0-only
# Licensed under the GNU General Public License, version 3 only.
# Provided without warranty. See LICENSE for the license text.

[CmdletBinding()]
param(
    [ValidateSet('wdi', 'wificx', 'diagnostic', 'fn', 'bt')]
    [string]$Driver = 'wdi',
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,
    [switch]$HostCiphers,
    [switch]$PacketTrace,
    [switch]$PackageFirmware,
    [string]$FirmwareDirectory = '',
    [string]$VersionCacheDirectory = '',
    [version]$Version,
    [string]$WdkPackageDirectory = ''
)

$ErrorActionPreference = 'Stop'
$sourceRoot = [IO.Path]::GetFullPath($PSScriptRoot)
$explicitVersion = $PSBoundParameters.ContainsKey('Version')
if ($explicitVersion -and ($null -eq $Version -or $Version.Major -gt 65535 -or $Version.Minor -gt 65535 -or
    $Version.Build -lt 0 -or $Version.Build -gt 65535 -or
    $Version.Revision -lt 0 -or $Version.Revision -gt 65535)) {
    throw 'Version must contain four components between 0 and 65535.'
}
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
if ($outputPath.Equals($sourceRoot, [StringComparison]::OrdinalIgnoreCase) -or
    $outputPath.StartsWith($sourceRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Choose an output directory outside the source folder.'
}
if ([string]::IsNullOrWhiteSpace($VersionCacheDirectory)) {
    $VersionCacheDirectory = Join-Path $env:LOCALAPPDATA 'AP6256\SourceVersions'
}
$versionCachePath = [IO.Path]::GetFullPath($VersionCacheDirectory)
if ($versionCachePath.Equals($sourceRoot, [StringComparison]::OrdinalIgnoreCase) -or
    $versionCachePath.StartsWith($sourceRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Choose a version cache outside the source folder.'
}

function Get-SourceFingerprint {
    $sourceEntries = @()
    foreach ($relativeDirectory in @('src', 'inf', 'bt\src', 'bt\inf', 'bt\props', 'fn\src', 'fn\inf', 'fn\props')) {
        $sourceEntries += @(Get-ChildItem -LiteralPath (Join-Path $sourceRoot $relativeDirectory) -File -Recurse)
    }
    foreach ($relativeDirectory in @('build', 'bt\build', 'fn\build')) {
        $sourceEntries += @(Get-ChildItem -LiteralPath (Join-Path $sourceRoot $relativeDirectory) -File)
    }
    $sourceEntries += @(Get-ChildItem -LiteralPath $sourceRoot -File)
    $sourceEntries += Get-Item -LiteralPath (Join-Path $sourceRoot 'bt\LICENSE-MS-PL.txt')
    $sourceEntries += Get-Item -LiteralPath (Join-Path $sourceRoot '.github\scripts\packages.config')
    $sourceManifest = New-Object Text.StringBuilder
    foreach ($sourceEntry in ($sourceEntries | Sort-Object FullName -Unique)) {
        $relativeName = $sourceEntry.FullName.Substring($sourceRoot.TrimEnd('\').Length + 1).Replace('\', '/')
        $sourceFileStream = [IO.File]::OpenRead($sourceEntry.FullName)
        $fileHasher = [Security.Cryptography.SHA256]::Create()
        try {
            $sourceHash = ([BitConverter]::ToString($fileHasher.ComputeHash($sourceFileStream))).Replace('-', '')
        } finally {
            $sourceFileStream.Dispose()
            $fileHasher.Dispose()
        }
        [void]$sourceManifest.Append($relativeName).Append([char]0).Append($sourceHash).Append("`n")
    }
    $sourceHasher = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sourceHasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($sourceManifest.ToString())))).Replace('-', '').ToLowerInvariant()
    } finally {
        $sourceHasher.Dispose()
    }
}

function Get-NextSourceVersion([version]$Previous) {
    $parts = @($Previous.Major, $Previous.Minor, $Previous.Build, $Previous.Revision)
    for ($partIndex = 3; $partIndex -ge 0; $partIndex--) {
        if ($parts[$partIndex] -lt 65535) {
            $parts[$partIndex]++
            return [version]($parts -join '.')
        }
        $parts[$partIndex] = 0
    }
    throw 'Driver version range exhausted.'
}
if (-not (Get-Command msbuild.exe -ErrorAction SilentlyContinue)) {
    throw 'Run this script from a configured WDK/EWDK build environment.'
}
if ($PackageFirmware -and $Driver -ne 'fn') {
    if ([string]::IsNullOrWhiteSpace($FirmwareDirectory)) {
        throw 'FirmwareDirectory is required for firmware packaging.'
    }
    $FirmwareDirectory = [IO.Path]::GetFullPath($FirmwareDirectory)
    if (-not (Test-Path -LiteralPath $FirmwareDirectory -PathType Container)) {
        throw 'FirmwareDirectory must be an existing external directory.'
    }
    if ($FirmwareDirectory.Equals($sourceRoot, [StringComparison]::OrdinalIgnoreCase) -or
        $FirmwareDirectory.StartsWith($sourceRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Keep firmware outside the source folder.'
    }
}
$project = switch ($Driver) {
    'fn' { Join-Path $sourceRoot 'fn\build\ap6256fn.vcxproj' }
    'bt' { Join-Path $sourceRoot 'bt\build\ap6256bt.vcxproj' }
    default { Join-Path $sourceRoot 'build\ap6256.vcxproj' }
}
$output = Join-Path $outputPath 'out'
$intermediate = Join-Path $outputPath 'obj'
$metadataPath = Join-Path $outputPath 'metadata'
New-Item -ItemType Directory -Path $output, $intermediate, $metadataPath, $versionCachePath -Force | Out-Null
$versionStatePath = Join-Path $versionCachePath 'source-versions.json'
$cacheHasher = [Security.Cryptography.SHA256]::Create()
try {
    $cacheKey = ([BitConverter]::ToString($cacheHasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($versionStatePath.ToLowerInvariant())))).Replace('-', '')
} finally {
    $cacheHasher.Dispose()
}
$versionMutex = New-Object Threading.Mutex($false, ('Local\AP6256SourceVersion-' + $cacheKey))
$versionLockHeld = $false
try {
    try {
        $versionLockHeld = $versionMutex.WaitOne()
    } catch [Threading.AbandonedMutexException] {
        $versionLockHeld = $true
    }
    $sourceFingerprint = Get-SourceFingerprint
    $versions = @{}
    $highestVersion = [version]'1.0.0.0'
    if (Test-Path -LiteralPath $versionStatePath -PathType Leaf) {
        $versionState = Get-Content -LiteralPath $versionStatePath -Raw | ConvertFrom-Json
        if ($versionState.schema -ne 1 -or $null -eq $versionState.versions) {
            throw 'Invalid source-version cache.'
        }
        foreach ($entry in $versionState.versions.PSObject.Properties) {
            if ($entry.Name -notmatch '^[0-9a-f]{64}$') { throw 'Invalid cached source fingerprint.' }
            $cachedVersion = [version]$entry.Value
            if ($cachedVersion.Build -lt 0 -or $cachedVersion.Revision -lt 0 -or
                @($cachedVersion.Major, $cachedVersion.Minor, $cachedVersion.Build, $cachedVersion.Revision | Where-Object { $_ -gt 65535 }).Count -ne 0) {
                throw 'Invalid cached driver version.'
            }
            $versions[$entry.Name] = $cachedVersion.ToString(4)
            if ($cachedVersion -gt $highestVersion) { $highestVersion = $cachedVersion }
        }
    }
    if ($explicitVersion) {
        $sourceVersion = $Version
    } elseif ($versions.ContainsKey($sourceFingerprint)) {
        $sourceVersion = [version]$versions[$sourceFingerprint]
    } elseif ($versions.Count -eq 0) {
        $sourceVersion = [version]'1.0.0.0'
    } else {
        $sourceVersion = Get-NextSourceVersion $highestVersion
    }
    $generatedHeader = New-Object Text.StringBuilder
    [void]$generatedHeader.AppendLine('/* SPDX-License-Identifier: GPL-3.0-only */')
    if ($Driver -eq 'bt') {
        [void]$generatedHeader.AppendLine('/* Additional permission: see BLUETOOTH-LINKING-EXCEPTION.txt. */')
    }
    [void]$generatedHeader.AppendLine('#ifndef AP6256_GENERATED_BUILD_VERSION_H')
    [void]$generatedHeader.AppendLine('#define AP6256_GENERATED_BUILD_VERSION_H')
    foreach ($versionPrefix in @('AP6256', 'AP6256FN', 'AP6256BT')) {
        [void]$generatedHeader.AppendLine("#define $($versionPrefix)_VER_MAJOR $($sourceVersion.Major)")
        [void]$generatedHeader.AppendLine("#define $($versionPrefix)_VER_MINOR $($sourceVersion.Minor)")
        [void]$generatedHeader.AppendLine("#define $($versionPrefix)_VER_BUILD $($sourceVersion.Build)")
        [void]$generatedHeader.AppendLine("#define $($versionPrefix)_VER_REV $($sourceVersion.Revision)")
    }
    [void]$generatedHeader.AppendLine('#define AP6256_BUILD_STAMP "' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + '"')
    [void]$generatedHeader.AppendLine('#endif')
    [IO.File]::WriteAllText((Join-Path $metadataPath 'ap6256_build_version.h'), $generatedHeader.ToString(), (New-Object Text.UTF8Encoding($false)))
    Write-Output ('Source version: ' + $sourceVersion.ToString(4))
    $packageBuildArguments = @()
    if (-not [string]::IsNullOrWhiteSpace($WdkPackageDirectory)) {
        $packageRoot = [IO.Path]::GetFullPath($WdkPackageDirectory)
        [xml]$packageManifest = Get-Content -LiteralPath (Join-Path $sourceRoot '.github\scripts\packages.config') -Raw
        $packageVersions = @{}
        foreach ($package in $packageManifest.packages.package) {
            if ($package.id -notin @('Microsoft.Windows.WDK.ARM64', 'Microsoft.Windows.SDK.CPP.ARM64', 'Microsoft.Windows.SDK.CPP') -or
                $packageVersions.ContainsKey([string]$package.id)) { throw 'Invalid SDK/WDK package manifest.' }
            $packageVersion = [version]$package.version
            if ($packageVersion.Major -ne 10 -or $packageVersion.Minor -ne 0 -or $packageVersion.Build -ne 26100 -or $packageVersion.Revision -lt 0) {
                throw 'Use the supported 10.0.26100 SDK/WDK series with Visual Studio 2022.'
            }
            $packageVersions[[string]$package.id] = [string]$package.version
        }
        if ($packageVersions.Count -ne 3) { throw 'The SDK/WDK manifest must contain all three packages.' }
        $packageImports = @(
            ('Microsoft.Windows.WDK.ARM64.' + $packageVersions['Microsoft.Windows.WDK.ARM64'] + '\build\native\Microsoft.Windows.WDK.arm64.props'),
            ('Microsoft.Windows.SDK.CPP.ARM64.' + $packageVersions['Microsoft.Windows.SDK.CPP.ARM64'] + '\build\native\Microsoft.Windows.SDK.cpp.arm64.props'),
            ('Microsoft.Windows.SDK.CPP.' + $packageVersions['Microsoft.Windows.SDK.CPP'] + '\build\native\Microsoft.Windows.SDK.cpp.props')
        )
        $packageProps = New-Object Text.StringBuilder
        [void]$packageProps.AppendLine('<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">')
        foreach ($packageImport in $packageImports) {
            $importPath = Join-Path $packageRoot $packageImport
            if (-not (Test-Path -LiteralPath $importPath -PathType Leaf)) {
                throw ('Missing WDK/SDK package: ' + $packageImport)
            }
            [void]$packageProps.AppendLine('<Import Project="' + [Security.SecurityElement]::Escape($importPath) + '" />')
        }
        [void]$packageProps.AppendLine('<PropertyGroup><Ap6256KitRoot>$([MSBuild]::EnsureTrailingSlash($(WDKContentRoot)))</Ap6256KitRoot></PropertyGroup>')
        [void]$packageProps.AppendLine('</Project>')
        $packagePropsPath = Join-Path $metadataPath 'ap6256_packages.props'
        [IO.File]::WriteAllText($packagePropsPath, $packageProps.ToString(), (New-Object Text.UTF8Encoding($false)))
        $packageBuildArguments += "/p:DirectoryBuildPropsPath=$packagePropsPath"
        $packageBuildArguments += '/p:WindowsTargetPlatformVersion=10.0.26100.0'
        $packageSharedHeaders = Join-Path $packageRoot ('Microsoft.Windows.SDK.CPP.' + $packageVersions['Microsoft.Windows.SDK.CPP'] + '\c\Include\10.0.26100.0\shared')
        $packageBuildArguments += "/p:Ap6256KitShared=$packageSharedHeaders"
    }
$buildArguments = @(
    $project,
    '/t:Rebuild',
    '/p:Configuration=Release',
    '/p:Platform=ARM64',
    '/p:SignMode=Off',
    '/p:RunCodeAnalysis=true',
    '/p:CodeAnalysisTreatWarningsAsErrors=true',
    "/p:SupportsPackaging=$($PackageFirmware.IsPresent.ToString().ToLowerInvariant())",
    "/p:EnableInf2cat=$($PackageFirmware.IsPresent.ToString().ToLowerInvariant())",
    "/p:Ap6256PackageFirmware=$($PackageFirmware.IsPresent.ToString().ToLowerInvariant())",
    "/p:OutDir=$output/",
    "/p:IntDir=$intermediate/",
    "/p:Ap6256BuildVersion=$($sourceVersion.ToString(4))",
    '/p:Ap6256UseBuildVersion=true',
    "/p:Ap6256BuildMetadataDirectory=$metadataPath",
    '/m'
)
$buildArguments += $packageBuildArguments
if ($Driver -in @('wdi', 'wificx', 'diagnostic')) {
    $buildArguments += "/p:Ap6256Wdi=$((($Driver -eq 'wdi')).ToString().ToLowerInvariant())"
    $buildArguments += "/p:Ap6256Wifi=$((($Driver -eq 'wificx')).ToString().ToLowerInvariant())"
    $buildArguments += "/p:Ap6256HostCipherCandidate=$($HostCiphers.IsPresent.ToString().ToLowerInvariant())"
    $buildArguments += "/p:Ap6256TraceDatapath=$($PacketTrace.IsPresent.ToString().ToLowerInvariant())"
}
if ($PackageFirmware -and $Driver -ne 'fn') {
    $buildArguments += "/p:Ap6256FirmwareRoot=$FirmwareDirectory"
}
& msbuild.exe @buildArguments
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE."
}
    if ((Get-SourceFingerprint) -ne $sourceFingerprint) {
        throw 'Source files changed during the build. Rebuild the current source.'
    }
    if (-not $explicitVersion -and -not $versions.ContainsKey($sourceFingerprint)) {
        $versions[$sourceFingerprint] = $sourceVersion.ToString(4)
        $stateJson = @{ schema = 1; versions = $versions } | ConvertTo-Json -Depth 4
        $temporaryStatePath = $versionStatePath + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
        [IO.File]::WriteAllText($temporaryStatePath, $stateJson + "`n", (New-Object Text.UTF8Encoding($false)))
        if (Test-Path -LiteralPath $versionStatePath -PathType Leaf) {
            [IO.File]::Replace($temporaryStatePath, $versionStatePath, [System.Management.Automation.Language.NullString]::Value)
        } else {
            [IO.File]::Move($temporaryStatePath, $versionStatePath)
        }
    }
} finally {
    if ($versionLockHeld) { $versionMutex.ReleaseMutex() }
    $versionMutex.Dispose()
}
