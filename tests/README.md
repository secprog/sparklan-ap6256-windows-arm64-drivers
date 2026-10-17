# Public host tests

These tests run against the source in this checkout.

Use 64-bit Windows, 64-bit Python 3.9 or later, MSVC C++ x64 tools and matching Windows SDK/WDK headers and x64 libraries, including the WDI/WiFiCx codec library. The tested local toolchain is MSVC 14.44 and SDK/WDK 10.0.26100.0. Microsoft describes the available [WDK packages](https://learn.microsoft.com/en-us/windows-hardware/drivers/install-the-wdk-using-nuget) and [matching SDK/WDK installations](https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk).

Install the independent Python AES-GCM reference implementation into your own environment:

```powershell
python -m pip install -r tests/requirements.txt
```

From a Visual Studio developer shell with the SDK and WDK installed:

```powershell
python -B tests/audit.py
python -B tests/run.py --list
python -B tests/run.py --output C:\ap6256-test-results\run-01
```

The output directory must be empty and outside the checkout. Missing prerequisites or failed suites produce a nonzero exit code. The runner records input hashes, compiler/codec hashes, dependency versions, per-suite logs and an overall `results.json`. It checks that source inputs did not change during the run.

For an extracted EWDK, set its location before running the same commands:

```powershell
$env:AP6256_TEST_EWDK = 'D:\EWDK'
python -B tests/run.py --output C:\ap6256-test-results\run-02
```

`AP6256_TEST_VC`, `AP6256_TEST_SDK` and `AP6256_TEST_SDK_VERSION` explicitly select an MSVC toolset directory, kit content root and header version. Normal developer-shell variables are also recognized. Split NuGet installations can supply additional kit, include and library roots through `AP6256_TEST_EXTRA_KIT`, `AP6256_TEST_EXTRA_INCLUDE` and `AP6256_TEST_EXTRA_LIB`, separated by semicolons. The CI helper configures those paths from the pinned package manifest.

Select individual suites by repeating `--suite`:

```powershell
python -B tests/run.py --suite native-departure --suite explicit-disconnect --suite firmware-startup --output C:\ap6256-test-results\departure
```

The `tracing-live` suite runs by default and creates and removes a user-mode ETW session. It may require Administrator or Performance Log Users privileges. To run it alone:

```powershell
python -B tests/run.py --suite tracing-live --output C:\ap6256-test-results\etw
```

## Coverage

| Area | Coverage |
| --- | --- |
| Protected departure | Receipt validation, pending recovery, commit confirmation, disconnect ordering and key cleanup |
| Startup | Download ordering, zero pre-start patch reads, allocation/write failures, flag publication and recovery; injected regression controls |
| Station security | PMF/RSNE validation, association refusals, enterprise requests, SAE station ownership, host cipher/CNG paths and key barriers |
| Transmit/receive | WDI packing, flow control, EAPOL receive handling, lifetime, queue and role ownership |
| Scan and NLO | Scan result parsing/ownership, NLO result decoding and Microsoft request-codec handling |
| Power and lifecycle | Offload admission, wake guards, role/factory cold paths, recovery, statistics and watchdog behavior |
| Capabilities and logging | WDI/WiFiCx capability publication, authentication/cipher constraints, Disabled logging policy and host trace lifecycle |
| Bluetooth | H4/ACL read bounds, prefetch ownership and cleanup races; logging thresholds; property-page controls, safe-default staging, validation, Apply/Cancel and save rollback; service-parameter reload and upgrade persistence |
| SDIO companion | Per-function quiet logging, settings reload, Advanced controls, safe defaults, Apply/Cancel and property-page lifetime |

The suite compiles selected production C functions in user mode.

## Reports

```powershell
python -B tests/export_reports.py --input C:\ap6256-test-results\run-01 --output C:\ap6256-test-reports\run-01
```

The exporter copies JSON reports, logs and repository license notices. CI uploads this report directory.

Project-authored test code and data are GPL-3.0-only; see [LICENSE](../LICENSE). Bluetooth test combinations use the [Bluetooth linking permission](../BLUETOOTH-LINKING-EXCEPTION.txt); extracted Bluetooth source retains MS-PL and its notices.
