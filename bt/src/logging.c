/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Licensed under the GNU General Public License, version 3 only.
 * Provided without warranty. See LICENSE for the license text.
 * Additional permission: see BLUETOOTH-LINKING-EXCEPTION.txt.
 */

#include "logging.h"
#include <stdarg.h>

static volatile LONG BtLogLevel = BT_LOG_DISABLED;

_IRQL_requires_(PASSIVE_LEVEL)
VOID
BtLogLoadSettings(_In_ PDEVICE_OBJECT PhysicalDevice)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"DebugLogLevel");
    KEY_VALUE_PARTIAL_INFORMATION value;
    HANDLE key;
    ULONG size = 0;
    ULONG level = BT_LOG_DISABLED;
    NTSTATUS status;

    InterlockedExchange(&BtLogLevel, BT_LOG_DISABLED);
    status = IoOpenDeviceRegistryKey(
        PhysicalDevice, PLUGPLAY_REGKEY_DRIVER, KEY_QUERY_VALUE, &key);
    if (!NT_SUCCESS(status)) {
        return;
    }

    RtlZeroMemory(&value, sizeof(value));
    status = ZwQueryValueKey(key, &name, KeyValuePartialInformation,
                             &value, sizeof(value), &size);
    ZwClose(key);
    if (NT_SUCCESS(status) && value.Type == REG_DWORD &&
        value.DataLength == sizeof(level) &&
        size >= FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(level)) {
        RtlCopyMemory(&level, value.Data, sizeof(level));
        if (level <= BT_LOG_VERBOSE) {
            InterlockedExchange(&BtLogLevel, (LONG)level);
        }
    }
}

BOOLEAN
BtLogEnabled(_In_ ULONG Level)
{
    return (BOOLEAN)(Level > BT_LOG_DISABLED && Level <= BT_LOG_VERBOSE &&
                     (ULONG)ReadNoFence(&BtLogLevel) >= Level);
}

BOOLEAN
BtWppEnabled(_In_ ULONG Level)
{
    return (BOOLEAN)(Level > 0 && Level <= 5 &&
                     BtLogEnabled(Level <= 2 ? BT_LOG_ERROR : Level - 1));
}

VOID
BtLogPrint(_In_ ULONG Level, _In_z_ PCSTR Format, ...)
{
    va_list args;
    ULONG severity;

    if (!BtLogEnabled(Level)) {
        return;
    }

    severity = Level == BT_LOG_ERROR ? DPFLTR_ERROR_LEVEL :
               Level == BT_LOG_WARNING ? DPFLTR_WARNING_LEVEL :
               Level == BT_LOG_INFORMATION ? DPFLTR_INFO_LEVEL : DPFLTR_TRACE_LEVEL;
    va_start(args, Format);
    vDbgPrintExWithPrefix("ap6256bt: ", DPFLTR_IHVDRIVER_ID,
                         DPFLTR_MASK | 1u | (1u << severity), Format, args);
    va_end(args);
}
