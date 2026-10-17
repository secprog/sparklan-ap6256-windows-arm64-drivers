/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Licensed under the GNU General Public License, version 3 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#pragma once
#include <ntddk.h>

#define FN_LOG_DISABLED     0u
#define FN_LOG_ERROR        1u
#define FN_LOG_WARNING      2u
#define FN_LOG_INFORMATION  3u
#define FN_LOG_VERBOSE      4u

_IRQL_requires_(PASSIVE_LEVEL)
ULONG FnLogReadSettings(_In_ PDEVICE_OBJECT PhysicalDevice);

BOOLEAN FnLogEnabled(_In_ ULONG ConfiguredLevel, _In_ ULONG Level);
VOID FnLogPrint(_In_ ULONG Level, _In_z_ PCSTR Format, ...);

#define FnTrace(configured, level, ...) do { \
    if (FnLogEnabled(configured, level)) { \
        FnLogPrint(level, __VA_ARGS__); \
    } \
} while (0)
