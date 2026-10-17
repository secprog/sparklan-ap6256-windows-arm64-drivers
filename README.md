<!--
SPDX-License-Identifier: GPL-3.0-only
Licensed under the GNU General Public License, version 3 only.
Provided without warranty. See LICENSE for the license text.
-->

# SparkLAN AP6256 Windows ARM64 drivers

Source for the Wi-Fi driver, auxiliary SDIO function driver and Bluetooth UART
transport. The Wi-Fi driver supports WDI, WiFiCx and a diagnostic configuration.
Firmware patching code and its patch data are included. Firmware images, NVRAM,
CLM data and Bluetooth HCD files are supplied separately.

Version state is stored outside the source folder in
`$env:LOCALAPPDATA\AP6256\SourceVersions`. Use `-VersionCacheDirectory` to
choose another external cache. Keep that cache to preserve version assignments.
Generated version headers are written to the external output directory. Use
`build-source.ps1` for automatic source versioning; direct project builds use
the source default version.
`-Version 1.2.3.4` selects an explicit test-build version without changing the
automatic version cache.

Use a configured Windows Driver Kit or Enterprise Windows Driver Kit environment:

```powershell
.\build-source.ps1 -Driver wdi -OutputDirectory $env:TEMP\ap6256-wdi
.\build-source.ps1 -Driver wificx -OutputDirectory $env:TEMP\ap6256-wificx
.\build-source.ps1 -Driver fn -OutputDirectory $env:TEMP\ap6256-fn
.\build-source.ps1 -Driver bt -OutputDirectory $env:TEMP\ap6256-bt
```

`-HostCiphers` enables the extended station cipher build. `-PacketTrace` enables
packet diagnostics. Log Level defaults to Disabled. Build outputs stay outside
this source folder. Source builds are unsigned and do not include firmware or
install drivers.

To construct a firmware-containing package, add `-PackageFirmware` and
`-FirmwareDirectory` pointing to a separately supplied directory. Wi-Fi packaging
expects `4345r9rtecdc.bin`, `4345r9nvram.txt`
and `4345r9.clmb`. Bluetooth packaging expects `BCM4345C5.hcd`. Signing and device
installation are separate steps.

For Bluetooth logging, open Device Manager as administrator, then open
**System devices > SparkLAN AP6256 Bluetooth (UART) > Properties > Advanced**.
Log Level offers Disabled (the default), Errors, Warnings, Information and
Verbose. Save the changed level and close the properties window to restart
Bluetooth. The setting controls
WinDbg output and the WPP recording threshold; byte dumps require Verbose.
The same tab provides startup, download and operating UART baud rates,
post-launch and reset release delays (0-5000 ms), controller sleep, and PCM-audio
initialization. Startup baud must match the controller. UART choices cover
115200 through 4000000 baud; an existing custom rate is retained. Installed
defaults remain 115200 / 921600 / 3000000 baud, 250 / 100 ms, with sleep and
PCM initialization enabled. Saved settings survive driver upgrades and take
effect when Bluetooth restarts after you close the properties window.
**Restore safe defaults** selects the shipped values for these controls,
including Disabled logging. Review them, then choose Apply or OK to save;
Cancel discards the staged defaults.
Bluetooth packages must include `ap6256btprops.dll` alongside the driver and INF.

The SDIO companion provides **System devices > SparkLAN AP6256 Auxiliary SDIO
Function > Properties > Advanced > Log Level** with the same five levels and
Disabled default. Each function keeps its own setting. **Restore safe defaults**
selects Disabled; save and close properties to restart that companion device.
Companion packages include `ap6256fnprops.dll`. Both property-page DLLs use the
static C runtime and require no Visual C++ Redistributable.

The [Build ARM64 drivers](.github/workflows/build-drivers.yml) workflow runs on
pushes, pull requests and manual dispatches. The host tests must pass before
the driver build starts. It uses the standard `windows-2022`
hosted runner with Visual Studio 2022, which is free for public repositories. No account secrets or
signing certificate are required. Microsoft SDK and ARM64 WDK NuGet packages
are pinned in `.github/scripts/packages.config` to the supported 10.0.26100
series and cached outside the checkout. All five driver
configurations build with code analysis and warnings treated as errors.

Download `ap6256-arm64-<version>-<run>` from the completed run's Artifacts
section. It contains the WDI, WiFiCx, diagnostic, SDIO function and Bluetooth
SYS/INF files, Bluetooth and companion property-page DLLs, SHA-256 checksums, matching source and license texts. These
unsigned outputs require external firmware, catalog generation and signing
before installation. Driver artifacts expire after 30 days; failed builds do
not publish a driver artifact. Dependabot checks Actions and NuGet dependencies
weekly and groups SDK/WDK updates. The build reads package versions from the
manifest, and a manifest change participates in source versioning.

Project-authored source is licensed under GPL-3.0-only. Linux-derived chip
routines and register/firmware command definitions in `src/common/chip.c`,
`chip.h`, `chipregs.h` and `sdpcm.h` retain Broadcom's ISC notices. These mixed
files declare `GPL-3.0-only AND ISC`; project-authored additions remain GPL.
Nine Bluetooth files identified in [NOTICE.txt](NOTICE.txt) retain MS-PL.
See [LICENSE](LICENSE), [LICENSE-ISC.txt](LICENSE-ISC.txt), NOTICE.txt and
[bt/LICENSE-MS-PL.txt](bt/LICENSE-MS-PL.txt) for the license scope and provenance.
The Bluetooth code, its tests and generated build metadata have the narrow
combination permission described in
[BLUETOOTH-LINKING-EXCEPTION.txt](BLUETOOTH-LINKING-EXCEPTION.txt).

## Trademarks

Broadcom, Cypress, Infineon, AMPAK, SparkLAN, Raspberry Pi, and Windows are
trademarks of their respective owners. This is an independent project.

## Host tests

[Public host tests](tests/README.md) cover departure, security, transmit/receive, scanning, power, capabilities, logging and Bluetooth.
