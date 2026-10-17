# SPDX-License-Identifier: GPL-3.0-only
# Licensed under the GNU General Public License, version 3 only.
# Provided without warranty. See LICENSE for the license text.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][string]$ArtifactDirectory,
    [Parameter(Mandatory = $true)][string]$VersionCacheDirectory,
    [Parameter(Mandatory = $true)][string]$WdkPackageDirectory
)

$ErrorActionPreference = 'Stop'
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$artifactRoot = [IO.Path]::GetFullPath($ArtifactDirectory)
if ($artifactRoot.Equals($sourceRoot, [StringComparison]::OrdinalIgnoreCase) -or
    $artifactRoot.StartsWith($sourceRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Choose an artifact directory outside the source folder.'
}
if ($env:BuildLab -notmatch '^[^.]+\.\d+\.\d+$') {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vsPath = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath
    if ([string]::IsNullOrWhiteSpace($vsPath)) { throw 'Visual Studio 2022 ARM64 build tools are required.' }
    Import-Module (Join-Path $vsPath 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=arm64 -host_arch=amd64'
}
New-Item -ItemType Directory -Path $artifactRoot -Force | Out-Null
$buildVersion = $null
$manifest = @()
foreach ($driver in @('wdi', 'wificx', 'diagnostic', 'fn', 'bt')) {
    $driverOutput = Join-Path $OutputDirectory $driver
    & (Join-Path $sourceRoot 'build-source.ps1') -Driver $driver -OutputDirectory $driverOutput -VersionCacheDirectory $VersionCacheDirectory -WdkPackageDirectory $WdkPackageDirectory
    $binaryName = 'ap6256' + $(if ($driver -in @('fn', 'bt')) { $driver } else { '' }) + '.sys'
    $infName = $(if ($driver -eq 'wificx') { 'ap6256-wificx.inf' } else { [IO.Path]::ChangeExtension($binaryName, '.inf') })
    $binaryPath = Join-Path $driverOutput ('out/' + $binaryName)
    $infPath = Join-Path $driverOutput ('out/' + $infName)
    $binaryBytes = [IO.File]::ReadAllBytes($binaryPath)
    $peOffset = [BitConverter]::ToInt32($binaryBytes, 0x3c)
    if ([BitConverter]::ToUInt16($binaryBytes, $peOffset + 4) -ne 0xaa64) { throw 'Expected an ARM64 driver.' }
    $binaryVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($binaryPath).FileVersion
    if ($null -eq $buildVersion) { $buildVersion = $binaryVersion }
    if ($binaryVersion -ne $buildVersion -or
        (Get-Content -LiteralPath $infPath -Raw) -notmatch ('(?m)^DriverVer\s*=\s*[^,\r\n]+,' + [regex]::Escape($buildVersion) + '\s*$')) {
        throw 'Driver binary and INF versions must match across all variants.'
    }
    $artifactVariant = Join-Path $artifactRoot $driver
    New-Item -ItemType Directory -Path $artifactVariant -Force | Out-Null
    Copy-Item -LiteralPath $binaryPath, $infPath -Destination $artifactVariant
    if ($driver -in @('bt', 'fn')) {
        $propertiesPath = Join-Path $driverOutput ("out/ap6256${driver}props.dll")
        $propertiesBytes = [IO.File]::ReadAllBytes($propertiesPath)
        $propertiesOffset = [BitConverter]::ToInt32($propertiesBytes, 0x3c)
        if ([BitConverter]::ToUInt16($propertiesBytes, $propertiesOffset + 4) -ne 0xaa64 -or
            [Diagnostics.FileVersionInfo]::GetVersionInfo($propertiesPath).FileVersion -ne $buildVersion) {
            throw 'Device properties must be ARM64 and match the driver version.'
        }
        Copy-Item -LiteralPath $propertiesPath -Destination $artifactVariant
    }
    foreach ($artifactFile in Get-ChildItem -LiteralPath $artifactVariant -File) {
        $manifest += (Get-FileHash -LiteralPath $artifactFile.FullName -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + $driver + '/' + $artifactFile.Name
    }
}
foreach ($licenseFile in @('LICENSE', 'LICENSE-ISC.txt', 'NOTICE.txt', 'BLUETOOTH-LINKING-EXCEPTION.txt', 'bt/LICENSE-MS-PL.txt')) {
    Copy-Item -LiteralPath (Join-Path $sourceRoot $licenseFile) -Destination $artifactRoot
}
$gitRoot = & git -C $sourceRoot rev-parse --show-toplevel
if ($LASTEXITCODE -ne 0 -or [IO.Path]::GetFullPath($gitRoot) -ne $sourceRoot) { throw 'Build artifacts require the matching source repository.' }
$sourceNames = ((& git -C $sourceRoot ls-files -z) -join "`n").Split([char]0, [StringSplitOptions]::RemoveEmptyEntries)
if ($LASTEXITCODE -ne 0) { throw 'Matching source file list failed.' }
$sourceZip = [IO.Compression.ZipFile]::Open((Join-Path $artifactRoot 'source.zip'), [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($sourceName in $sourceNames) {
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($sourceZip, (Join-Path $sourceRoot $sourceName), $sourceName, [IO.Compression.CompressionLevel]::Optimal)
    }
} finally {
    $sourceZip.Dispose()
}
$artifactReadme = 'AP6256 Windows ARM64 drivers, version ' + $buildVersion + ".`n`nUnsigned Release builds with code analysis enabled. Each variant contains SYS and INF files. Bluetooth and the SDIO companion include their device property-page DLLs with a static C runtime. Firmware, catalogs and signing are supplied separately. See the matching source.zip and included licenses for build instructions and license scope.`n"
[IO.File]::WriteAllText((Join-Path $artifactRoot 'README.txt'), $artifactReadme, (New-Object Text.UTF8Encoding($false)))
[IO.File]::WriteAllText((Join-Path $artifactRoot 'SHA256SUMS.txt'), (($manifest -join "`n") + "`n"), (New-Object Text.UTF8Encoding($false)))
if (-not [string]::IsNullOrWhiteSpace($env:GITHUB_OUTPUT)) {
    Add-Content -LiteralPath $env:GITHUB_OUTPUT -Value ('version=' + $buildVersion)
}
if (-not [string]::IsNullOrWhiteSpace($env:GITHUB_STEP_SUMMARY)) {
    Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value ('Built ARM64 version **' + $buildVersion + '**: WDI, WiFiCx, diagnostic, SDIO function and Bluetooth. Download the driver artifact from this run. The artifact includes unsigned SYS/INF files, Bluetooth and companion property-page DLLs, matching source and licenses.')
}
