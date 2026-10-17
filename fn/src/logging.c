/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Licensed under the GNU General Public License, version 3 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#include "logging.h"
#include <stdarg.h>

_IRQL_requires_(PASSIVE_LEVEL)
ULONG
FnLogReadSettings(_In_ PDEVICE_OBJECT PhysicalDevice)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"DebugLogLevel");
    KEY_VALUE_PARTIAL_INFORMATION value;
    HANDLE key;
    ULONG size = 0;
    ULONG level = FN_LOG_DISABLED;
    NTSTATUS status;

    status = IoOpenDeviceRegistryKey(
        PhysicalDevice, PLUGPLAY_REGKEY_DRIVER, KEY_QUERY_VALUE, &key);
    if (!NT_SUCCESS(status)) {
        return FN_LOG_DISABLED;
    }
    RtlZeroMemory(&value, sizeof(value));
    status = ZwQueryValueKey(key, &name, KeyValuePartialInformation,
                             &value, sizeof(value), &size);
    ZwClose(key);
    if (!NT_SUCCESS(status) || value.Type != REG_DWORD ||
        value.DataLength != sizeof(level) ||
        size < FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(level)) {
        return FN_LOG_DISABLED;
    }
    RtlCopyMemory(&level, value.Data, sizeof(level));
    return level <= FN_LOG_VERBOSE ? level : FN_LOG_DISABLED;
}

BOOLEAN
FnLogEnabled(_In_ ULONG ConfiguredLevel, _In_ ULONG Level)
{
    return (BOOLEAN)(ConfiguredLevel <= FN_LOG_VERBOSE &&
                     Level > FN_LOG_DISABLED && Level <= ConfiguredLevel);
}

VOID
FnLogPrint(_In_ ULONG Level, _In_z_ PCSTR Format, ...)
{
    va_list args;
    ULONG severity;

    if (Level == FN_LOG_DISABLED || Level > FN_LOG_VERBOSE) {
        return;
    }
    severity = Level == FN_LOG_ERROR ? DPFLTR_ERROR_LEVEL :
               Level == FN_LOG_WARNING ? DPFLTR_WARNING_LEVEL :
               Level == FN_LOG_INFORMATION ? DPFLTR_INFO_LEVEL : DPFLTR_TRACE_LEVEL;
    va_start(args, Format);
    vDbgPrintExWithPrefix("ap6256fn: ", DPFLTR_IHVDRIVER_ID,
                         DPFLTR_MASK | 1u | (1u << severity), Format, args);
    va_end(args);
}
