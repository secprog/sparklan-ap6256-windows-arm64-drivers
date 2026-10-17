/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Licensed under the GNU General Public License, version 3 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#include <ntddk.h>
#include <wdf.h>

#include <initguid.h>
#include <ntddsd.h>
#include "version.h"
#include "buildstamp.h"
#include "logging.h"

#define AP6256FN_F2_BLOCKSIZE 512

typedef struct _AP6256FN_CONTEXT {
    WDFDEVICE                Device;
    SDBUS_INTERFACE_STANDARD SdInterface;

    USHORT                   BusDriverVersion;
    UCHAR                    FunctionNumber;
    UCHAR                    FunctionNumberCopy;
    UCHAR                    VersionAtLeast2;
    ULONG                    LogLevel;
} AP6256FN_CONTEXT, *PAP6256FN_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AP6256FN_CONTEXT, Ap6256FnGetContext)

DRIVER_INITIALIZE              DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD      Ap6256FnEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE Ap6256FnEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE Ap6256FnEvtReleaseHardware;

EVT_WDF_DEVICE_D0_EXIT          Ap6256FnEvtD0Exit;
EVT_WDF_DEVICE_QUERY_REMOVE     Ap6256FnEvtQueryRemove;
EVT_WDF_DEVICE_QUERY_STOP       Ap6256FnEvtQueryStop;
EVT_WDF_DEVICE_SURPRISE_REMOVAL Ap6256FnEvtSurpriseRemoval;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, Ap6256FnEvtDeviceAdd)
#pragma alloc_text(PAGE, Ap6256FnEvtPrepareHardware)
#pragma alloc_text(PAGE, Ap6256FnEvtReleaseHardware)
#pragma alloc_text(PAGE, Ap6256FnEvtD0Exit)
#pragma alloc_text(PAGE, Ap6256FnEvtQueryRemove)
#pragma alloc_text(PAGE, Ap6256FnEvtQueryStop)
#pragma alloc_text(PAGE, Ap6256FnEvtSurpriseRemoval)
#endif

_IRQL_requires_max_(APC_LEVEL)
static NTSTATUS
Ap6256FnSdProperty(
    _In_ PAP6256FN_CONTEXT Ctx,
    _In_ SD_REQUEST_FUNCTION RequestFunction,
    _In_ SDBUS_PROPERTY Property,
    _Inout_updates_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length
    )
{
    SDBUS_REQUEST_PACKET packet;

    RtlZeroMemory(&packet, sizeof(packet));
    packet.RequestFunction = RequestFunction;
    packet.Parameters.GetSetProperty.Property = Property;
    packet.Parameters.GetSetProperty.Buffer = Buffer;
    packet.Parameters.GetSetProperty.Length = Length;

    return SdBusSubmitRequest(Ctx->SdInterface.Context, &packet);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
static VOID
Ap6256FnReleaseInterface(
    _In_ PAP6256FN_CONTEXT Ctx
    )
{
    if (Ctx->SdInterface.InterfaceDereference != NULL) {
        Ctx->SdInterface.InterfaceDereference(Ctx->SdInterface.Context);
    }
    RtlZeroMemory(&Ctx->SdInterface, sizeof(Ctx->SdInterface));
}

_IRQL_requires_max_(PASSIVE_LEVEL)
static NTSTATUS
Ap6256FnAcquireInterface(
    _In_ WDFDEVICE Device,
    _In_ PAP6256FN_CONTEXT Ctx
    )
{
    SDBUS_INTERFACE_PARAMETERS ifParams;
    NTSTATUS status;

    if (Ctx->SdInterface.InterfaceDereference != NULL) {
        FnTrace(Ctx->LogLevel, FN_LOG_WARNING, "interface already open; refusing to open it twice\n");
        return STATUS_DEVICE_BUSY;
    }

    RtlZeroMemory(&Ctx->SdInterface, sizeof(Ctx->SdInterface));
    status = SdBusOpenInterface(WdfDeviceWdmGetPhysicalDevice(Device),
                                &Ctx->SdInterface,
                                sizeof(SDBUS_INTERFACE_STANDARD),
                                SDBUS_INTERFACE_VERSION);
    if (!NT_SUCCESS(status)) {
        FnTrace(Ctx->LogLevel, FN_LOG_ERROR, "SdBusOpenInterface failed 0x%08X\n", status);
        RtlZeroMemory(&Ctx->SdInterface, sizeof(Ctx->SdInterface));
        return status;
    }

    RtlZeroMemory(&ifParams, sizeof(ifParams));
    ifParams.Size = sizeof(ifParams);
    ifParams.SdioFlags = SDIO_FLAG_DO_NOT_MANAGE_IO_ENABLE;
    ifParams.TargetObject = WdfDeviceWdmGetAttachedDevice(Device);
    ifParams.DeviceGeneratesInterrupts = FALSE;
    ifParams.CallbackAtDpcLevel = FALSE;
    ifParams.CallbackRoutine = NULL;
    ifParams.CallbackRoutineContext = Ctx;

    status = STATUS_UNSUCCESSFUL;
    if (Ctx->SdInterface.InitializeInterface != NULL) {
        status = Ctx->SdInterface.InitializeInterface(
            Ctx->SdInterface.Context, &ifParams);
    }
    FnTrace(Ctx->LogLevel, NT_SUCCESS(status) ? FN_LOG_INFORMATION : FN_LOG_ERROR,
            "InitializeInterface -> 0x%08X\n", status);
    if (!NT_SUCCESS(status)) {
        Ap6256FnReleaseInterface(Ctx);
    }
    return status;
}

_Use_decl_annotations_
NTSTATUS
Ap6256FnEvtPrepareHardware(
    WDFDEVICE Device,
    WDFCMRESLIST ResourcesRaw,
    WDFCMRESLIST ResourcesTranslated
    )
{
    PAP6256FN_CONTEXT ctx = Ap6256FnGetContext(Device);
    USHORT hostBlockLength = 0;
    USHORT functionBlockLength;
    UCHAR  interruptEnable = 0;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    PAGED_CODE();

    ctx->LogLevel = FnLogReadSettings(WdfDeviceWdmGetPhysicalDevice(Device));
    FnTrace(ctx->LogLevel, FN_LOG_INFORMATION, "PrepareHardware entry\n");

    if (ctx->SdInterface.InterfaceDereference == NULL) {
        FnTrace(ctx->LogLevel, FN_LOG_INFORMATION, "interface not held at PrepareHardware; acquiring now\n");
        status = Ap6256FnAcquireInterface(Device, ctx);
        if (!NT_SUCCESS(status)) {
            FnTrace(ctx->LogLevel, FN_LOG_WARNING, "interface acquire failed 0x%08X - starting anyway\n",
                    status);
            return STATUS_SUCCESS;
        }
    }

    status = Ap6256FnSdProperty(ctx, SDRF_GET_PROPERTY, SDP_FUNCTION_NUMBER,
                                &ctx->FunctionNumber, sizeof(ctx->FunctionNumber));
    if (!NT_SUCCESS(status)) {
        FnTrace(ctx->LogLevel, FN_LOG_ERROR, "get function number failed 0x%08X\n", status);
        goto fail;
    }
    FnTrace(ctx->LogLevel, FN_LOG_INFORMATION, "function number %u\n", (ULONG)ctx->FunctionNumber);

    ctx->BusDriverVersion = 0x100;
    (void)Ap6256FnSdProperty(ctx, SDRF_GET_PROPERTY, SDP_BUS_DRIVER_VERSION,
                             &ctx->BusDriverVersion, sizeof(ctx->BusDriverVersion));
    ctx->FunctionNumberCopy = ctx->FunctionNumber;
    ctx->VersionAtLeast2 = (UCHAR)((ctx->BusDriverVersion >= 0x200) ? 1 : 0);

    status = Ap6256FnSdProperty(ctx, SDRF_GET_PROPERTY, SDP_HOST_BLOCK_LENGTH,
                                &hostBlockLength, sizeof(hostBlockLength));
    if (!NT_SUCCESS(status)) {
        FnTrace(ctx->LogLevel, FN_LOG_ERROR, "fn%u get host block length failed 0x%08X\n",
                (ULONG)ctx->FunctionNumber, status);
        goto fail;
    }
    if (hostBlockLength == 0) {
        FnTrace(ctx->LogLevel, FN_LOG_ERROR, "fn%u host block length is zero\n", (ULONG)ctx->FunctionNumber);
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto fail;
    }
    FnTrace(ctx->LogLevel, FN_LOG_VERBOSE, "fn%u host block length %u, bus version 0x%X\n",
            (ULONG)ctx->FunctionNumber, (ULONG)hostBlockLength,
            (ULONG)ctx->BusDriverVersion);

    status = Ap6256FnSdProperty(ctx, SDRF_GET_PROPERTY, SDP_FUNCTION_INT_ENABLE,
                                &interruptEnable, sizeof(interruptEnable));
    if (!NT_SUCCESS(status)) {
        FnTrace(ctx->LogLevel, FN_LOG_ERROR, "fn%u get interrupt enable failed 0x%08X\n",
                (ULONG)ctx->FunctionNumber, status);
        goto fail;
    }

    functionBlockLength = (USHORT)((hostBlockLength < AP6256FN_F2_BLOCKSIZE)
                                   ? hostBlockLength : AP6256FN_F2_BLOCKSIZE);
    status = Ap6256FnSdProperty(ctx, SDRF_SET_PROPERTY, SDP_FUNCTION_BLOCK_LENGTH,
                                &functionBlockLength, sizeof(functionBlockLength));
    if (NT_SUCCESS(status)) {
        FnTrace(ctx->LogLevel, FN_LOG_INFORMATION, "fn%u ready: function block length %u\n",
                (ULONG)ctx->FunctionNumber, (ULONG)functionBlockLength);
        return STATUS_SUCCESS;
    }
    FnTrace(ctx->LogLevel, FN_LOG_ERROR, "fn%u set function block length %u failed 0x%08X\n",
            (ULONG)ctx->FunctionNumber, (ULONG)functionBlockLength, status);

fail:

    FnTrace(ctx->LogLevel, FN_LOG_WARNING, "fn%u PrepareHardware failed 0x%08X - starting anyway, the main "
            "driver programs the card block size itself\n",
            (ULONG)ctx->FunctionNumber, status);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
Ap6256FnEvtReleaseHardware(
    WDFDEVICE Device,
    WDFCMRESLIST ResourcesTranslated
    )
{
    PAP6256FN_CONTEXT ctx = Ap6256FnGetContext(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);
    PAGED_CODE();

    FnTrace(ctx->LogLevel, FN_LOG_VERBOSE, "fn%u ReleaseHardware (interface kept)\n",
            (ULONG)ctx->FunctionNumber);
    return STATUS_SUCCESS;
}

static PCSTR
Ap6256FnPowerStateName(
    WDF_POWER_DEVICE_STATE State
    )
{
    switch (State) {
    case WdfPowerDeviceD0:      return "D0";
    case WdfPowerDeviceD1:      return "D1";
    case WdfPowerDeviceD2:      return "D2";
    case WdfPowerDeviceD3:      return "D3 (powering down, staying enumerated)";
    case WdfPowerDeviceD3Final: return "D3Final (being removed)";
    case WdfPowerDevicePrepareForHibernation: return "PrepareForHibernation";
    default:                    return "unknown";
    }
}

_Use_decl_annotations_
NTSTATUS
Ap6256FnEvtD0Exit(
    WDFDEVICE Device,
    WDF_POWER_DEVICE_STATE TargetState
    )
{
    PAP6256FN_CONTEXT ctx = Ap6256FnGetContext(Device);

    PAGED_CODE();

    FnTrace(ctx->LogLevel, FN_LOG_VERBOSE, "fn%u D0Exit -> %s\n",
            (ULONG)ctx->FunctionNumber, Ap6256FnPowerStateName(TargetState));
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
Ap6256FnEvtQueryRemove(
    WDFDEVICE Device
    )
{
    PAP6256FN_CONTEXT ctx = Ap6256FnGetContext(Device);

    PAGED_CODE();

    FnTrace(ctx->LogLevel, FN_LOG_INFORMATION, "fn%u QueryRemove (orderly removal requested)\n",
            (ULONG)ctx->FunctionNumber);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
Ap6256FnEvtQueryStop(
    WDFDEVICE Device
    )
{
    PAP6256FN_CONTEXT ctx = Ap6256FnGetContext(Device);

    PAGED_CODE();

    FnTrace(ctx->LogLevel, FN_LOG_INFORMATION, "fn%u QueryStop (resource rebalance requested)\n",
            (ULONG)ctx->FunctionNumber);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
Ap6256FnEvtSurpriseRemoval(
    WDFDEVICE Device
    )
{
    PAP6256FN_CONTEXT ctx = Ap6256FnGetContext(Device);

    PAGED_CODE();

    FnTrace(ctx->LogLevel, FN_LOG_WARNING, "fn%u SURPRISE REMOVAL (the bus dropped the devnode)\n",
            (ULONG)ctx->FunctionNumber);
}

static EVT_WDF_OBJECT_CONTEXT_CLEANUP Ap6256FnEvtDeviceCleanup;

static VOID
Ap6256FnEvtDeviceCleanup(
    WDFOBJECT Object
    )
{
    PAP6256FN_CONTEXT ctx = Ap6256FnGetContext((WDFDEVICE)Object);

    FnTrace(ctx->LogLevel, FN_LOG_INFORMATION, "fn%u device cleanup; releasing the SD bus interface\n",
            (ULONG)ctx->FunctionNumber);
    Ap6256FnReleaseInterface(ctx);
}

_Use_decl_annotations_
NTSTATUS
Ap6256FnEvtDeviceAdd(
    WDFDRIVER Driver,
    PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPower;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_DEVICE_PNP_CAPABILITIES pnpCaps;
    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS idleSettings;
    WDFDEVICE device;
    PAP6256FN_CONTEXT ctx;
    NTSTATUS status;
    ULONG logLevel;

    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    logLevel = FnLogReadSettings(WdfFdoInitWdmGetPhysicalDevice(DeviceInit));

    WdfDeviceInitSetPowerPageable(DeviceInit);
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPower);
    pnpPower.EvtDevicePrepareHardware = Ap6256FnEvtPrepareHardware;
    pnpPower.EvtDeviceReleaseHardware = Ap6256FnEvtReleaseHardware;

    pnpPower.EvtDeviceD0Exit          = Ap6256FnEvtD0Exit;
    pnpPower.EvtDeviceQueryRemove     = Ap6256FnEvtQueryRemove;
    pnpPower.EvtDeviceQueryStop       = Ap6256FnEvtQueryStop;
    pnpPower.EvtDeviceSurpriseRemoval = Ap6256FnEvtSurpriseRemoval;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPower);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, AP6256FN_CONTEXT);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    attributes.SynchronizationScope = WdfSynchronizationScopeDevice;
    attributes.EvtCleanupCallback = Ap6256FnEvtDeviceCleanup;

    FnTrace(logLevel, FN_LOG_INFORMATION, "companion v%s built %s DeviceAdd entry\n",
            AP6256FN_VER_STR, AP6256_BUILD_STAMP);

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        FnTrace(logLevel, FN_LOG_ERROR, "WdfDeviceCreate failed 0x%08X\n", status);
        return status;
    }

    WDF_DEVICE_PNP_CAPABILITIES_INIT(&pnpCaps);
    pnpCaps.Removable = WdfFalse;
    pnpCaps.NoDisplayInUI = WdfFalse;
    WdfDeviceSetPnpCapabilities(device, &pnpCaps);

    ctx = Ap6256FnGetContext(device);
    ctx->Device = device;
    ctx->LogLevel = logLevel;

    status = Ap6256FnAcquireInterface(device, ctx);
    if (!NT_SUCCESS(status)) {
        FnTrace(ctx->LogLevel, FN_LOG_WARNING, "DeviceAdd: interface acquire failed 0x%08X - the function "
                "will almost certainly fail to start\n", status);
    }

    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&idleSettings, IdleCannotWakeFromS0);
    idleSettings.IdleTimeout = 10000;
    status = WdfDeviceAssignS0IdleSettings(device, &idleSettings);
    if (!NT_SUCCESS(status)) {

        FnTrace(ctx->LogLevel, FN_LOG_WARNING, "WdfDeviceAssignS0IdleSettings failed 0x%08X - continuing "
                "without idle power management\n", status);
    }

    FnTrace(ctx->LogLevel, FN_LOG_INFORMATION, "DeviceAdd done\n");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
DriverEntry(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    WDF_DRIVER_CONFIG_INIT(&config, Ap6256FnEvtDeviceAdd);
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES,
                             &config, WDF_NO_HANDLE);
    return status;
}
