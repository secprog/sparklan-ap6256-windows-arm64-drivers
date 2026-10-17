# SPDX-License-Identifier: GPL-3.0-only
# Licensed under the GNU General Public License, version 3 only.
# Provided without warranty. See LICENSE for the license text.
# Additional permission: BLUETOOTH-LINKING-EXCEPTION.txt.

from pathlib import Path
import subprocess

from test_support import output_dir, write_bluetooth_text
from toolchain import VC, INC, LIB

ROOT = Path(__file__).resolve().parents[1]
OUT = output_dir('bt-logging')
OUT.mkdir(parents=True, exist_ok=True)

STUB = r'''
#pragma once
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define PASSIVE_LEVEL 0
#define PLUGPLAY_REGKEY_DRIVER 2
#define KeyValuePartialInformation 2
#define DPFLTR_ERROR_LEVEL 0
#define DPFLTR_WARNING_LEVEL 1
#define DPFLTR_TRACE_LEVEL 2
#define DPFLTR_INFO_LEVEL 3
#define DPFLTR_MASK 0x80000000u
#define DPFLTR_IHVDRIVER_ID 77
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#define ReadNoFence(p) (*(p))
typedef PVOID PDEVICE_OBJECT;
typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } UNICODE_STRING;
#define RTL_CONSTANT_STRING(s) {sizeof(s) - sizeof(WCHAR), sizeof(s), (PWSTR)(s)}
typedef struct { ULONG TitleIndex, Type, DataLength; UCHAR Data[1]; } KEY_VALUE_PARTIAL_INFORMATION;
static NTSTATUS openStatus, queryStatus;
static ULONG queryType = REG_DWORD, queryLength = 4, querySize = 16, queryLevel;
static unsigned closes, prints;
static ULONG printedMask;
static NTSTATUS IoOpenDeviceRegistryKey(PDEVICE_OBJECT device, ULONG kind, ACCESS_MASK access, HANDLE *key) {
    assert(device == (PVOID)42 && kind == PLUGPLAY_REGKEY_DRIVER && access == KEY_QUERY_VALUE);
    *key = (HANDLE)43;
    return openStatus;
}
static NTSTATUS ZwQueryValueKey(HANDLE key, UNICODE_STRING *name, ULONG kind, void *buffer, ULONG size, ULONG *returned) {
    KEY_VALUE_PARTIAL_INFORMATION *value = buffer;
    assert(key == (HANDLE)43 && kind == KeyValuePartialInformation);
    assert(wcscmp(name->Buffer, L"DebugLogLevel") == 0 && size >= 16);
    value->Type = queryType; value->DataLength = queryLength;
    memcpy(value->Data, &queryLevel, sizeof(queryLevel)); *returned = querySize;
    return queryStatus;
}
static void ZwClose(HANDLE key) { assert(key == (HANDLE)43); closes++; }
static ULONG vDbgPrintExWithPrefix(const char *prefix, ULONG component, ULONG mask, const char *format, va_list args) {
    char text[256];
    assert(strcmp(prefix, "ap6256bt: ") == 0 && component == DPFLTR_IHVDRIVER_ID);
    vsnprintf(text, sizeof(text), format, args);
    prints++; printedMask = mask; return 0;
}
'''

HARNESS = r'''
#include "logging.c"
#define AP6256BT_TRACE_BYTES 16u
'''

device = (ROOT / 'bt/src/device.c').read_text(encoding='utf-8-sig')
fdo = (ROOT / 'bt/src/Fdo.c').read_text(encoding='utf-8-sig')
dump = device[device.index('static VOID\nAp6256BtTraceBytes('):device.index('static NTSTATUS\nAp6256BtWriteExact(')]
ioctl = fdo[fdo.index('#define BT_IOCTL_TRACE_LIMIT'):fdo.index('static PCSTR\nBtIoctlName(')]
HARNESS += dump + ioctl + r'''
static void load(ULONG level) {
    openStatus = queryStatus = 0;
    queryType = REG_DWORD; queryLength = 4; querySize = 16; queryLevel = level;
    BtLogLoadSettings((PVOID)42);
}
int main(void) {
    ULONG selected, severity;
    unsigned before, evaluated = 0;
    UCHAR bytes[16] = {0};
    assert(!BtLogEnabled(BT_LOG_ERROR));
    for (selected = 0; selected <= 4; selected++) {
        load(selected);
        for (severity = 0; severity <= 6; severity++) {
            assert(BtLogEnabled(severity) == (severity > 0 && severity <= selected));
            assert(BtWppEnabled(severity) == (severity > 0 && severity <= 5 &&
                selected > 0 && (severity <= 2 ? 1 : severity - 1) <= selected));
        }
        before = prints;
        BtTraceError("error %u\n", ++evaluated);
        BtTraceWarning("warning\n");
        BtTrace("information\n");
        BtTraceVerbose("verbose\n");
        assert(prints - before == selected);
        if (selected) assert((printedMask & 0x80000001u) == 0x80000001u);
    }
    assert(evaluated == 4);
    load(4); before = prints;
    BtLogPrint(0, "off\n"); BtLogPrint(5, "invalid\n");
    assert(prints == before);
    load(1); before = prints;
    BtTraceStatus((NTSTATUS)0, "success\n");
    BtTraceStatus((NTSTATUS)0xc0000001, "failure\n");
    assert(prints == before + 1);
    load(0); before = prints;
    Ap6256BtTraceBytes("off", (const UCHAR *)1, 16);
    assert(!BtShouldTraceIoctl() && BtIoctlTraceCount == 0 && prints == before);
    load(3); assert(!BtShouldTraceIoctl() && BtIoctlTraceCount == 0);
    load(4); Ap6256BtTraceBytes("data", bytes, sizeof(bytes));
    assert(prints == before + 1);
    for (severity = 0; severity < BT_IOCTL_TRACE_LIMIT; severity++) assert(BtShouldTraceIoctl());
    assert(!BtShouldTraceIoctl());
    load(4); queryLevel = 5; BtLogLoadSettings((PVOID)42); assert(!BtLogEnabled(1));
    load(4); queryLevel = MAXDWORD; BtLogLoadSettings((PVOID)42); assert(!BtLogEnabled(1));
    load(4); queryType = REG_SZ; BtLogLoadSettings((PVOID)42); assert(!BtLogEnabled(1));
    load(4); queryLength = 3; BtLogLoadSettings((PVOID)42); assert(!BtLogEnabled(1));
    load(4); queryLength = 8; BtLogLoadSettings((PVOID)42); assert(!BtLogEnabled(1));
    load(4); querySize = 12; BtLogLoadSettings((PVOID)42); assert(!BtLogEnabled(1));
    load(4); queryStatus = (NTSTATUS)0xc0000034; before = closes;
    BtLogLoadSettings((PVOID)42); assert(!BtLogEnabled(1) && closes == before + 1);
    load(4); openStatus = (NTSTATUS)0xc0000022; before = closes;
    BtLogLoadSettings((PVOID)42); assert(!BtLogEnabled(1) && closes == before);
    puts("Bluetooth log thresholds, quiet paths and registry validation passed");
    return 0;
}
'''

write_bluetooth_text(OUT / 'ntddk.h', STUB)
write_bluetooth_text(OUT / 'logging-test.c', HARNESS)
command = [str(VC / 'bin/HostX64/x64/cl.exe'), '/nologo', '/W4', '/WX', '/wd4505',
           '/I' + str(OUT), '/I' + str(ROOT / 'bt/src'), '/I' + str(VC / 'include'),
           '/I' + str(INC / 'ucrt'), '/I' + str(INC / 'shared'), '/I' + str(INC / 'um'),
           str(OUT / 'logging-test.c'), '/Fe:' + str(OUT / 'logging-test.exe'),
           '/Fo:' + str(OUT / 'logging-test.obj'), '/link',
           '/LIBPATH:' + str(VC / 'lib/x64'), '/LIBPATH:' + str(LIB / 'ucrt/x64'),
           '/LIBPATH:' + str(LIB / 'um/x64')]
subprocess.run(command, check=True, cwd=OUT)
subprocess.run([str(OUT / 'logging-test.exe')], check=True, cwd=OUT)
debug = (ROOT / 'bt/src/Debugdef.h').read_text(encoding='utf-8-sig')
assert debug.count('BtWppEnabled(lvl)') == 2
driver = (ROOT / 'bt/src/driver.c').read_text(encoding='utf-8-sig')
add = driver[driver.index('DriverDeviceAdd('):]
assert add.index('BtLogLoadSettings(') < add.index('DoTrace(')
prepare = fdo[fdo.index('FdoDevPrepareHardware('):]
assert prepare.index('BtLogLoadSettings(') < prepare.index('DoTrace(')
