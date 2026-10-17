/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Licensed under the GNU General Public License, version 3 only.
 * Provided without warranty. See LICENSE for the license text.
 * Additional permission: see BLUETOOTH-LINKING-EXCEPTION.txt.
 */

#pragma once

#include <ntddk.h>

#define BT_LOG_DISABLED     0u
#define BT_LOG_ERROR        1u
#define BT_LOG_WARNING      2u
#define BT_LOG_INFORMATION  3u
#define BT_LOG_VERBOSE      4u

_IRQL_requires_(PASSIVE_LEVEL)
VOID BtLogLoadSettings(_In_ PDEVICE_OBJECT PhysicalDevice);

BOOLEAN BtLogEnabled(_In_ ULONG Level);
BOOLEAN BtWppEnabled(_In_ ULONG Level);

VOID BtLogPrint(_In_ ULONG Level, _In_z_ PCSTR Format, ...);

#define BtLog(level, ...) do { \
    if (BtLogEnabled(level)) { \
        BtLogPrint(level, __VA_ARGS__); \
    } \
} while (0)

#define BtTrace(...) BtLog(BT_LOG_INFORMATION, __VA_ARGS__)
#define BtTraceError(...) BtLog(BT_LOG_ERROR, __VA_ARGS__)
#define BtTraceWarning(...) BtLog(BT_LOG_WARNING, __VA_ARGS__)
#define BtTraceVerbose(...) BtLog(BT_LOG_VERBOSE, __VA_ARGS__)
#define BtTraceStatus(status, ...) \
    BtLog(NT_SUCCESS(status) ? BT_LOG_INFORMATION : BT_LOG_ERROR, __VA_ARGS__)
