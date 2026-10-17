/*
 * SPDX-License-Identifier: MS-PL
 * Copyright (c) Microsoft Corporation All Rights Reserved
 * See bt/LICENSE-MS-PL.txt for the license text.
 */

#ifndef __DEBUGDEF_H__
#define __DEBUGDEF_H__

#include "version.h"
#include "buildstamp.h"
#include "logging.h"

#define WPP_CONTROL_GUIDS \
    WPP_DEFINE_CONTROL_GUID(ap6256bt,(a6256b70,4345,4c00,b7a2,56b7a2ea6002),  \
        WPP_DEFINE_BIT(TFLAG_PNP)           \
        WPP_DEFINE_BIT(TFLAG_POWER)         \
        WPP_DEFINE_BIT(TFLAG_UART)          \
        WPP_DEFINE_BIT(TFLAG_IOCTL)         \
        WPP_DEFINE_BIT(TFLAG_IO)            \
        WPP_DEFINE_BIT(TFLAG_DATA)          \
        WPP_DEFINE_BIT(TFLAG_HCI))

#define LEVEL_CRITICAL  TRACE_LEVEL_CRITICAL
#define LEVEL_ERROR     TRACE_LEVEL_ERROR
#define LEVEL_WARNING   TRACE_LEVEL_WARNING
#define LEVEL_INFO      TRACE_LEVEL_INFORMATION
#define LEVEL_VERBOSE   TRACE_LEVEL_VERBOSE

#define WPP_LEVEL_FLAG_ENABLED(lvl, component) \
    (BtWppEnabled(lvl) && WPP_LEVEL_ENABLED(component) && WPP_CONTROL(WPP_BIT_ ## component).Level >=lvl)

#define WPP_LEVEL_FLAG_LOGGER(lvl, component) \
    WPP_LEVEL_LOGGER(component)

#define WPP_RECORDER_LEVEL_FLAG_ARGS(lvl, component) \
    WPP_CONTROL(WPP_BIT_ ## component).AutoLogContext, 0, WPP_BIT_ ## component
#define WPP_RECORDER_LEVEL_FLAG_FILTER(lvl, component) \
    (BtWppEnabled(lvl) && (lvl < TRACE_LEVEL_VERBOSE || WPP_CONTROL(WPP_BIT_ ## component).AutoLogVerboseEnabled))

#define WithinRange(min, value, max) (min <= value && value <= max)
#define MinToPrint(val1, val2) (val1 < val2 ? val1 : val2)
#define MAX_EVENT_PARAMS_TO_DISPLAY     8
#define MAX_COMMAND_PARAMS_TO_DISPLAY   8

#endif
