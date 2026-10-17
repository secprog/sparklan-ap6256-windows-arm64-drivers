# SPDX-License-Identifier: GPL-3.0-only
# Licensed under the GNU General Public License, version 3 only.
# Provided without warranty. See LICENSE for the license text.

from pathlib import Path
import subprocess

from test_support import output_dir, write_text
from toolchain import VC, INC, LIB

ROOT = Path(__file__).resolve().parents[1]
OUT = output_dir('fn-logging')
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
typedef PVOID PDEVICE_OBJECT;
typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } UNICODE_STRING;
#define RTL_CONSTANT_STRING(s) {sizeof(s) - sizeof(WCHAR), sizeof(s), (PWSTR)(s)}
typedef struct { ULONG TitleIndex, Type, DataLength; UCHAR Data[1]; } KEY_VALUE_PARTIAL_INFORMATION;
static NTSTATUS openStatus, queryStatus;
static ULONG queryType = REG_DWORD, queryLength = 4, querySize = 16, queryLevels[4];
static unsigned opens, closes, prints;
static ULONG printedMask;
static NTSTATUS IoOpenDeviceRegistryKey(PDEVICE_OBJECT device, ULONG kind, ACCESS_MASK access, HANDLE *key) {
    assert((device == (PVOID)2 || device == (PVOID)3) && kind == PLUGPLAY_REGKEY_DRIVER && access == KEY_QUERY_VALUE);
    if (openStatus < 0) return openStatus;
    opens++; *key = device; return 0;
}
static NTSTATUS ZwQueryValueKey(HANDLE key, UNICODE_STRING *name, ULONG kind, void *buffer, ULONG size, ULONG *returned) {
    KEY_VALUE_PARTIAL_INFORMATION *value = buffer;
    assert((key == (HANDLE)2 || key == (HANDLE)3) && kind == KeyValuePartialInformation && size >= 16);
    assert(wcscmp(name->Buffer, L"DebugLogLevel") == 0);
    value->Type = queryType; value->DataLength = queryLength;
    memcpy(value->Data, &queryLevels[(UINT_PTR)key], 4); *returned = querySize;
    return queryStatus;
}
static void ZwClose(HANDLE key) { assert(key == (HANDLE)2 || key == (HANDLE)3); closes++; }
static ULONG vDbgPrintExWithPrefix(const char *prefix, ULONG component, ULONG mask, const char *format, va_list args) {
    char text[256];
    assert(strcmp(prefix, "ap6256fn: ") == 0 && component == DPFLTR_IHVDRIVER_ID);
    vsnprintf(text, sizeof(text), format, args); prints++; printedMask = mask; return 0;
}
'''

HARNESS = r'''
#include "logging.c"
int main(void) {
    ULONG selected, severity, f2, f3;
    unsigned evaluated = 0, before;
    assert(FnLogReadSettings((PVOID)2) == 0 && FnLogReadSettings((PVOID)3) == 0);
    for (selected = 0; selected <= 4; selected++) {
        queryLevels[2] = selected; f2 = FnLogReadSettings((PVOID)2);
        for (severity = 0; severity <= 6; severity++) assert(FnLogEnabled(f2, severity) == (severity > 0 && severity <= selected));
        before = prints;
        FnTrace(f2, FN_LOG_ERROR, "error %u\n", ++evaluated);
        FnTrace(f2, FN_LOG_WARNING, "warning\n");
        FnTrace(f2, FN_LOG_INFORMATION, "information\n");
        FnTrace(f2, FN_LOG_VERBOSE, "verbose\n");
        assert(prints - before == selected);
        if (selected) assert((printedMask & 0x80000001u) == 0x80000001u);
    }
    assert(evaluated == 4);
    queryLevels[2] = 0; queryLevels[3] = 4;
    f2 = FnLogReadSettings((PVOID)2); f3 = FnLogReadSettings((PVOID)3); before = prints;
    FnTrace(f2, FN_LOG_ERROR, "off %u", ++evaluated);
    FnTrace(f3, FN_LOG_VERBOSE, "function 3 enabled\n");
    assert(prints == before + 1 && evaluated == 4 && !FnLogEnabled(f2, 1));
    queryLevels[2] = 2; f2 = FnLogReadSettings((PVOID)2);
    assert(FnLogEnabled(f2, 2) && !FnLogEnabled(f2, 3) && FnLogEnabled(f3, 4));
    queryLevels[2] = 5; assert(FnLogReadSettings((PVOID)2) == 0);
    queryLevels[2] = MAXDWORD; assert(FnLogReadSettings((PVOID)2) == 0);
    queryLevels[2] = 4; queryType = REG_SZ; assert(FnLogReadSettings((PVOID)2) == 0); queryType = REG_DWORD;
    queryLength = 3; assert(FnLogReadSettings((PVOID)2) == 0);
    queryLength = 8; assert(FnLogReadSettings((PVOID)2) == 0); queryLength = 4;
    querySize = 12; assert(FnLogReadSettings((PVOID)2) == 0); querySize = 16;
    queryStatus = (NTSTATUS)0xc0000034; assert(FnLogReadSettings((PVOID)2) == 0); queryStatus = 0;
    before = opens; openStatus = (NTSTATUS)0xc0000022;
    assert(FnLogReadSettings((PVOID)2) == 0 && opens == before);
    before = prints; FnLogPrint(0, "off"); FnLogPrint(5, "bad"); assert(prints == before);
    assert(!FnLogEnabled(5, 1) && !FnLogEnabled(MAXDWORD, 1) && opens == closes);
    puts("Companion quiet defaults, per-function settings, reload, level filtering and malformed values passed");
    return 0;
}
'''

write_text(OUT / 'ntddk.h', STUB)
write_text(OUT / 'fn-logging-test.c', HARNESS)
subprocess.run([str(VC / 'bin/HostX64/x64/cl.exe'), '/nologo', '/W4', '/WX',
                *['/I' + str(path) for path in (OUT, ROOT / 'fn/src', VC / 'include', INC / 'ucrt', INC / 'shared', INC / 'um')],
                str(OUT / 'fn-logging-test.c'), '/Fe:' + str(OUT / 'fn-logging-test.exe'),
                '/Fo:' + str(OUT / 'fn-logging-test.obj'), '/link',
                '/LIBPATH:' + str(VC / 'lib/x64'), '/LIBPATH:' + str(LIB / 'ucrt/x64'),
                '/LIBPATH:' + str(LIB / 'um/x64')], check=True, cwd=OUT)
subprocess.run([str(OUT / 'fn-logging-test.exe')], check=True, cwd=OUT)
source = (ROOT / 'fn/src/fn.c').read_text(encoding='utf-8-sig')
prepare = source[source.index('Ap6256FnEvtPrepareHardware('):source.index('Ap6256FnEvtReleaseHardware(')]
assert prepare.index('ctx->LogLevel = FnLogReadSettings(') < prepare.index('FnTrace(')
add = source[source.index('Ap6256FnEvtDeviceAdd('):]
assert add.index('logLevel = FnLogReadSettings(') < add.index('FnTrace(')
assert 'pnpCaps.NoDisplayInUI = WdfFalse;' in add
assert 'DbgPrint' not in source
assert 'HKR,,DebugLogLevel,0x10003,0' in (ROOT / 'fn/inf/ap6256fn.inf').read_text()
