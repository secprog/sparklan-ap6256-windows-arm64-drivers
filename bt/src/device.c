/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Licensed under the GNU General Public License, version 3 only.
 * Provided without warranty. See LICENSE for the license text.
 * Additional permission: see BLUETOOTH-LINKING-EXCEPTION.txt.
 * Copyright (c) AP6256 OSS Driver Project contributors.
 */

#include "driver.h"
#include "Device.tmh"
#include <gpio.h>
#include <reshub.h>

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, DeviceQueryDeviceParameters)
#endif

#define AP6256BT_DEFAULT_INITIAL_BAUD       115200UL
#define AP6256BT_DEFAULT_DOWNLOAD_BAUD      921600UL

#define AP6256BT_DEFAULT_OPERATIONAL_BAUD  3000000UL

#define AP6256BT_DEFAULT_POST_LAUNCH_MS        250UL
#define AP6256BT_DEFAULT_REG_ON_DELAY_MS       100UL
#define AP6256BT_MAX_FIRMWARE_BYTES       (2UL * 1024UL * 1024UL)
#define AP6256BT_MAX_HCD_RECORDS              8192UL
#define AP6256BT_H4_SYNC_LIMIT                  64UL

#define AP6256BT_TRACE_BYTES                    16UL
#define AP6256BT_IO_TIMEOUT_SECONDS               5UL

#define AP6256BT_IO_RETRIES                       3UL
#define AP6256BT_DEV_WAKE_HOLD_MS                 15UL
#define BCM_OPCODE_WRITE_RAM  0xFC4C
#define BCM_OPCODE_LAUNCH_RAM 0xFC4E
#define AP6256BT_FIRMWARE_DIRECTORY L"\\SystemRoot\\System32\\drivers\\"
#define AP6256BT_FIRMWARE_NAME L"BCM4345C5.hcd"

typedef struct _AP6256BT_CONFIG {
    ULONG InitialBaudRate;
    ULONG DownloadBaudRate;
    ULONG OperationalBaudRate;
    ULONG SkipFwDownload;
    ULONG PostLaunchDelayMs;
    ULONG DevWakeGpioIndex;
    ULONG RegOnGpioIndex;
    ULONG RegOnDelayMs;
    ULONG BtWakeActiveHigh;
    ULONG HostWakeActiveHigh;
    ULONG ConfigureSleepMode;
    ULONG ConfigureAudio;
    ULONG PCMClockMode;
    ULONG PCMFillMethod;
    ULONG PCMFillNum;
    ULONG PCMFillValue;
    ULONG PCMInCallBitclock;
    ULONG PCMLSBFirst;
    ULONG PCMRightJustify;
    ULONG PCMRouting;
    ULONG PCMShortFrameSync;
    ULONG PCMSyncMode;
    WCHAR FirmwareDirectoryBuffer[260];
    WCHAR RampatchFileNameBuffer[128];
    UNICODE_STRING FirmwareDirectory;
    UNICODE_STRING RampatchFileName;
} AP6256BT_CONFIG, *PAP6256BT_CONFIG;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AP6256BT_CONFIG, Ap6256BtGetConfig)

typedef struct _BCM_VERBOSE_CONFIG {
    UCHAR ChipId;
    UCHAR TargetId;
    USHORT BuildBase;
    USHORT BuildNumber;
} BCM_VERBOSE_CONFIG, *PBCM_VERBOSE_CONFIG;

static VOID
Ap6256BtDelayMilliseconds(_In_ ULONG Milliseconds)
{
    LARGE_INTEGER interval;
    interval.QuadPart = -((LONGLONG)Milliseconds * 10LL * 1000LL);
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

static NTSTATUS
Ap6256BtReuseRequest(_In_opt_ WDFREQUEST Request)
{
    WDF_REQUEST_REUSE_PARAMS params;

    if (Request == NULL) {
        return STATUS_SUCCESS;
    }

    WDF_REQUEST_REUSE_PARAMS_INIT(
        &params,
        WDF_REQUEST_REUSE_NO_FLAGS,
        STATUS_SUCCESS);
    return WdfRequestReuse(Request, &params);
}

static NTSTATUS
Ap6256BtSendIoctl(
    _In_ WDFIOTARGET Target,
    _In_opt_ WDFREQUEST Request,
    _In_ ULONG IoControlCode,
    _In_reads_bytes_opt_(InputLength) PVOID Input,
    _In_ ULONG InputLength,
    _Out_writes_bytes_opt_(OutputLength) PVOID Output,
    _In_ ULONG OutputLength,
    _Out_opt_ PULONG_PTR Information)
{
    NTSTATUS status;
    WDF_MEMORY_DESCRIPTOR inputDescriptor;
    WDF_MEMORY_DESCRIPTOR outputDescriptor;
    PWDF_MEMORY_DESCRIPTOR inputPointer = NULL;
    PWDF_MEMORY_DESCRIPTOR outputPointer = NULL;
    WDF_REQUEST_SEND_OPTIONS options;
    ULONG_PTR information = 0;

    status = Ap6256BtReuseRequest(Request);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if ((Input != NULL) && (InputLength != 0)) {
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
            &inputDescriptor, Input, InputLength);
        inputPointer = &inputDescriptor;
    }

    if ((Output != NULL) && (OutputLength != 0)) {
        RtlZeroMemory(Output, OutputLength);
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
            &outputDescriptor, Output, OutputLength);
        outputPointer = &outputDescriptor;
    }

    WDF_REQUEST_SEND_OPTIONS_INIT(
        &options,
        WDF_REQUEST_SEND_OPTION_SYNCHRONOUS |
        WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(
        &options,
        WDF_REL_TIMEOUT_IN_SEC(AP6256BT_IO_TIMEOUT_SECONDS));

    status = WdfIoTargetSendIoctlSynchronously(
        Target,
        Request,
        IoControlCode,
        inputPointer,
        outputPointer,
        &options,
        &information);

    if (NT_SUCCESS(status) && (Information != NULL)) {
        *Information = information;
    }

    return status;
}

static VOID
Ap6256BtTraceBytes(
    _In_ PCSTR Label,
    _In_reads_bytes_(Length) const UCHAR *Data,
    _In_ ULONG Length)
{
    static const CHAR digits[] = "0123456789ABCDEF";
    CHAR text[(AP6256BT_TRACE_BYTES * 3) + 1];
    ULONG shown;
    ULONG i;

    if (!BtLogEnabled(BT_LOG_VERBOSE) || Data == NULL) {
        return;
    }

    shown = min(Length, AP6256BT_TRACE_BYTES);
    for (i = 0; i < shown; i++) {
        text[(i * 3) + 0] = digits[(Data[i] >> 4) & 0x0F];
        text[(i * 3) + 1] = digits[Data[i] & 0x0F];
        text[(i * 3) + 2] = ' ';
    }
    text[shown * 3] = '\0';

    BtTraceVerbose("    %s (%u byte%s): %s%s\n",
            Label, Length, (Length == 1) ? "" : "s", text,
            (Length > shown) ? "..." : "");
}

static NTSTATUS
Ap6256BtWriteExact(
    _In_ WDFIOTARGET Target,
    _In_opt_ WDFREQUEST Request,
    _In_reads_bytes_(Length) const UCHAR *Data,
    _In_ ULONG Length)
{
    NTSTATUS status;
    ULONG offset = 0;

    while (offset < Length) {
        WDF_MEMORY_DESCRIPTOR descriptor;
        WDF_REQUEST_SEND_OPTIONS options;
        ULONG_PTR written = 0;

        status = Ap6256BtReuseRequest(Request);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
            &descriptor,
            (PVOID)(Data + offset),
            Length - offset);
        WDF_REQUEST_SEND_OPTIONS_INIT(
            &options,
            WDF_REQUEST_SEND_OPTION_SYNCHRONOUS |
            WDF_REQUEST_SEND_OPTION_TIMEOUT);
        WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(
            &options,
            WDF_REL_TIMEOUT_IN_SEC(AP6256BT_IO_TIMEOUT_SECONDS));

        status = WdfIoTargetSendWriteSynchronously(
            Target, Request, &descriptor, NULL, &options, &written);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        if ((written == 0) || (written > (Length - offset))) {
            return STATUS_DEVICE_DATA_ERROR;
        }
        offset += (ULONG)written;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
Ap6256BtReadExact(
    _In_ WDFIOTARGET Target,
    _In_opt_ WDFREQUEST Request,
    _Out_writes_bytes_(Length) UCHAR *Data,
    _In_ ULONG Length)
{
    NTSTATUS status;
    ULONG offset = 0;
    ULONG retries = 0;

    RtlZeroMemory(Data, Length);
    while (offset < Length) {
        WDF_MEMORY_DESCRIPTOR descriptor;
        WDF_REQUEST_SEND_OPTIONS options;
        ULONG_PTR read = 0;

        status = Ap6256BtReuseRequest(Request);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
            &descriptor, Data + offset, Length - offset);
        WDF_REQUEST_SEND_OPTIONS_INIT(
            &options,
            WDF_REQUEST_SEND_OPTION_SYNCHRONOUS |
            WDF_REQUEST_SEND_OPTION_TIMEOUT);
        WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(
            &options,
            WDF_REL_TIMEOUT_IN_SEC(AP6256BT_IO_TIMEOUT_SECONDS));

        status = WdfIoTargetSendReadSynchronously(
            Target, Request, &descriptor, NULL, &options, &read);

        if ((status == STATUS_TIMEOUT) ||
            (status == STATUS_IO_TIMEOUT) ||
            (NT_SUCCESS(status) && (read == 0))) {
            if (retries >= AP6256BT_IO_RETRIES) {
                return NT_SUCCESS(status) ? STATUS_DEVICE_DATA_ERROR : status;
            }
            retries++;
            continue;
        }

        if (!NT_SUCCESS(status)) {
            return status;
        }

        if (read > (Length - offset)) {
            return STATUS_DEVICE_DATA_ERROR;
        }

        offset += (ULONG)read;
        retries = 0;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
Ap6256BtReadHciEvent(
    _In_ WDFIOTARGET Target,
    _In_opt_ WDFREQUEST Request,
    _Out_writes_bytes_(Capacity) UCHAR *Event,
    _In_ ULONG Capacity,
    _Out_ PULONG EventLength)
{
    NTSTATUS status;
    UCHAR packetType = 0;
    UCHAR sync[AP6256BT_TRACE_BYTES];
    ULONG syncCount = 0;
    ULONG attempts;
    ULONG totalLength;

    if ((Event == NULL) || (EventLength == NULL) ||
        (Capacity < HCI_EVENT_HEADER_SIZE)) {
        return STATUS_INVALID_PARAMETER;
    }

    for (attempts = 0; attempts < AP6256BT_H4_SYNC_LIMIT; attempts++) {
        status = Ap6256BtReadExact(
            Target, Request, &packetType, sizeof(packetType));
        if (!NT_SUCCESS(status)) {
            BtTraceWarning("H4 sync: read failed 0x%08X after %u byte%s\n",
                    status, syncCount, (syncCount == 1) ? "" : "s");
            if (syncCount != 0) {
                Ap6256BtTraceBytes(
                    "saw", sync, min(syncCount, AP6256BT_TRACE_BYTES));
            }
            return status;
        }
        if (packetType == (UCHAR)HciPacketEvent) {
            break;
        }
        if (syncCount < AP6256BT_TRACE_BYTES) {
            sync[syncCount] = packetType;
        }
        syncCount++;
    }

    if (packetType != (UCHAR)HciPacketEvent) {
        BtTraceWarning("H4 sync: no 0x04 event marker in %u byte%s - the line is\n"
                "         carrying something, but not HCI (baud mismatch is\n"
                "         the usual cause)\n",
                syncCount, (syncCount == 1) ? "" : "s");
        Ap6256BtTraceBytes("saw", sync, min(syncCount, AP6256BT_TRACE_BYTES));
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    status = Ap6256BtReadExact(
        Target, Request, Event, HCI_EVENT_HEADER_SIZE);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    totalLength = HCI_EVENT_HEADER_SIZE + Event[1];
    if (totalLength > Capacity) {
        UCHAR discard[32];
        ULONG remaining = Event[1];

        while (remaining != 0) {
            ULONG chunkLength = min(remaining, (ULONG)sizeof(discard));
            status = Ap6256BtReadExact(
                Target, Request, discard, chunkLength);
            if (!NT_SUCCESS(status)) {
                return status;
            }
            remaining -= chunkLength;
        }
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (Event[1] != 0) {
        status = Ap6256BtReadExact(
            Target,
            Request,
            Event + HCI_EVENT_HEADER_SIZE,
            Event[1]);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    *EventLength = totalLength;
    return STATUS_SUCCESS;
}

static NTSTATUS
Ap6256BtVerifyCommandEvent(
    _In_reads_bytes_(CommandLength) const UCHAR *Command,
    _In_ ULONG CommandLength,
    _In_reads_bytes_(EventLength) const UCHAR *Event,
    _In_ ULONG EventLength)
{
    USHORT opcode;
    USHORT returnedOpcode;

    if ((Command == NULL) || (Event == NULL) ||
        (CommandLength < MIN_HCI_CMD_SIZE) ||
        (EventLength < 2) ||
        (EventLength != (ULONG)(HCI_EVENT_HEADER_SIZE + Event[1]))) {
        return STATUS_INVALID_PARAMETER;
    }

    opcode = (USHORT)(Command[0] | ((USHORT)Command[1] << 8));
    if (Event[0] == CommandComplete) {
        if (EventLength < 6) {
            return STATUS_DEVICE_PROTOCOL_ERROR;
        }
        returnedOpcode = (USHORT)(Event[3] | ((USHORT)Event[4] << 8));
        if ((returnedOpcode != opcode) || (Event[5] != 0)) {
            return STATUS_DEVICE_PROTOCOL_ERROR;
        }
        return STATUS_SUCCESS;
    }

    if (Event[0] == CommandStatus) {
        if (EventLength < 6) {
            return STATUS_DEVICE_PROTOCOL_ERROR;
        }
        returnedOpcode = (USHORT)(Event[4] | ((USHORT)Event[5] << 8));
        if ((returnedOpcode != opcode) || (Event[2] != 0)) {
            return STATUS_DEVICE_PROTOCOL_ERROR;
        }
        return STATUS_SUCCESS;
    }

    return STATUS_DEVICE_PROTOCOL_ERROR;
}

static NTSTATUS
Ap6256BtCommand(
    _In_ WDFIOTARGET Target,
    _In_opt_ WDFREQUEST Request,
    _In_reads_bytes_(CommandLength) const UCHAR *Command,
    _In_ ULONG CommandLength,
    _Out_writes_bytes_opt_(EventCapacity) UCHAR *ReturnedEvent,
    _In_ ULONG EventCapacity,
    _Out_opt_ PULONG ReturnedEventLength)
{
    UCHAR packet[MAX_HCI_CMD_SIZE + 1];
    UCHAR localEvent[MAX_HCI_EVENT_SIZE];
    UCHAR *event = localEvent;
    ULONG eventCapacity = sizeof(localEvent);
    ULONG eventLength = 0;
    USHORT opcode;
    NTSTATUS status;

    if ((Command == NULL) ||
        (CommandLength < MIN_HCI_CMD_SIZE) ||
        (CommandLength > MAX_HCI_CMD_SIZE) ||
        (Command[2] != (UCHAR)(CommandLength - MIN_HCI_CMD_SIZE))) {
        return STATUS_INVALID_PARAMETER;
    }

    if (ReturnedEvent != NULL) {
        if ((ReturnedEventLength == NULL) ||
            (EventCapacity < HCI_EVENT_HEADER_SIZE)) {
            return STATUS_INVALID_PARAMETER;
        }
        event = ReturnedEvent;
        eventCapacity = EventCapacity;
    }

    packet[0] = (UCHAR)HciPacketCommand;
    RtlCopyMemory(packet + 1, Command, CommandLength);
    opcode = (USHORT)(Command[0] | ((USHORT)Command[1] << 8));

    status = Ap6256BtWriteExact(
        Target, Request, packet, CommandLength + 1);
    if (!NT_SUCCESS(status)) {
        BtTraceError("HCI %04X: write failed 0x%08X - the UART would not take\n"
                "         the bytes, so nothing reached the chip\n",
                opcode, status);
        return status;
    }

    status = Ap6256BtReadHciEvent(
        Target, Request, event, eventCapacity, &eventLength);
    if (!NT_SUCCESS(status)) {
        BtTraceError("HCI %04X: sent, no usable reply 0x%08X - the controller did\n"
                "         not answer. If this is the first command after\n"
                "         LAUNCH_RAM it is still rebooting: raise\n"
                "         PostLaunchDelayMs. Otherwise the two ends disagree\n"
                "         about the baud rate, or BT_REG_ON went low.\n",
                opcode, status);
        return status;
    }

    status = Ap6256BtVerifyCommandEvent(
        Command, CommandLength, event, eventLength);
    if (!NT_SUCCESS(status)) {
        BtTraceError("HCI %04X: the chip replied but the event was rejected "
                "0x%08X\n", opcode, status);
        Ap6256BtTraceBytes("event", event, eventLength);
        return status;
    }

    if (ReturnedEventLength != NULL) {
        *ReturnedEventLength = eventLength;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
Ap6256BtSetHostBaud(
    _In_ WDFIOTARGET Target,
    _In_opt_ WDFREQUEST Request,
    _In_ ULONG BaudRate)
{
    SERIAL_BAUD_RATE baud;

    baud.BaudRate = BaudRate;
    return Ap6256BtSendIoctl(
        Target,
        Request,
        IOCTL_SERIAL_SET_BAUD_RATE,
        &baud,
        sizeof(baud),
        NULL,
        0,
        NULL);
}

static NTSTATUS
Ap6256BtConfigureHostUart(
    _In_ WDFIOTARGET Target,
    _In_opt_ WDFREQUEST Request,
    _In_ ULONG BaudRate)
{
    SERIAL_LINE_CONTROL line;
    SERIAL_HANDFLOW handflow;
    SERIAL_TIMEOUTS timeouts;
    ULONG purge;
    NTSTATUS status;

    RtlZeroMemory(&line, sizeof(line));
    line.StopBits = STOP_BIT_1;
    line.Parity = NO_PARITY;
    line.WordLength = 8;
    status = Ap6256BtSendIoctl(
        Target,
        Request,
        IOCTL_SERIAL_SET_LINE_CONTROL,
        &line,
        sizeof(line),
        NULL,
        0,
        NULL);

    if (!NT_SUCCESS(status)) {
        BtTraceError("uart: SET_LINE_CONTROL failed 0x%08X\n", status);
        return status;
    }

    RtlZeroMemory(&handflow, sizeof(handflow));
    handflow.ControlHandShake = SERIAL_CTS_HANDSHAKE;
    handflow.FlowReplace = SERIAL_RTS_HANDSHAKE;
    status = Ap6256BtSendIoctl(
        Target,
        Request,
        IOCTL_SERIAL_SET_HANDFLOW,
        &handflow,
        sizeof(handflow),
        NULL,
        0,
        NULL);
    if (!NT_SUCCESS(status)) {
        BtTraceError("uart: SET_HANDFLOW (CTS/RTS) failed 0x%08X\n", status);
        return status;
    }

    RtlZeroMemory(&timeouts, sizeof(timeouts));
    status = Ap6256BtSendIoctl(
        Target,
        Request,
        IOCTL_SERIAL_SET_TIMEOUTS,
        &timeouts,
        sizeof(timeouts),
        NULL,
        0,
        NULL);
    if (!NT_SUCCESS(status)) {
        BtTraceError("uart: SET_TIMEOUTS failed 0x%08X\n", status);
        return status;
    }

    status = Ap6256BtSetHostBaud(Target, Request, BaudRate);
    if (!NT_SUCCESS(status)) {
        BtTraceError("uart: SET_BAUD_RATE %u failed 0x%08X\n", BaudRate, status);
        return status;
    }

    purge = SERIAL_PURGE_RXABORT |
            SERIAL_PURGE_RXCLEAR |
            SERIAL_PURGE_TXABORT |
            SERIAL_PURGE_TXCLEAR;
    status = Ap6256BtSendIoctl(
        Target,
        Request,
        IOCTL_SERIAL_PURGE,
        &purge,
        sizeof(purge),
        NULL,
        0,
        NULL);
    if (!NT_SUCCESS(status)) {
        BtTraceWarning("uart: PURGE failed 0x%08X\n", status);
        return status;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
BcmHciReset(
    _In_ WDFIOTARGET Target,
    _In_opt_ WDFREQUEST Request)
{
    const UCHAR command[] = { 0x03, 0x0C, 0x00 };

    return Ap6256BtCommand(
        Target, Request, command, sizeof(command), NULL, 0, NULL);
}

static NTSTATUS
BcmGetVerboseConfig(
    _In_ WDFIOTARGET Target,
    _In_opt_ WDFREQUEST Request,
    _Out_ PBCM_VERBOSE_CONFIG Config)
{
    const UCHAR command[] = { 0x79, 0xFC, 0x00 };
    UCHAR event[MAX_HCI_EVENT_SIZE];
    ULONG eventLength = 0;
    NTSTATUS status;

    RtlZeroMemory(Config, sizeof(*Config));
    status = Ap6256BtCommand(
        Target,
        Request,
        command,
        sizeof(command),
        event,
        sizeof(event),
        &eventLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (eventLength < 12) {
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    Config->ChipId = event[6];
    Config->TargetId = event[7];
    Config->BuildBase = (USHORT)(event[8] | ((USHORT)event[9] << 8));
    Config->BuildNumber = (USHORT)(event[10] | ((USHORT)event[11] << 8));
    DoTrace(
        LEVEL_INFO,
        TFLAG_HCI,
        ("BCM verbose config chip=%u target=%u base=%u build=%u",
         Config->ChipId,
         Config->TargetId,
         Config->BuildBase,
         Config->BuildNumber));
    return STATUS_SUCCESS;
}

static NTSTATUS
BcmSetControllerClock48Mhz(
    _In_ WDFIOTARGET Target,
    _In_opt_ WDFREQUEST Request)
{
    const UCHAR command[] = { 0x45, 0xFC, 0x01, 0x01 };

    return Ap6256BtCommand(
        Target, Request, command, sizeof(command), NULL, 0, NULL);
}

static NTSTATUS
BcmSetControllerBaud(
    _In_ WDFIOTARGET Target,
    _In_opt_ WDFREQUEST Request,
    _In_ ULONG BaudRate)
{
    UCHAR command[] = {
        0x18, 0xFC, 0x06,
        0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };

    command[5] = (UCHAR)BaudRate;
    command[6] = (UCHAR)(BaudRate >> 8);
    command[7] = (UCHAR)(BaudRate >> 16);
    command[8] = (UCHAR)(BaudRate >> 24);
    return Ap6256BtCommand(
        Target, Request, command, sizeof(command), NULL, 0, NULL);
}

static NTSTATUS
BcmSetBaudPair(
    _In_ WDFIOTARGET Target,
    _In_opt_ WDFREQUEST Request,
    _In_ ULONG BaudRate)
{
    NTSTATUS status;

    if (BaudRate > 3000000UL) {
        status = BcmSetControllerClock48Mhz(Target, Request);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    status = BcmSetControllerBaud(Target, Request, BaudRate);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = Ap6256BtSetHostBaud(Target, Request, BaudRate);
    if (NT_SUCCESS(status)) {
        Ap6256BtDelayMilliseconds(20);
    }
    return status;
}

static NTSTATUS
BcmConfigureSleep(
    _In_ WDFIOTARGET Target,
    _In_opt_ WDFREQUEST Request,
    _In_ PAP6256BT_CONFIG Config)
{
    UCHAR command[] = {
        0x27, 0xFC, 0x0C,
        0x01, 0x02, 0x02, 0x01, 0x01, 0x01,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    command[6] = (UCHAR)(Config->BtWakeActiveHigh != 0);
    command[7] = (UCHAR)(Config->HostWakeActiveHigh != 0);
    return Ap6256BtCommand(
        Target, Request, command, sizeof(command), NULL, 0, NULL);
}

static NTSTATUS
BcmReadPcmInterface(
    _In_ WDFIOTARGET Target,
    _In_opt_ WDFREQUEST Request,
    _Out_writes_(5) UCHAR Parameters[5])
{
    const UCHAR command[] = {
        0x1D,
        0xFC,
        0x00
    };
    UCHAR event[MAX_HCI_EVENT_SIZE];
    ULONG eventLength = 0;
    NTSTATUS status;

    RtlZeroMemory(Parameters, 5);
    status = Ap6256BtCommand(
        Target,
        Request,
        command,
        sizeof(command),
        event,
        sizeof(event),
        &eventLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if ((event[0] != CommandComplete) || (eventLength < 11)) {
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    RtlCopyMemory(Parameters, event + 6, 5);
    return STATUS_SUCCESS;
}

static NTSTATUS
BcmWritePcmInterface(
    _In_ WDFIOTARGET Target,
    _In_opt_ WDFREQUEST Request,
    _In_reads_(5) const UCHAR Parameters[5])
{
    UCHAR command[] = {
        0x1C,
        0xFC,
        0x05,
        0, 0, 0, 0, 0
    };

    RtlCopyMemory(command + 3, Parameters, 5);
    return Ap6256BtCommand(
        Target, Request, command, sizeof(command), NULL, 0, NULL);
}

static NTSTATUS
BcmConfigurePcmAudio(
    _In_ WDFIOTARGET Target,
    _In_opt_ WDFREQUEST Request,
    _In_ PAP6256BT_CONFIG Config)
{
    UCHAR current[5];
    UCHAR requestedInterface[5];
    UCHAR formatCommand[] = {
        0x1E,
        0xFC,
        0x05,
        0, 0, 0, 0, 0
    };
    NTSTATUS status;

    requestedInterface[0] = (UCHAR)Config->PCMRouting;
    requestedInterface[1] = (UCHAR)Config->PCMInCallBitclock;
    requestedInterface[2] = (UCHAR)Config->PCMShortFrameSync;
    requestedInterface[3] = (UCHAR)Config->PCMSyncMode;
    requestedInterface[4] = (UCHAR)Config->PCMClockMode;

    status = BcmReadPcmInterface(Target, Request, current);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = BcmWritePcmInterface(Target, Request, requestedInterface);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = BcmReadPcmInterface(Target, Request, current);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (RtlCompareMemory(current, requestedInterface, sizeof(current)) !=
        sizeof(current)) {
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    formatCommand[3] = (UCHAR)Config->PCMLSBFirst;
    formatCommand[4] = (UCHAR)Config->PCMFillValue;
    formatCommand[5] = (UCHAR)Config->PCMFillMethod;
    formatCommand[6] = (UCHAR)Config->PCMFillNum;
    formatCommand[7] = (UCHAR)Config->PCMRightJustify;
    return Ap6256BtCommand(
        Target,
        Request,
        formatCommand,
        sizeof(formatCommand),
        NULL,
        0,
        NULL);
}

static NTSTATUS
Ap6256BtReadFileExact(
    _In_ HANDLE FileHandle,
    _Inout_ PLARGE_INTEGER Offset,
    _Out_writes_bytes_(Length) UCHAR *Buffer,
    _In_ ULONG Length)
{
    IO_STATUS_BLOCK ioStatus;
    NTSTATUS status;

    status = ZwReadFile(
        FileHandle,
        NULL,
        NULL,
        NULL,
        &ioStatus,
        Buffer,
        Length,
        Offset,
        NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (ioStatus.Information != Length) {
        return STATUS_END_OF_FILE;
    }

    Offset->QuadPart += Length;
    return STATUS_SUCCESS;
}

static NTSTATUS
BcmDownloadHcd(
    _In_ WDFIOTARGET Target,
    _In_opt_ WDFREQUEST Request,
    _In_ PCUNICODE_STRING FirmwarePath,
    _In_ ULONG PostLaunchDelayMs)
{
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK ioStatus;
    FILE_STANDARD_INFORMATION fileInfo;
    HANDLE fileHandle = NULL;
    LARGE_INTEGER offset;
    UCHAR record[MAX_HCI_CMD_SIZE];
    ULONG recordCount = 0;
    BOOLEAN launchSeen = FALSE;
    NTSTATUS status;
    const UCHAR downloadCommand[] = { 0x2E, 0xFC, 0x00 };

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    RtlZeroMemory(&fileInfo, sizeof(fileInfo));
    InitializeObjectAttributes(
        &attributes,
        (PUNICODE_STRING)FirmwarePath,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL,
        NULL);
    status = ZwCreateFile(
        &fileHandle,
        GENERIC_READ | SYNCHRONIZE,
        &attributes,
        &ioStatus,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ,
        FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
        NULL,
        0);
    if (!NT_SUCCESS(status)) {
        DoTrace(
            LEVEL_ERROR,
            TFLAG_HCI,
            ("Opening HCD %wZ failed %!STATUS!", FirmwarePath, status));
        return status;
    }

    status = ZwQueryInformationFile(
        fileHandle,
        &ioStatus,
        &fileInfo,
        sizeof(fileInfo),
        FileStandardInformation);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    if ((fileInfo.EndOfFile.QuadPart < MIN_HCI_CMD_SIZE) ||
        (fileInfo.EndOfFile.QuadPart > AP6256BT_MAX_FIRMWARE_BYTES)) {
        status = STATUS_INVALID_IMAGE_FORMAT;
        goto Exit;
    }

    status = Ap6256BtCommand(
        Target,
        Request,
        downloadCommand,
        sizeof(downloadCommand),
        NULL,
        0,
        NULL);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    Ap6256BtDelayMilliseconds(50);
    offset.QuadPart = 0;

    while (offset.QuadPart < fileInfo.EndOfFile.QuadPart) {
        ULONG payloadLength;
        ULONG recordLength;
        USHORT opcode;
        LONGLONG remaining;

        if (++recordCount > AP6256BT_MAX_HCD_RECORDS) {
            status = STATUS_INVALID_IMAGE_FORMAT;
            goto Exit;
        }

        remaining = fileInfo.EndOfFile.QuadPart - offset.QuadPart;
        if (remaining < MIN_HCI_CMD_SIZE) {
            status = STATUS_INVALID_IMAGE_FORMAT;
            goto Exit;
        }

        status = Ap6256BtReadFileExact(
            fileHandle, &offset, record, MIN_HCI_CMD_SIZE);
        if (!NT_SUCCESS(status)) {
            goto Exit;
        }

        payloadLength = record[2];
        recordLength = MIN_HCI_CMD_SIZE + payloadLength;
        if ((fileInfo.EndOfFile.QuadPart - offset.QuadPart) <
            payloadLength) {
            status = STATUS_INVALID_IMAGE_FORMAT;
            goto Exit;
        }

        if (payloadLength != 0) {
            status = Ap6256BtReadFileExact(
                fileHandle,
                &offset,
                record + MIN_HCI_CMD_SIZE,
                payloadLength);
            if (!NT_SUCCESS(status)) {
                goto Exit;
            }
        }

        opcode = (USHORT)(record[0] | ((USHORT)record[1] << 8));
        if (opcode == BCM_OPCODE_LAUNCH_RAM) {
            if ((payloadLength != 4) ||
                (offset.QuadPart != fileInfo.EndOfFile.QuadPart)) {
                status = STATUS_INVALID_IMAGE_FORMAT;
                goto Exit;
            }
            launchSeen = TRUE;
        } else if ((opcode != BCM_OPCODE_WRITE_RAM) || launchSeen) {
            status = STATUS_INVALID_IMAGE_FORMAT;
            goto Exit;
        }

        status = Ap6256BtCommand(
            Target,
            Request,
            record,
            recordLength,
            NULL,
            0,
            NULL);
        if (!NT_SUCCESS(status)) {
            goto Exit;
        }
    }

    if (!launchSeen || (recordCount < 2)) {
        status = STATUS_INVALID_IMAGE_FORMAT;
        goto Exit;
    }

    Ap6256BtDelayMilliseconds(PostLaunchDelayMs);
    status = STATUS_SUCCESS;

Exit:
    ZwClose(fileHandle);
    DoTrace(
        NT_SUCCESS(status) ? LEVEL_INFO : LEVEL_ERROR,
        TFLAG_HCI,
        ("HCD records=%u bytes=%I64d status=%!STATUS!",
         recordCount,
         fileInfo.EndOfFile.QuadPart,
         status));
    return status;
}

static VOID
Ap6256BtQueryULong(
    _In_ WDFKEY Key,
    _In_z_ PCWSTR Name,
    _Inout_ PULONG Value)
{
    UNICODE_STRING valueName;
    ULONG queriedValue;

    RtlInitUnicodeString(&valueName, Name);
    if (NT_SUCCESS(
            WdfRegistryQueryULong(Key, &valueName, &queriedValue))) {
        *Value = queriedValue;
    }
}

static VOID
Ap6256BtQueryString(
    _In_ WDFKEY Key,
    _In_z_ PCWSTR Name,
    _Inout_ PUNICODE_STRING Value)
{
    UNICODE_STRING valueName;

    RtlInitUnicodeString(&valueName, Name);
    (VOID)WdfRegistryQueryUnicodeString(
        Key, &valueName, NULL, Value);
}

VOID
DeviceQueryDeviceParameters(_In_ WDFDRIVER Driver)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    PAP6256BT_CONFIG config;
    WDFKEY key;
    NTSTATUS status;

    PAGED_CODE();

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
        &attributes,
        AP6256BT_CONFIG);
    status = WdfObjectAllocateContext(
        Driver,
        &attributes,
        (PVOID *)&config);
    if (!NT_SUCCESS(status)) {
        DoTrace(
            LEVEL_ERROR,
            TFLAG_PNP,
            ("Allocating AP6256BT config failed %!STATUS!", status));
        return;
    }

    RtlZeroMemory(config, sizeof(*config));
    config->InitialBaudRate = AP6256BT_DEFAULT_INITIAL_BAUD;
    config->DownloadBaudRate = AP6256BT_DEFAULT_DOWNLOAD_BAUD;
    config->OperationalBaudRate = AP6256BT_DEFAULT_OPERATIONAL_BAUD;
    config->PostLaunchDelayMs = AP6256BT_DEFAULT_POST_LAUNCH_MS;
    config->DevWakeGpioIndex = 0;
    config->RegOnGpioIndex = 1;
    config->RegOnDelayMs = AP6256BT_DEFAULT_REG_ON_DELAY_MS;
    config->BtWakeActiveHigh = 1;
    config->HostWakeActiveHigh = 1;
    config->PCMFillMethod = 2;
    config->PCMFillValue = 3;

    RtlInitEmptyUnicodeString(
        &config->FirmwareDirectory,
        config->FirmwareDirectoryBuffer,
        sizeof(config->FirmwareDirectoryBuffer));
    RtlInitEmptyUnicodeString(
        &config->RampatchFileName,
        config->RampatchFileNameBuffer,
        sizeof(config->RampatchFileNameBuffer));

    status = RtlUnicodeStringCopyString(
        &config->FirmwareDirectory,
        AP6256BT_FIRMWARE_DIRECTORY);
    if (!NT_SUCCESS(status)) {
        config->InitialBaudRate = 0;
        return;
    }

    status = RtlUnicodeStringCopyString(
        &config->RampatchFileName,
        AP6256BT_FIRMWARE_NAME);
    if (!NT_SUCCESS(status)) {
        config->InitialBaudRate = 0;
        return;
    }

    status = WdfDriverOpenParametersRegistryKey(
        Driver,
        GENERIC_READ,
        WDF_NO_OBJECT_ATTRIBUTES,
        &key);
    if (NT_SUCCESS(status)) {
        Ap6256BtQueryULong(
            key, L"InitialBaudRate", &config->InitialBaudRate);
        Ap6256BtQueryULong(
            key, L"DownloadBaudRate", &config->DownloadBaudRate);
        Ap6256BtQueryULong(
            key, L"OperationalBaudRate", &config->OperationalBaudRate);
        Ap6256BtQueryULong(
            key, L"SkipFwDownload", &config->SkipFwDownload);
        Ap6256BtQueryULong(
            key, L"PostLaunchDelayMs", &config->PostLaunchDelayMs);
        Ap6256BtQueryULong(
            key, L"DevWakeGpioIndex", &config->DevWakeGpioIndex);
        Ap6256BtQueryULong(
            key, L"RegOnGpioIndex", &config->RegOnGpioIndex);
        Ap6256BtQueryULong(
            key, L"RegOnDelayMs", &config->RegOnDelayMs);
        Ap6256BtQueryULong(
            key, L"BtWakeActiveHigh", &config->BtWakeActiveHigh);
        Ap6256BtQueryULong(
            key, L"HostWakeActiveHigh", &config->HostWakeActiveHigh);
        Ap6256BtQueryULong(
            key, L"ConfigureSleepMode", &config->ConfigureSleepMode);
        Ap6256BtQueryULong(
            key, L"ConfigureAudio", &config->ConfigureAudio);
        Ap6256BtQueryULong(
            key, L"PCMClockMode", &config->PCMClockMode);
        Ap6256BtQueryULong(
            key, L"PCMFillMethod", &config->PCMFillMethod);
        Ap6256BtQueryULong(
            key, L"PCMFillNum", &config->PCMFillNum);
        Ap6256BtQueryULong(
            key, L"PCMFillValue", &config->PCMFillValue);
        Ap6256BtQueryULong(
            key, L"PCMInCallBitclock", &config->PCMInCallBitclock);
        Ap6256BtQueryULong(
            key, L"PCMLSBFirst", &config->PCMLSBFirst);
        Ap6256BtQueryULong(
            key, L"PCMRightJustify", &config->PCMRightJustify);
        Ap6256BtQueryULong(
            key, L"PCMRouting", &config->PCMRouting);
        Ap6256BtQueryULong(
            key, L"PCMShortFrameSync", &config->PCMShortFrameSync);
        Ap6256BtQueryULong(
            key, L"PCMSyncMode", &config->PCMSyncMode);
        Ap6256BtQueryString(
            key, L"FirmwareDirectory", &config->FirmwareDirectory);
        Ap6256BtQueryString(
            key, L"RAMPatchFileName", &config->RampatchFileName);
        WdfRegistryClose(key);
    }

    if ((config->InitialBaudRate == 0) ||
        (config->DownloadBaudRate == 0) ||
        (config->OperationalBaudRate == 0) ||
        (config->OperationalBaudRate > 4000000UL) ||
        (config->PostLaunchDelayMs > 5000UL) ||
        (config->RegOnDelayMs > 5000UL) ||
        (config->BtWakeActiveHigh > 1) ||
        (config->HostWakeActiveHigh > 1) ||
        (config->ConfigureSleepMode > 1) ||
        (config->ConfigureAudio > 1) ||
        (config->PCMClockMode > MAXUCHAR) ||
        (config->PCMFillMethod > MAXUCHAR) ||
        (config->PCMFillNum > MAXUCHAR) ||
        (config->PCMFillValue > MAXUCHAR) ||
        (config->PCMInCallBitclock > MAXUCHAR) ||
        (config->PCMLSBFirst > MAXUCHAR) ||
        (config->PCMRightJustify > MAXUCHAR) ||
        (config->PCMRouting > MAXUCHAR) ||
        (config->PCMShortFrameSync > MAXUCHAR) ||
        (config->PCMSyncMode > MAXUCHAR) ||
        (config->FirmwareDirectory.Length == 0) ||
        (config->RampatchFileName.Length == 0)) {
        DoTrace(
            LEVEL_ERROR,
            TFLAG_PNP,
            ("Invalid AP6256BT service parameters"));
        config->InitialBaudRate = 0;
    }
}

static NTSTATUS
Ap6256BtOpenGpio(
    _In_ WDFDEVICE Device,
    _In_ LARGE_INTEGER ConnectionId,
    _Out_ WDFIOTARGET *Target)
{
    WCHAR pathBuffer[RESOURCE_HUB_PATH_CHARS];
    UNICODE_STRING path;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_TARGET_OPEN_PARAMS openParams;
    WDFIOTARGET target = NULL;
    NTSTATUS status;

    RtlInitEmptyUnicodeString(
        &path,
        pathBuffer,
        sizeof(pathBuffer));
    status = RESOURCE_HUB_CREATE_PATH_FROM_ID(
        &path,
        ConnectionId.LowPart,
        ConnectionId.HighPart);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Device;
    status = WdfIoTargetCreate(Device, &attributes, &target);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(
        &openParams,
        &path,
        FILE_GENERIC_WRITE);
    status = WdfIoTargetOpen(target, &openParams);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(target);
        return status;
    }

    *Target = target;
    return STATUS_SUCCESS;
}

static NTSTATUS
Ap6256BtWriteGpio(
    _In_ WDFIOTARGET Target,
    _In_ BOOLEAN High)
{
    UCHAR value = High ? 1 : 0;
    WDF_MEMORY_DESCRIPTOR descriptor;
    WDF_REQUEST_SEND_OPTIONS options;

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
        &descriptor,
        &value,
        sizeof(value));
    WDF_REQUEST_SEND_OPTIONS_INIT(
        &options,
        WDF_REQUEST_SEND_OPTION_SYNCHRONOUS |
        WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(
        &options,
        WDF_REL_TIMEOUT_IN_SEC(AP6256BT_IO_TIMEOUT_SECONDS));

    return WdfIoTargetSendIoctlSynchronously(
        Target,
        NULL,
        IOCTL_GPIO_WRITE_PINS,
        &descriptor,
        &descriptor,
        &options,
        NULL);
}

NTSTATUS
DeviceEnable(
    _In_ WDFDEVICE Device,
    _In_ BOOLEAN IsEnabled)
{
    PFDO_EXTENSION extension = FdoGetExtension(Device);
    PAP6256BT_CONFIG config = Ap6256BtGetConfig(WdfGetDriver());
    NTSTATUS status;

    if ((config == NULL) ||
        (config->InitialBaudRate == 0) ||
        (config->DevWakeGpioIndex >= extension->GPIOConnectionCount) ||
        (config->RegOnGpioIndex >= extension->GPIOConnectionCount) ||
        (config->DevWakeGpioIndex == config->RegOnGpioIndex)) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    if (extension->IoTargetRegOn == NULL) {
        status = Ap6256BtOpenGpio(
            Device,
            extension->GPIOConnectionIds[config->RegOnGpioIndex],
            &extension->IoTargetRegOn);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    if (extension->IoTargetDevWake == NULL) {
        status = Ap6256BtOpenGpio(
            Device,
            extension->GPIOConnectionIds[config->DevWakeGpioIndex],
            &extension->IoTargetDevWake);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    if (IsEnabled) {
        status = Ap6256BtWriteGpio(
            extension->IoTargetRegOn,
            TRUE);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        Ap6256BtDelayMilliseconds(config->RegOnDelayMs);
        return Ap6256BtWriteGpio(
            extension->IoTargetDevWake,
            (BOOLEAN)(config->BtWakeActiveHigh != 0));
    }

    status = Ap6256BtWriteGpio(
        extension->IoTargetDevWake,
        (BOOLEAN)(config->BtWakeActiveHigh == 0));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return Ap6256BtWriteGpio(
        extension->IoTargetRegOn,
        FALSE);
}

NTSTATUS
DevicePowerOn(_In_ WDFDEVICE Device)
{
    PFDO_EXTENSION extension = FdoGetExtension(Device);
    PAP6256BT_CONFIG config = Ap6256BtGetConfig(WdfGetDriver());

    if ((config == NULL) || (extension->IoTargetDevWake == NULL)) {
        return STATUS_SUCCESS;
    }

    return Ap6256BtWriteGpio(
        extension->IoTargetDevWake,
        (BOOLEAN)(config->BtWakeActiveHigh != 0));
}

NTSTATUS
DevicePowerOff(_In_ WDFDEVICE Device)
{
    PFDO_EXTENSION extension = FdoGetExtension(Device);
    PAP6256BT_CONFIG config = Ap6256BtGetConfig(WdfGetDriver());

    if ((config == NULL) || (extension->IoTargetDevWake == NULL)) {
        return STATUS_SUCCESS;
    }

    return Ap6256BtWriteGpio(
        extension->IoTargetDevWake,
        (BOOLEAN)(config->BtWakeActiveHigh == 0));
}

BOOLEAN
DeviceSleepModeRequested(VOID)
{
    PAP6256BT_CONFIG config = Ap6256BtGetConfig(WdfGetDriver());

    return (BOOLEAN)((config != NULL) &&
                     (config->ConfigureSleepMode != 0));
}

NTSTATUS
DeviceAcquireTxWake(
    _In_ PFDO_EXTENSION FdoExtension,
    _Out_ PBOOLEAN WakeReferenceHeld)
{
    NTSTATUS status;
    LONG references;

    *WakeReferenceHeld = FALSE;
    if (!DeviceSleepModeRequested()) {
        return STATUS_SUCCESS;
    }
    if (!FdoExtension->HostWakeOperational ||
        (FdoExtension->PowerLock == NULL)) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    status = WdfWaitLockAcquire(FdoExtension->PowerLock, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if ((InterlockedCompareExchange(
             &FdoExtension->ResetInProgress, 0, 0) != 0) ||
        (InterlockedCompareExchange(
             &FdoExtension->PowerTransitionInProgress, 0, 0) != 0) ||
        !IsDeviceInitialized(FdoExtension)) {
        status = STATUS_DEVICE_NOT_READY;
        goto Exit;
    }

    references = InterlockedIncrement(&FdoExtension->TxWakeReferences);
    if (references == 1) {
        status = DevicePowerOn(FdoExtension->WdfDevice);
        if (!NT_SUCCESS(status)) {
            (VOID)InterlockedDecrement(&FdoExtension->TxWakeReferences);
            goto Exit;
        }
    }

    *WakeReferenceHeld = TRUE;
    status = STATUS_SUCCESS;

Exit:
    WdfWaitLockRelease(FdoExtension->PowerLock);
    return status;
}

VOID
DeviceScheduleSleep(_In_ PFDO_EXTENSION FdoExtension)
{
    if (DeviceSleepModeRequested() &&
        FdoExtension->HostWakeOperational &&
        (FdoExtension->DevWakeReleaseWorkItem != NULL)) {
        WdfWorkItemEnqueue(FdoExtension->DevWakeReleaseWorkItem);
    }
}

VOID
DeviceReleaseTxWake(_In_ PFDO_EXTENSION FdoExtension)
{
    LONG references;

    if (!DeviceSleepModeRequested()) {
        return;
    }

    references = InterlockedDecrement(&FdoExtension->TxWakeReferences);
    if (references < 0) {
        InterlockedExchange(&FdoExtension->TxWakeReferences, 0);
        DoTrace(
            LEVEL_ERROR,
            TFLAG_POWER,
            ("Unbalanced BT_DEV_WAKE reference"));
        return;
    }

    if (references == 0) {
        DeviceScheduleSleep(FdoExtension);
    }
}

VOID
DeviceDevWakeReleaseWorker(_In_ WDFWORKITEM WorkItem)
{
    WDFDEVICE device =
        (WDFDEVICE)WdfWorkItemGetParentObject(WorkItem);
    PFDO_EXTENSION extension = FdoGetExtension(device);
    NTSTATUS status;

    Ap6256BtDelayMilliseconds(AP6256BT_DEV_WAKE_HOLD_MS);
    status = WdfWaitLockAcquire(extension->PowerLock, NULL);
    if (!NT_SUCCESS(status)) {
        return;
    }

    if ((InterlockedCompareExchange(
             &extension->TxWakeReferences, 0, 0) == 0) &&
        (InterlockedCompareExchange(
             &extension->ResetInProgress, 0, 0) == 0) &&
        (InterlockedCompareExchange(
             &extension->PowerTransitionInProgress, 0, 0) == 0) &&
        IsDeviceInitialized(extension) &&
        DeviceSleepModeRequested() &&
        extension->HostWakeOperational) {
        status = DevicePowerOff(device);
        if (!NT_SUCCESS(status)) {
            DoTrace(
                LEVEL_ERROR,
                TFLAG_POWER,
                ("Deasserting BT_DEV_WAKE failed %!STATUS!", status));
        }
    }

    WdfWaitLockRelease(extension->PowerLock);
}

NTSTATUS
DeviceEnableWakeControl(
    _In_ WDFDEVICE Device,
    _In_ SYSTEM_POWER_STATE PowerState)
{
    PFDO_EXTENSION extension = FdoGetExtension(Device);

    UNREFERENCED_PARAMETER(PowerState);

    if (!DeviceSleepModeRequested() ||
        !extension->HostWakeOperational ||
        !IsDeviceInitialized(extension)) {
        return STATUS_NOT_SUPPORTED;
    }

    InterlockedExchange(&extension->WakeArmed, 1);
    return STATUS_SUCCESS;
}

VOID
DeviceDisableWakeControl(WDFDEVICE Device)
{
    PFDO_EXTENSION extension = FdoGetExtension(Device);

    InterlockedExchange(&extension->WakeArmed, 0);
}

BOOLEAN
DeviceInitialize(
    _In_ PFDO_EXTENSION FdoExtension,
    _In_ WDFIOTARGET IoTargetSerial,
    _In_ WDFREQUEST RequestSync,
    _In_ BOOLEAN IsUartReset)
{
    PAP6256BT_CONFIG config = Ap6256BtGetConfig(WdfGetDriver());
    BCM_VERBOSE_CONFIG verboseConfig;
    WCHAR firmwarePathBuffer[512];
    WCHAR ntFirmwarePathBuffer[516];
    UNICODE_STRING firmwarePath;
    UNICODE_STRING ntFirmwarePath;
    PCUNICODE_STRING downloadPath;
    NTSTATUS status;
    ULONG currentBaud;

    UNREFERENCED_PARAMETER(IsUartReset);

    if ((config == NULL) || (config->InitialBaudRate == 0)) {
        BtTraceError("DeviceInitialize: no usable config (config %p)\n", config);
        return FALSE;
    }

    BtTrace("DeviceInitialize: baud init %u / download %u / operational %u, "
            "skipFw %u, audio %u, sleep %u\n",
            config->InitialBaudRate, config->DownloadBaudRate,
            config->OperationalBaudRate, config->SkipFwDownload,
            config->ConfigureAudio, config->ConfigureSleepMode);

    currentBaud = config->InitialBaudRate;
    status = Ap6256BtConfigureHostUart(
        IoTargetSerial,
        RequestSync,
        config->InitialBaudRate);
    BtTraceStatus(status, "host uart @ %u baud -> 0x%08X\n", config->InitialBaudRate, status);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    status = BcmHciReset(IoTargetSerial, RequestSync);
    BtTraceStatus(status, "HCI reset -> 0x%08X%s\n", status,
            NT_SUCCESS(status) ? "" :
                " (no reply: check BT_REG_ON, the patchram baud, or RTS/CTS)");
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    RtlZeroMemory(&verboseConfig, sizeof(verboseConfig));
    status = BcmGetVerboseConfig(
        IoTargetSerial,
        RequestSync,
        &verboseConfig);
    BtTraceStatus(status, "read verbose config -> 0x%08X (build %u)\n",
            status, verboseConfig.BuildNumber);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    if ((config->SkipFwDownload == 0) &&
        (verboseConfig.BuildNumber == 0)) {
        RtlInitEmptyUnicodeString(
            &firmwarePath,
            firmwarePathBuffer,
            sizeof(firmwarePathBuffer));

        status = RtlUnicodeStringCopy(
            &firmwarePath,
            &config->FirmwareDirectory);
        if (NT_SUCCESS(status)) {
            status = RtlUnicodeStringCat(
                &firmwarePath,
                &config->RampatchFileName);
        }
        if (!NT_SUCCESS(status)) {
            goto Exit;
        }

        downloadPath = &firmwarePath;
        if ((firmwarePath.Length >= (2 * sizeof(WCHAR))) &&
            (firmwarePath.Buffer[1] == L':')) {
            RtlInitEmptyUnicodeString(
                &ntFirmwarePath,
                ntFirmwarePathBuffer,
                sizeof(ntFirmwarePathBuffer));
            status = RtlUnicodeStringCopyString(
                &ntFirmwarePath,
                L"\\??\\");
            if (NT_SUCCESS(status)) {
                status = RtlUnicodeStringCat(
                    &ntFirmwarePath,
                    &firmwarePath);
            }
            if (!NT_SUCCESS(status)) {
                goto Exit;
            }
            downloadPath = &ntFirmwarePath;
        }

        if (config->DownloadBaudRate != config->InitialBaudRate) {
            status = BcmSetBaudPair(
                IoTargetSerial,
                RequestSync,
                config->DownloadBaudRate);
            if (!NT_SUCCESS(status)) {
                goto Exit;
            }
            currentBaud = config->DownloadBaudRate;
        }

        BtTrace("downloading patchram from '%wZ'\n", downloadPath);
        status = BcmDownloadHcd(
            IoTargetSerial,
            RequestSync,
            downloadPath,
            config->PostLaunchDelayMs);
        BtTraceStatus(status, "patchram download -> 0x%08X\n", status);
        if (!NT_SUCCESS(status)) {
            goto Exit;
        }

        status = Ap6256BtConfigureHostUart(
            IoTargetSerial,
            RequestSync,
            config->InitialBaudRate);
        BtTraceStatus(status, "post-patchram resync @ %u baud -> 0x%08X\n",
                config->InitialBaudRate, status);
        if (!NT_SUCCESS(status)) {
            goto Exit;
        }
        currentBaud = config->InitialBaudRate;
    }

    if (config->OperationalBaudRate != currentBaud) {
        status = BcmSetBaudPair(
            IoTargetSerial,
            RequestSync,
            config->OperationalBaudRate);
        BtTraceStatus(status, "operational baud %u -> 0x%08X\n",
                config->OperationalBaudRate, status);
        if (!NT_SUCCESS(status)) {
            goto Exit;
        }
        currentBaud = config->OperationalBaudRate;
    }

    status = BcmHciReset(IoTargetSerial, RequestSync);
    BtTraceStatus(status, "post-config HCI reset -> 0x%08X\n", status);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    if (config->ConfigureAudio != 0) {
        status = BcmConfigurePcmAudio(
            IoTargetSerial,
            RequestSync,
            config);
        BtTraceStatus(status, "PCM audio config -> 0x%08X\n", status);
        if (!NT_SUCCESS(status)) {
            goto Exit;
        }
    }

    if (config->ConfigureSleepMode != 0) {
        if (!FdoExtension->HostWakeOperational) {
            BtTraceWarning("sleep mode requested but HOST_WAKE is not operational -\n"
                    "         continuing without it. The ASL GpioInt needs\n"
                    "         ExclusiveAndWake for the wake hint to reach us.\n");
            config->ConfigureSleepMode = 0;
        }
    }

    if (config->ConfigureSleepMode != 0) {
        status = BcmConfigureSleep(
            IoTargetSerial,
            RequestSync,
            config);
        if (!NT_SUCCESS(status)) {
            goto Exit;
        }

        status = DevicePowerOff(FdoExtension->WdfDevice);
        if (!NT_SUCCESS(status)) {
            goto Exit;
        }
    }

Exit:
    DoTrace(
        NT_SUCCESS(status) ? LEVEL_INFO : LEVEL_ERROR,
        TFLAG_HCI,
        ("BCM4345C0 initialization %!STATUS!", status));
    BtTraceStatus(status, "DeviceInitialize -> 0x%08X\n", status);
    return NT_SUCCESS(status) ? TRUE : FALSE;
}

_Use_decl_annotations_
NTSTATUS
DeviceDoPLDR(WDFDEVICE Fdo)
{
    PFDO_EXTENSION extension = FdoGetExtension(Fdo);
    NTSTATUS status = STATUS_SUCCESS;
    LONG wakeReferences;

    if ((extension->PowerLock == NULL) ||
        (extension->IoTargetSerial == NULL)) {
        return STATUS_DEVICE_NOT_READY;
    }

    if (InterlockedCompareExchange(
            &extension->ResetInProgress, 1, 0) != 0) {
        return STATUS_DEVICE_BUSY;
    }

    status = WdfWaitLockAcquire(extension->PowerLock, NULL);
    if (!NT_SUCCESS(status)) {
        InterlockedExchange(&extension->ResetInProgress, 0);
        return status;
    }

    extension->DeviceInitialized = FALSE;
    DeviceDisableWakeControl(Fdo);

    if (extension->DefaultQueue != NULL) {
        WdfIoQueueStopSynchronously(extension->DefaultQueue);
    }

    WdfIoTargetStop(
        extension->IoTargetSerial,
        WdfIoTargetCancelSentIo);
    ReadResourcesReset(Fdo);

    wakeReferences =
        InterlockedExchange(&extension->TxWakeReferences, 0);
    if (wakeReferences != 0) {
        DoTrace(
            LEVEL_WARNING,
            TFLAG_POWER,
            ("PLDR cleared %d stale BT_DEV_WAKE references",
             wakeReferences));
    }

    status = DeviceEnable(Fdo, FALSE);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    Ap6256BtDelayMilliseconds(20);
    status = DeviceEnable(Fdo, TRUE);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    status = WdfIoTargetStart(extension->IoTargetSerial);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    extension->DeviceInitialized = DeviceInitialize(
        extension,
        extension->IoTargetSerial,
        extension->RequestIoctlSync,
        TRUE);
    if (!IsDeviceInitialized(extension)) {
        status = STATUS_DEVICE_HARDWARE_ERROR;
        goto Exit;
    }

    if (extension->ReadRequest != NULL) {
        extension->ReadContext.RequestState = REQUEST_COMPLETE;
        extension->ReadPumpRunning = TRUE;
        status = ReadH4Packet(
            &extension->ReadContext,
            extension->ReadRequest,
            extension->ReadMemory,
            extension->ReadBuffer,
            sizeof(extension->ReadBuffer),
            INITIAL_H4_READ_SIZE);
        if (status == STATUS_PENDING) {
            status = STATUS_SUCCESS;
        }
    }

Exit:
    if (!NT_SUCCESS(status)) {
        extension->DeviceInitialized = FALSE;
        (VOID)DeviceEnable(Fdo, FALSE);
    }

    if (extension->DefaultQueue != NULL) {
        WdfIoQueueStart(extension->DefaultQueue);
    }
    WdfWaitLockRelease(extension->PowerLock);
    InterlockedExchange(&extension->ResetInProgress, 0);
    DoTrace(
        NT_SUCCESS(status) ? LEVEL_INFO : LEVEL_ERROR,
        TFLAG_POWER,
        ("BCM4345C0 platform reset %!STATUS!", status));
    return status;
}
