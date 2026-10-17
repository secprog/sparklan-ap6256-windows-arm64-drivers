# SPDX-License-Identifier: GPL-3.0-only
# Licensed under the GNU General Public License, version 3 only.
# Provided without warranty. See LICENSE for the license text.
# Additional permission: BLUETOOTH-LINKING-EXCEPTION.txt.

from pathlib import Path
import re
import subprocess

from test_support import output_dir, write_bluetooth_text
from toolchain import VC, INC, LIB

ROOT = Path(__file__).resolve().parents[1]
OUT = output_dir('bt-parameters')
OUT.mkdir(parents=True, exist_ok=True)
source = (ROOT / 'bt/src/device.c').read_text(encoding='utf-8-sig')
config = source[source.index('typedef struct _AP6256BT_CONFIG'):source.index('WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AP6256BT_CONFIG')]
query = source[source.index('static VOID\nAp6256BtQueryULong('):source.index('static NTSTATUS\nAp6256BtOpenGpio(')]
constants = '\n'.join(line for line in source.splitlines()
                      if re.match(r'#define AP6256BT_(DEFAULT_|FIRMWARE_)', line))

PRE = r'''
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <wchar.h>
typedef LONG NTSTATUS;
#define PAGED_CODE() ((void)0)
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#define WDF_NO_OBJECT_ATTRIBUTES NULL
#define DoTrace(l, f, args) ((void)0)
#define MAXUCHAR 255
typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } UNICODE_STRING, *PUNICODE_STRING;
typedef HANDLE WDFKEY, WDFDRIVER;
typedef struct { unsigned type; } WDF_OBJECT_ATTRIBUTES;
#define WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(a, t) ((a)->type = sizeof(t))
static void RtlInitEmptyUnicodeString(UNICODE_STRING *value, WCHAR *buffer, USHORT size) {
    value->Length = 0; value->MaximumLength = size; value->Buffer = buffer; buffer[0] = 0;
}
static void RtlInitUnicodeString(UNICODE_STRING *value, PCWSTR text) {
    value->Buffer = (PWSTR)text; value->Length = (USHORT)(wcslen(text) * sizeof(WCHAR));
    value->MaximumLength = value->Length + sizeof(WCHAR);
}
static NTSTATUS RtlUnicodeStringCopyString(UNICODE_STRING *value, PCWSTR text) {
    size_t length = wcslen(text);
    assert((length + 1) * sizeof(WCHAR) <= value->MaximumLength);
    memcpy(value->Buffer, text, (length + 1) * sizeof(WCHAR));
    value->Length = (USHORT)(length * sizeof(WCHAR)); return 0;
}
'''

MOCKS = r'''
static AP6256BT_CONFIG current;
static BOOL allocated, opened, missingKey, failAllocate;
static unsigned allocationCalls, reads, closes;
static const PCWSTR names[] = {L"InitialBaudRate", L"DownloadBaudRate", L"OperationalBaudRate",
    L"PostLaunchDelayMs", L"RegOnDelayMs", L"ConfigureSleepMode", L"ConfigureAudio"};
static ULONG values[] = {115200, 921600, 3000000, 250, 100, 1, 1};
static NTSTATUS WdfObjectAllocateContext(WDFDRIVER driver, WDF_OBJECT_ATTRIBUTES *attributes, PVOID *context) {
    BOOL exists = allocated;
    assert(driver == (HANDLE)42 && attributes->type == sizeof(current)); allocationCalls++;
    if (failAllocate) return (NTSTATUS)0xc000009a;
    allocated = TRUE; *context = &current; return exists ? (NTSTATUS)0x40000000 : 0;
}
static NTSTATUS WdfDriverOpenParametersRegistryKey(WDFDRIVER driver, ACCESS_MASK access, PVOID attributes, WDFKEY *key) {
    assert(driver == (HANDLE)42 && access == GENERIC_READ && !attributes && !opened);
    if (missingKey) return (NTSTATUS)0xc0000034;
    opened = TRUE; *key = (HANDLE)43; return 0;
}
static NTSTATUS WdfRegistryQueryULong(WDFKEY key, UNICODE_STRING *name, ULONG *value) {
    UINT index;
    assert(opened && key == (HANDLE)43); reads++;
    for (index = 0; index < ARRAYSIZE(names); index++) {
        if (!wcscmp(name->Buffer, names[index])) { *value = values[index]; return 0; }
    }
    return (NTSTATUS)0xc0000034;
}
static NTSTATUS WdfRegistryQueryUnicodeString(WDFKEY key, UNICODE_STRING *name, PVOID size, UNICODE_STRING *value) {
    assert(opened && key == (HANDLE)43 && !size);
    if (!wcscmp(name->Buffer, L"FirmwareDirectory")) return RtlUnicodeStringCopyString(value, L"\\??\\D:\\package\\");
    assert(!wcscmp(name->Buffer, L"RAMPatchFileName"));
    return RtlUnicodeStringCopyString(value, L"BCM4345C5.hcd");
}
static void WdfRegistryClose(WDFKEY key) { assert(opened && key == (HANDLE)43); opened = FALSE; closes++; }
'''

MAIN = r'''
int main(void) {
    unsigned before;
    DeviceQueryDeviceParameters((HANDLE)42);
    assert(allocated && !opened && allocationCalls == 1 && closes == 1);
    assert(current.InitialBaudRate == 115200 && current.DownloadBaudRate == 921600 && current.OperationalBaudRate == 3000000);
    assert(current.PostLaunchDelayMs == 250 && current.RegOnDelayMs == 100 && current.ConfigureSleepMode == 1 && current.ConfigureAudio == 1);
    values[0] = 230400; values[1] = 115200; values[2] = 921600;
    values[3] = 5000; values[4] = 0; values[5] = values[6] = 0;
    current.ConfigureSleepMode = 0;
    before = reads; DeviceQueryDeviceParameters((HANDLE)42);
    assert(allocationCalls == 2 && closes == 2 && reads > before && !opened);
    assert(current.InitialBaudRate == 230400 && current.DownloadBaudRate == 115200 && current.OperationalBaudRate == 921600);
    assert(current.PostLaunchDelayMs == 5000 && current.RegOnDelayMs == 0 && !current.ConfigureSleepMode && !current.ConfigureAudio);
    assert(!wcscmp(current.FirmwareDirectory.Buffer, L"\\??\\D:\\package\\"));
    assert(!wcscmp(current.RampatchFileName.Buffer, L"BCM4345C5.hcd"));
    values[3] = 5001; DeviceQueryDeviceParameters((HANDLE)42); assert(current.InitialBaudRate == 0 && !opened);
    values[3] = 0; values[4] = 5000; DeviceQueryDeviceParameters((HANDLE)42); assert(current.InitialBaudRate == 230400 && !opened);
    values[5] = 2; DeviceQueryDeviceParameters((HANDLE)42); assert(current.InitialBaudRate == 0 && !opened);
    values[5] = 0; values[6] = 2; DeviceQueryDeviceParameters((HANDLE)42); assert(current.InitialBaudRate == 0 && !opened);
    values[6] = 1; values[2] = 4000001; DeviceQueryDeviceParameters((HANDLE)42); assert(current.InitialBaudRate == 0 && !opened);
    values[2] = 4000000; DeviceQueryDeviceParameters((HANDLE)42); assert(current.InitialBaudRate == 230400 && !opened);
    missingKey = TRUE; before = closes; DeviceQueryDeviceParameters((HANDLE)42);
    assert(current.InitialBaudRate == 115200 && current.DownloadBaudRate == 921600 && current.OperationalBaudRate == 3000000);
    assert(current.PostLaunchDelayMs == 250 && current.RegOnDelayMs == 100 && !current.ConfigureSleepMode && !current.ConfigureAudio);
    assert(closes == before && !opened);
    failAllocate = TRUE; DeviceQueryDeviceParameters((HANDLE)42); assert(closes == before && !opened);
    puts("Bluetooth parameter defaults, existing-context reload, service key reads and validation passed");
    return 0;
}
'''

write_bluetooth_text(OUT / 'parameters-test.c', PRE + constants + '\n' + config + MOCKS + query + MAIN)
subprocess.run([str(VC / 'bin/HostX64/x64/cl.exe'), '/nologo', '/W4', '/WX',
                *['/I' + str(path) for path in (VC / 'include', INC / 'ucrt', INC / 'shared', INC / 'um')],
                str(OUT / 'parameters-test.c'), '/Fe:' + str(OUT / 'parameters-test.exe'),
                '/Fo:' + str(OUT / 'parameters-test.obj'), '/link',
                '/LIBPATH:' + str(VC / 'lib/x64'), '/LIBPATH:' + str(LIB / 'ucrt/x64'),
                '/LIBPATH:' + str(LIB / 'um/x64')], check=True, cwd=OUT)
subprocess.run([str(OUT / 'parameters-test.exe')], check=True, cwd=OUT)
fdo = (ROOT / 'bt/src/Fdo.c').read_text(encoding='utf-8-sig')
prepare = fdo[fdo.index('FdoDevPrepareHardware('):]
assert prepare.index('DeviceQueryDeviceParameters(') < prepare.index('FdoFindConnectResources(')
driver = (ROOT / 'bt/src/driver.c').read_text(encoding='utf-8-sig')
assert 'DeviceQueryDeviceParameters(' not in driver
inf = (ROOT / 'bt/inf/ap6256bt.inf').read_text(encoding='utf-8-sig')
for name in ('InitialBaudRate', 'DownloadBaudRate', 'OperationalBaudRate', 'PostLaunchDelayMs',
             'RegOnDelayMs', 'ConfigureSleepMode', 'ConfigureAudio'):
    assert 'HKR,Parameters,' + name + ',0x10003,' in inf
assert 'HKR,,DebugLogLevel,0x10003,0' in inf
