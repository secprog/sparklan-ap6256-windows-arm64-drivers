/*
 * SPDX-License-Identifier: MS-PL
 * Copyright (c) Microsoft Corporation All Rights Reserved
 * See bt/LICENSE-MS-PL.txt for the license text.
 */

#include "driver.h"
#include <reshub.h>
#include "fdo.tmh"

#define BTHX_VALID_WRITE_PACKET_TYPE(type) (type == HciPacketCommand || type == HciPacketAclData)
#define BTHX_VALID_READ_PACKET_TYPE(type)  (type == HciPacketEvent   || type == HciPacketAclData)

#define BT_IOCTL_TRACE_LIMIT 48
static LONG BtIoctlTraceCount = 0;

static BOOLEAN
BtShouldTraceIoctl(VOID)
{
    if (!BtLogEnabled(BT_LOG_VERBOSE)) {
        return FALSE;
    }
    return (BOOLEAN)(InterlockedIncrement(&BtIoctlTraceCount) <=
                     BT_IOCTL_TRACE_LIMIT);
}

static PCSTR
BtIoctlName(_In_ ULONG Code)
{
    switch (Code) {
    case IOCTL_BTHX_WRITE_HCI:          return "WRITE_HCI";
    case IOCTL_BTHX_READ_HCI:           return "READ_HCI";
    case IOCTL_BTHX_GET_VERSION:        return "GET_VERSION";
    case IOCTL_BTHX_SET_VERSION:        return "SET_VERSION";
    case IOCTL_BTHX_QUERY_CAPABILITIES: return "QUERY_CAPABILITIES";
    case IOCTL_BUSENUM_SET_RADIO_ONOFF_VENDOR_SPECFIC:
                                        return "SET_RADIO_ONOFF";
    default:                            return "unrecognised";
    }
}

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, FdoCreateOneChildDevice)
#pragma alloc_text (PAGE, FdoRemoveOneChildDevice)
#pragma alloc_text (PAGE, FdoCreateAllChildren)
#pragma alloc_text (PAGE, FdoFindConnectResources)
#pragma alloc_text (PAGE, FdoDevPrepareHardware)
#pragma alloc_text (PAGE, FdoDevReleaseHardware)
#pragma alloc_text (PAGE, FdoDevSelfManagedIoInit)
#pragma alloc_text (PAGE, FdoDevSelfManagedIoCleanup)
#pragma alloc_text (PAGE, FdoDevD0Exit)
#pragma alloc_text (PAGE, HlpInitializeFdoExtension)
#pragma alloc_text (PAGE, FdoWriteToDeviceSync)
#endif

#ifdef DYNAMIC_ENUM

typedef struct _ENABLE_PDO_CONTEXT {
    WDFDEVICE     Fdo;
} ENABLE_PDO_CONTEXT, *PENABLE_PDO_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(ENABLE_PDO_CONTEXT, GetEnablePdoWorkItemContext)

ULONG g_WaitToEnablePDO = 20000;

VOID
DeviceEnablePDOWorker(
    _In_  WDFWORKITEM  _WorkItem
    )

{
    PENABLE_PDO_CONTEXT Context;
    LARGE_INTEGER    RemoteWakeTimeout;

    NTSTATUS Status = STATUS_SUCCESS;

    PAGED_CODE();

    DoTrace(LEVEL_INFO, TFLAG_PNP, ("+DeviceEnablePDOWorker"));
    Context = GetEnablePdoWorkItemContext(_WorkItem);

    RemoteWakeTimeout.QuadPart = WDF_REL_TIMEOUT_IN_MS(g_WaitToEnablePDO);
    KeDelayExecutionThread(KernelMode, FALSE, &RemoteWakeTimeout);

    DoTrace(LEVEL_INFO, TFLAG_PNP, ("+Complete the wait"));

    Status = FdoCreateOneChildDeviceDynamic(Context->Fdo,
                                            BT_PDO_HARDWARE_IDS,
                                            sizeof(BT_PDO_HARDWARE_IDS)/sizeof(WCHAR),
                                            BLUETOOTH_FUNC_IDS );

    WdfObjectDelete(_WorkItem);

    DoTrace(LEVEL_INFO, TFLAG_POWER, ("-DeviceEnablePDOWorker %!STATUS!", Status));

}

NTSTATUS
FdoEvtDeviceListCreatePdo(
    WDFCHILDLIST DeviceList,
    PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER IdentificationDescription,
    PWDFDEVICE_INIT ChildInit
    )

{
    PPDO_IDENTIFICATION_DESCRIPTION pDesc;

    PAGED_CODE();

    pDesc = CONTAINING_RECORD(IdentificationDescription,
                              PDO_IDENTIFICATION_DESCRIPTION,
                              Header);

    return PdoCreateDynamic(WdfChildListGetDevice(DeviceList),
                            ChildInit,
                            pDesc->HardwareIds,
                            pDesc->SerialNo);
}

NTSTATUS
FdoCreateOneChildDeviceDynamic(
    _In_ WDFDEVICE  _Device,
    _In_ PWCHAR     _HardwareIds,
    _In_ size_t     _CchHardwareIds,
    _In_ ULONG      _SerialNo
    )

{
    PDO_IDENTIFICATION_DESCRIPTION Description;
    NTSTATUS         Status;

    PAGED_CODE ();

    WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER_INIT(&Description.Header,
                                                     sizeof(Description));

    Description.SerialNo = _SerialNo;
    Description.CchHardwareIds = _CchHardwareIds;
    Description.HardwareIds = _HardwareIds;

    Status = WdfChildListAddOrUpdateChildDescriptionAsPresent(WdfFdoGetDefaultChildList(_Device),
                                                              &Description.Header,
                                                               NULL);

    if (Status == STATUS_OBJECT_NAME_EXISTS) {

        Status = STATUS_INVALID_PARAMETER;
    }

    return Status;
}

#endif

NTSTATUS
FdoCreateOneChildDevice(
    _In_ WDFDEVICE  _Device,
    _In_ PWSTR      _HardwareIds,
    _In_ ULONG      _SerialNo
    )

{
    NTSTATUS         Status = STATUS_SUCCESS;
    BOOLEAN          IsUnique = TRUE;
    WDFDEVICE        ChildDevice;
    PPDO_EXTENSION   PdoExtension;
    PFDO_EXTENSION   FdoExtension;

    PAGED_CODE();

    DoTrace(LEVEL_INFO, TFLAG_PNP, ("+ FdoCreateOneChildDevice() HWID: %S", _HardwareIds));

    FdoExtension = FdoGetExtension(_Device);
    ChildDevice = NULL;

    WdfWaitLockAcquire(FdoExtension->ChildLock, NULL);
    WdfFdoLockStaticChildListForIteration(_Device);

    while ((ChildDevice = WdfFdoRetrieveNextStaticChild(_Device,
                                                        ChildDevice,
                                                        WdfRetrieveAddedChildren)) != NULL) {

        PdoExtension = PdoGetExtension(ChildDevice);

        if (_SerialNo == PdoExtension->SerialNo) {
            IsUnique = FALSE;
            Status = STATUS_INVALID_PARAMETER;
            break;
        }
    }

    if (IsUnique) {

        Status = PdoCreate(_Device, _HardwareIds, _SerialNo);
    }

    WdfFdoUnlockStaticChildListFromIteration(_Device);
    WdfWaitLockRelease(FdoExtension->ChildLock);

    DoTrace(LEVEL_INFO, TFLAG_PNP, ("- FdoCreateOneChildDevice() %!STATUS!", Status));

    return Status;
}

NTSTATUS
FdoRemoveOneChildDevice(
    WDFDEVICE   _Device,
    ULONG       _SerialNo
    )

{
    PPDO_EXTENSION  PdoExtension;
    BOOLEAN         Found = FALSE;
    BOOLEAN         PlugOutAll;
    WDFDEVICE       ChildDevice;
    NTSTATUS        Status = STATUS_INVALID_PARAMETER;

    PAGED_CODE();

    PlugOutAll = (0 == _SerialNo) ? TRUE : FALSE;

    ChildDevice = NULL;

    WdfFdoLockStaticChildListForIteration(_Device);

    while ((ChildDevice = WdfFdoRetrieveNextStaticChild(_Device,
                                                        ChildDevice,
                                                        WdfRetrieveAddedChildren)) != NULL) {
        if (PlugOutAll) {

            Status = WdfPdoMarkMissing(ChildDevice);
            if(!NT_SUCCESS(Status)) {
                DoTrace(LEVEL_INFO, TFLAG_PNP, ("WdfPdoMarkMissing failed 0x%x\n", Status));
                break;
            }

            Found = TRUE;
        }
        else {
            PdoExtension = PdoGetExtension(ChildDevice);

            if (_SerialNo == PdoExtension->SerialNo) {

                Status = WdfPdoMarkMissing(ChildDevice);
                if(!NT_SUCCESS(Status)) {
                    DoTrace(LEVEL_INFO, TFLAG_PNP, ("WdfPdoMarkMissing failed 0x%x\n", Status));
                    break;
                }

                Found = TRUE;
                break;
            }
        }
    }

    WdfFdoUnlockStaticChildListFromIteration(_Device);

    if (Found) {
        Status = STATUS_SUCCESS;
    }

    return Status;
}

NTSTATUS
FdoCreateAllChildren(
    _In_  WDFDEVICE _Device
    )

{
    NTSTATUS       Status;
    PFDO_EXTENSION FdoExtension;

    PAGED_CODE();

    DoTrace(LEVEL_INFO, TFLAG_PNP, (" + FdoCreateAllChildren"));

    Status = FdoCreateOneChildDevice(_Device,
                                     BT_PDO_HARDWARE_IDS,
                                     BLUETOOTH_FUNC_IDS);

    FdoExtension = FdoGetExtension(_Device);
    if (NT_SUCCESS(Status)) {
        FdoExtension->IsRadioEnabled = TRUE;
    }

    return Status;
}

NTSTATUS
HlpInitializeFdoExtension(
    WDFDEVICE _Device
    )

{
    PFDO_EXTENSION        FdoExtension;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_WORKITEM_CONFIG   WorkitemConfig;
    NTSTATUS              Status;

    PAGED_CODE();

    DoTrace(LEVEL_INFO, TFLAG_PNP,("+HlpInitializeFdoExtension"));

    FdoExtension = FdoGetExtension(_Device);
    FdoExtension->WdfDevice = _Device;
    FdoExtension->SuspendedForIdle = FALSE;
    InterlockedExchange(&FdoExtension->WakeArmed, 0);
    InterlockedExchange(&FdoExtension->TxWakeReferences, 0);
    InterlockedExchange(&FdoExtension->ResetInProgress, 0);
    InterlockedExchange(
        &FdoExtension->PowerTransitionInProgress,
        0);

    FdoExtension->BthXCaps.MaxAclTransferInSize = MAX_HCI_ACLDATA_SIZE;
    FdoExtension->BthXCaps.ScoSupport = ScoSupportHCIBypass;
    FdoExtension->BthXCaps.MaxScoChannels = 1;
    FdoExtension->BthXCaps.IsDeviceIdleCapable = TRUE;

    FdoExtension->BthXCaps.IsDeviceWakeCapable =
        FdoExtension->HostWakeOperational;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = _Device;

    Status = WdfRequestCreate(&Attributes, FdoExtension->IoTargetSerial, &FdoExtension->RequestIoctlSync);
    if (!NT_SUCCESS(Status))
    {
        DoTrace(LEVEL_ERROR, TFLAG_PNP, (" WdfRequestCreate failed %!STATUS!", Status));
        goto Exit;
    }

    FdoExtension->HardwareErrorDetected = FALSE;

    Status = WdfRequestCreate(&Attributes, FdoExtension->IoTargetSerial, &FdoExtension->RequestWaitOnError);
    if (!NT_SUCCESS(Status))
    {
        DoTrace(LEVEL_ERROR, TFLAG_PNP, (" WdfRequestCreate failed %!STATUS!", Status));
        goto Exit;
    }

    Status = WdfMemoryCreatePreallocated(&Attributes,
                                         &FdoExtension->SerErrorMask,
                                         sizeof(FdoExtension->SerErrorMask),
                                         &FdoExtension->WaitMaskMemory);

    if (!NT_SUCCESS(Status))
    {
        DoTrace(LEVEL_ERROR, TFLAG_PNP, (" WdfMemoryCreatePreallocated failed %!STATUS!", Status));
        goto Exit;
    }

    if (FdoExtension->QueueAccessLock == NULL)
    {
        Status = WdfSpinLockCreate(&Attributes, &FdoExtension->QueueAccessLock);
        if (!NT_SUCCESS(Status))
        {
            DoTrace(LEVEL_ERROR, TFLAG_PNP, (" WdfSpinLockCreate failed %!STATUS!", Status));
            goto Exit;
        }
    }

    Status = WdfWaitLockCreate(
        &Attributes,
        &FdoExtension->PowerLock);
    if (!NT_SUCCESS(Status))
    {
        DoTrace(LEVEL_ERROR, TFLAG_PNP, (" WdfWaitLockCreate failed %!STATUS!", Status));
        goto Exit;
    }

    WDF_WORKITEM_CONFIG_INIT(
        &WorkitemConfig,
        DeviceDevWakeReleaseWorker);
    Status = WdfWorkItemCreate(
        &WorkitemConfig,
        &Attributes,
        &FdoExtension->DevWakeReleaseWorkItem);

Exit:

    return Status;

}

VOID
FdoEvtDeviceDisarmWake(
    _In_  WDFDEVICE  _Device
    )

{
    DoTrace(LEVEL_INFO, TFLAG_PNP,(" FdoEvtDeviceDisarmWake"));
    DeviceDisableWakeControl(_Device);
}

NTSTATUS
FdoEvtDeviceArmWake(
    _In_  WDFDEVICE  _Device
    )

{
    NTSTATUS Status;

    DoTrace(LEVEL_INFO, TFLAG_PNP,(" FdoEvtDeviceArmWake"));

    Status = DeviceEnableWakeControl(
        _Device,
        PowerSystemWorking);
    return Status;
}

BOOLEAN
FdoEvtHostWakeIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG MessageID
    )
{
    WDFDEVICE device = WdfInterruptGetDevice(Interrupt);
    PFDO_EXTENSION extension = FdoGetExtension(device);

    UNREFERENCED_PARAMETER(MessageID);
    InterlockedIncrement(&extension->HostWakeCount);
    return TRUE;
}

NTSTATUS
FdoFindConnectResources(
    _In_ WDFDEVICE    _Device,
    _In_ WDFCMRESLIST _ResourcesRaw,
    _In_ WDFCMRESLIST _ResourcesTranslated
    )

{
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR RawDescriptor;
    PFDO_EXTENSION FdoExtension;
    WDF_INTERRUPT_CONFIG InterruptConfig;
    ULONG Index;
    ULONG ResourceCount = 0;
    NTSTATUS Status;
    ULONG RawResourceCount;
    BOOLEAN UartConnectionIdIsFound = FALSE;

    BOOLEAN HostWakeFound = FALSE;
    BOOLEAN SleepRequested = DeviceSleepModeRequested();
    UNREFERENCED_PARAMETER(_ResourcesRaw);

    PAGED_CODE();

    DoTrace(LEVEL_INFO, TFLAG_PNP,("+FdoFindConnectResources"));

    FdoExtension = FdoGetExtension(_Device);
    FdoExtension->GPIOConnectionCount = 0;
    RtlZeroMemory(FdoExtension->GPIOConnectionIds, sizeof(FdoExtension->GPIOConnectionIds));

    FdoExtension->HostWakeOperational = FALSE;
    FdoExtension->HostWakeInterrupt = NULL;
    Status = STATUS_SUCCESS;

    ResourceCount = WdfCmResourceListGetCount(_ResourcesTranslated);

    RawResourceCount = WdfCmResourceListGetCount(_ResourcesRaw);
    for (Index = 0; Index < ResourceCount; Index++)
    {
        Descriptor = WdfCmResourceListGetDescriptor(_ResourcesTranslated, Index);

        if (Descriptor == NULL) {
            Status = STATUS_DEVICE_CONFIGURATION_ERROR;
            break;
        }
        switch(Descriptor->Type)
        {
        case CmResourceTypeConnection:

            if ((Descriptor->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL) &&
                (Descriptor->u.Connection.Type  == CM_RESOURCE_CONNECTION_TYPE_SERIAL_UART))
            {
                NT_ASSERT(UartConnectionIdIsFound == FALSE && L"More than one set of UART connection");

                UartConnectionIdIsFound = TRUE;

                FdoExtension->UARTConnectionId.LowPart  = Descriptor->u.Connection.IdLowPart;
                FdoExtension->UARTConnectionId.HighPart = Descriptor->u.Connection.IdHighPart;

                DoTrace(LEVEL_INFO, TFLAG_PNP,(" UART ConnectionID (0x%x, 0x%x)",
                    FdoExtension->UARTConnectionId.HighPart, FdoExtension->UARTConnectionId.LowPart));
            }
            else if ((Descriptor->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL) &&
                (Descriptor->u.Connection.Type  == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C))
            {

                FdoExtension->I2CConnectionId.LowPart  = Descriptor->u.Connection.IdLowPart;
                FdoExtension->I2CConnectionId.HighPart = Descriptor->u.Connection.IdHighPart;

                DoTrace(LEVEL_INFO, TFLAG_PNP,(" I2C ConnectionID (0x%x, 0x%x)",
                    FdoExtension->I2CConnectionId.HighPart, FdoExtension->I2CConnectionId.LowPart));
            }
            else if ((Descriptor->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_GPIO) &&
                (Descriptor->u.Connection.Type  == CM_RESOURCE_CONNECTION_TYPE_GPIO_IO))
            {

                if (FdoExtension->GPIOConnectionCount >= AP6256BT_MAX_GPIO_RESOURCES)
                {
                    Status = STATUS_DEVICE_CONFIGURATION_ERROR;
                    DoTrace(LEVEL_ERROR, TFLAG_PNP,(" Too many GPIO IO resources"));
                    break;
                }

                FdoExtension->GPIOConnectionIds[FdoExtension->GPIOConnectionCount].LowPart =
                    Descriptor->u.Connection.IdLowPart;
                FdoExtension->GPIOConnectionIds[FdoExtension->GPIOConnectionCount].HighPart =
                    Descriptor->u.Connection.IdHighPart;

                DoTrace(LEVEL_INFO, TFLAG_PNP,(" GPIO[%d] ConnectionID (0x%x, 0x%x)",
                    FdoExtension->GPIOConnectionCount,
                    Descriptor->u.Connection.IdHighPart, Descriptor->u.Connection.IdLowPart));
                FdoExtension->GPIOConnectionCount++;
            }
            break;

        case CmResourceTypeInterrupt:

            if (!SleepRequested) {
                DoTrace(
                    LEVEL_INFO,
                    TFLAG_PNP,
                    (" HOST_WAKE present but controller sleep is disabled"));
                break;
            }

            if (HostWakeFound || (Index >= RawResourceCount)) {
                Status = STATUS_DEVICE_CONFIGURATION_ERROR;
                break;
            }

            RawDescriptor =
                WdfCmResourceListGetDescriptor(_ResourcesRaw, Index);
            if ((RawDescriptor == NULL) ||
                (RawDescriptor->Type != CmResourceTypeInterrupt) ||
                (((Descriptor->Flags | RawDescriptor->Flags) &
                  CM_RESOURCE_INTERRUPT_WAKE_HINT) == 0)) {
                Status = STATUS_DEVICE_CONFIGURATION_ERROR;
                DoTrace(
                    LEVEL_ERROR,
                    TFLAG_PNP,
                    (" HOST_WAKE is not marked wake-capable by ACPI"));
                break;
            }

            WDF_INTERRUPT_CONFIG_INIT(
                &InterruptConfig,
                FdoEvtHostWakeIsr,
                NULL);
            InterruptConfig.InterruptRaw = RawDescriptor;
            InterruptConfig.InterruptTranslated = Descriptor;
            InterruptConfig.PassiveHandling = TRUE;
            InterruptConfig.CanWakeDevice = TRUE;
            Status = WdfInterruptCreate(
                _Device,
                &InterruptConfig,
                WDF_NO_OBJECT_ATTRIBUTES,
                &FdoExtension->HostWakeInterrupt);
            if (!NT_SUCCESS(Status)) {
                DoTrace(
                    LEVEL_ERROR,
                    TFLAG_PNP,
                    (" WdfInterruptCreate(HOST_WAKE) failed %!STATUS!",
                     Status));
                break;
            }

            HostWakeFound = TRUE;
            FdoExtension->HostWakeOperational = TRUE;
            DoTrace(LEVEL_INFO, TFLAG_PNP, (" HOST_WAKE interrupt configured"));
            break;
        default:
            DoTrace(LEVEL_INFO, TFLAG_PNP,(" Resource type %d not used.", Descriptor->Type));
            break;
        }

    }

    if (!UartConnectionIdIsFound)
    {
        Status = STATUS_NOT_FOUND;
    }

    if (NT_SUCCESS(Status) && SleepRequested && !HostWakeFound) {
        Status = STATUS_DEVICE_CONFIGURATION_ERROR;
        DoTrace(LEVEL_ERROR, TFLAG_PNP, (" Wake-capable HOST_WAKE is required when ConfigureSleepMode=1"));
    }

    DoTrace(LEVEL_INFO, TFLAG_PNP,("-FdoFindConnectResources ResourceCount %d, %!STATUS!", ResourceCount, Status));

    return Status;
}

NTSTATUS
FdoOpenDevice(
    _In_  WDFDEVICE   _Device,
    _Out_ WDFIOTARGET *_pIoTarget
    )

{
    NTSTATUS Status = STATUS_SUCCESS;
    WDFIOTARGET IoTargetSerial;
    PFDO_EXTENSION FdoExtension = NULL;
    WCHAR TargetDeviceNameBuffer[100];
    PWSTR SymbolicLinkList = NULL;
    UNICODE_STRING TargetDeviceName;

    WDF_IO_TARGET_OPEN_PARAMS   OpenParams;

    DoTrace(LEVEL_INFO, TFLAG_PNP,("+FdoOpenDevice"));

    Status = WdfIoTargetCreate(_Device,
                               WDF_NO_OBJECT_ATTRIBUTES,
                               &IoTargetSerial);

    if (!NT_SUCCESS(Status))
    {
        BtTraceError("WdfIoTargetCreate failed 0x%08X\n", Status);
        goto Exit;
    }

    FdoExtension = FdoGetExtension(_Device);

    if (ValidConnectionID(FdoExtension->UARTConnectionId))
    {
        RtlInitEmptyUnicodeString(&TargetDeviceName,
                                  TargetDeviceNameBuffer,
                                  sizeof(TargetDeviceNameBuffer));

        Status = RESOURCE_HUB_CREATE_PATH_FROM_ID(&TargetDeviceName,
                                                  FdoExtension->UARTConnectionId.LowPart,
                                                  FdoExtension->UARTConnectionId.HighPart);
        if (!NT_SUCCESS(Status))
        {
            DoTrace(LEVEL_INFO, TFLAG_PNP,(" Failed to construct the open path %!STATUS!", Status));
            BtTraceError("RESOURCE_HUB_CREATE_PATH_FROM_ID failed 0x%08X\n", Status);
            goto Exit;
        }
    }
    else
    {

        Status = IoGetDeviceInterfaces(&GUID_DEVINTERFACE_COMPORT,
                                       NULL,
                                       0,
                                       &SymbolicLinkList
                                       );

        if (!NT_SUCCESS(Status))
        {
            DoTrace(LEVEL_INFO, TFLAG_PNP,("IoGetDeviceInterfaces(): %!STATUS!", Status));
            BtTraceError("no UART connection id in ACPI, and IoGetDeviceInterfaces "
                    "for GUID_DEVINTERFACE_COMPORT failed 0x%08X\n", Status);
            goto Exit;
        }

        if (*SymbolicLinkList == L'\0')
        {
            Status = STATUS_DEVICE_DOES_NOT_EXIST;
            BtTraceError("no UART connection id in ACPI and no COM port device "
                    "interface exists\n");
            goto Exit;
        }

        RtlInitUnicodeString(&TargetDeviceName, SymbolicLinkList);
    }

    DoTrace(LEVEL_INFO, TFLAG_PNP, (" Symbolic Name '%S'", TargetDeviceName.Buffer));
    BtTrace("opening UART target '%S'\n", TargetDeviceName.Buffer);

    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&OpenParams,
                                                &TargetDeviceName,
                                                STANDARD_RIGHTS_ALL);
    OpenParams.ShareAccess = 0;

    Status = WdfIoTargetOpen(IoTargetSerial,
                             &OpenParams);

    if (!NT_SUCCESS(Status))
    {
        DoTrace(LEVEL_INFO, TFLAG_PNP, ( " WdfIoTargetOpen failed %!STATUS!", Status));
        BtTraceWarning("WdfIoTargetOpen (exclusive) failed 0x%08X\n", Status);

        WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&OpenParams,
                                                    &TargetDeviceName,
                                                    STANDARD_RIGHTS_ALL);
        OpenParams.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;

        Status = WdfIoTargetOpen(IoTargetSerial, &OpenParams);
        BtTraceStatus(Status, "WdfIoTargetOpen (shared) -> 0x%08X%s\n", Status,
                NT_SUCCESS(Status) ?
                    " <- the port was held by someone else" : "");
    }

    if (!NT_SUCCESS(Status))
    {
        WdfObjectDelete(IoTargetSerial);
        goto Exit;
    }

    *_pIoTarget = IoTargetSerial;

Exit:

    if (SymbolicLinkList)
    {
        ExFreePool(SymbolicLinkList);
        SymbolicLinkList = NULL;
    }

    return Status;
}

NTSTATUS
FdoSetIdleSettings(
    _In_  WDFDEVICE _Device,
    _In_  IDLE_CAP_STATE   _IdleCapState
    )

{
    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS  IdleSettings;
    NTSTATUS  Status = STATUS_SUCCESS;
    BOOLEAN AssignS0IdleSettings = TRUE;

    DoTrace(LEVEL_INFO, TFLAG_PNP,("+FdoSetIdleSettings"));

    switch (_IdleCapState)
    {
    case IdleCapActiveOnly:

        WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&IdleSettings,
                                                   IdleCannotWakeFromS0);

        IdleSettings.DxState         = PowerDeviceD3;
        IdleSettings.IdleTimeout     = IdleTimeoutDefaultValue;
        IdleSettings.IdleTimeoutType = DriverManagedIdleTimeout;

        IdleSettings.UserControlOfIdleSettings = IdleDoNotAllowUserControl;
        IdleSettings.Enabled         = WdfFalse;

        IdleSettings.PowerUpIdleDeviceOnSystemWake = WdfFalse;
        break;

    case IdleCapCanWake:

        WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&IdleSettings,
                                                   IdleCanWakeFromS0);

        IdleSettings.DxState         = PowerDeviceD2;
        IdleSettings.IdleTimeout     = 0;
        IdleSettings.IdleTimeoutType = DriverManagedIdleTimeout;

        IdleSettings.UserControlOfIdleSettings = IdleAllowUserControl;
        IdleSettings.Enabled         = WdfTrue;

        break;

    case IdleCapCanTurnOff:

        WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&IdleSettings,
                                                   IdleCannotWakeFromS0);

        IdleSettings.DxState         = PowerDeviceD3;
        IdleSettings.IdleTimeout     = IdleTimeoutDefaultValue;
        IdleSettings.IdleTimeoutType = DriverManagedIdleTimeout;

        IdleSettings.UserControlOfIdleSettings = IdleAllowUserControl;
        IdleSettings.Enabled         = WdfTrue;

        IdleSettings.PowerUpIdleDeviceOnSystemWake = WdfFalse;
        break;

    default:
        AssignS0IdleSettings = FALSE;
        break;
    }

    if (AssignS0IdleSettings)
    {
        Status = WdfDeviceAssignS0IdleSettings(_Device,
                                               &IdleSettings);
    }

    DoTrace(LEVEL_INFO, TFLAG_PNP,("-FdoSetIdleSettings %!STATUS!", Status));
    return Status;
}

NTSTATUS
FdoDevPrepareHardware(
    _In_  WDFDEVICE     _Device,
    _In_  WDFCMRESLIST  _ResourcesRaw,
    _In_  WDFCMRESLIST  _ResourcesTranslated
    )

{
    NTSTATUS       Status;
    PFDO_EXTENSION FdoExtension;

    PAGED_CODE();

    BtLogLoadSettings(WdfDeviceWdmGetPhysicalDevice(_Device));
    DeviceQueryDeviceParameters(WdfGetDriver());
    DoTrace(LEVEL_INFO, TFLAG_PNP,("+FdoDevPrepareHardware"));
    BtTrace("PrepareHardware entry\n");

    Status = FdoFindConnectResources(_Device,
                                     _ResourcesRaw,
                                     _ResourcesTranslated);
    if (!NT_SUCCESS(Status))
    {
        DoTrace(LEVEL_ERROR, TFLAG_PNP, (" Required ACPI connection resources are missing %!STATUS!", Status));
        BtTraceError("FdoFindConnectResources failed 0x%08X - the ACPI device is "
                "not exposing the UART/GPIO connection resources\n", Status);
        goto Exit;
    }

    FdoExtension = FdoGetExtension(_Device);

    BtTrace("connect resources: UART id %08X`%08X, I2C id %08X`%08X, "
            "GPIO count %u\n",
            FdoExtension->UARTConnectionId.HighPart,
            FdoExtension->UARTConnectionId.LowPart,
            FdoExtension->I2CConnectionId.HighPart,
            FdoExtension->I2CConnectionId.LowPart,
            FdoExtension->GPIOConnectionCount);

    Status = FdoOpenDevice(_Device, &FdoExtension->IoTargetSerial);

    if (!NT_SUCCESS(Status) || FdoExtension->IoTargetSerial == NULL)
    {
        DoTrace(LEVEL_ERROR, TFLAG_PNP, (" FdoOpenDevice failed %!STATUS!", Status));
        BtTraceError("FdoOpenDevice failed 0x%08X (IoTargetSerial %p)\n",
                Status, FdoExtension->IoTargetSerial);

        goto Exit;
    }

    Status = HlpInitializeFdoExtension(_Device);

    if (!NT_SUCCESS(Status))
    {
        DoTrace(LEVEL_ERROR, TFLAG_PNP, (" HlpInitializeFdoExtension failed %!STATUS!", Status));
        BtTraceError("HlpInitializeFdoExtension failed 0x%08X\n", Status);
        goto Exit;
    }

    Status = FdoSetIdleSettings(_Device,
                                FdoExtension->HostWakeOperational ?
                                    IdleCapCanWake : IdleCapActiveOnly);
    if (!NT_SUCCESS(Status))
    {
        DoTrace(LEVEL_ERROR, TFLAG_PNP, (" FdoSetIdleSettings failed %!STATUS!", Status));
        BtTraceWarning("FdoSetIdleSettings failed 0x%08X (not fatal)\n", Status);

    }

    if (FdoExtension->GPIOConnectionCount != 0) {
        Status = DeviceEnable(_Device, TRUE);
        if (!NT_SUCCESS(Status)) {
            DoTrace(LEVEL_ERROR, TFLAG_PNP,("DeviceEnable failed %!STATUS!", Status));
            BtTraceError("DeviceEnable failed 0x%08X\n", Status);
            goto Exit;
        }
    }

    if (ValidConnectionID(FdoExtension->I2CConnectionId)) {
        Status = DevicePowerOn(_Device);
        if (!NT_SUCCESS(Status)) {
            DoTrace(LEVEL_ERROR, TFLAG_PNP,("DevicePowerOn failed %!STATUS!", Status));
            BtTraceError("DevicePowerOn failed 0x%08X\n", Status);
            goto Exit;
        }
    }

    FdoExtension->DeviceInitialized = DeviceInitialize(FdoExtension,
                                                       FdoExtension->IoTargetSerial,
                                                       FdoExtension->RequestIoctlSync,
                                                       TRUE);
    if (!IsDeviceInitialized(FdoExtension))
    {

        Status = STATUS_DEVICE_NOT_READY;
        DoTrace(LEVEL_ERROR, TFLAG_PNP, (" DeviceInitialize failed %!STATUS!", Status));
        BtTraceError("DeviceInitialize failed - the UART would not configure\n");

        goto Exit;
    }

#ifdef DYNAMIC_ENUM

    {
        WDF_OBJECT_ATTRIBUTES   ObjAttributes;
        WDF_WORKITEM_CONFIG WorkitemConfig;
        WDFWORKITEM WorkItem;
        PENABLE_PDO_CONTEXT Context;

        PAGED_CODE();

        DoTrace(LEVEL_INFO, TFLAG_PNP, ("+CreateWorkItem to enable PDO"));

        WDF_OBJECT_ATTRIBUTES_INIT(&ObjAttributes);

        WDF_OBJECT_ATTRIBUTES_SET_CONTEXT_TYPE(&ObjAttributes,
                                               ENABLE_PDO_CONTEXT);
        ObjAttributes.ParentObject = _Device;

        WDF_WORKITEM_CONFIG_INIT(&WorkitemConfig, DeviceEnablePDOWorker);
        Status = WdfWorkItemCreate(&WorkitemConfig, &ObjAttributes, &WorkItem);

        if (NT_SUCCESS(Status))
        {

            Context = GetEnablePdoWorkItemContext(WorkItem);
            Context->Fdo = _Device;

            WdfWorkItemEnqueue(WorkItem);
        }
    }
#else

    Status = FdoCreateAllChildren(_Device);
    BtTraceStatus(Status, "FdoCreateAllChildren -> 0x%08X\n", Status);
#endif

Exit:

    DoTrace(LEVEL_INFO, TFLAG_PNP, ("-FdoDevPrepareHardware %!STATUS!", Status));
    BtTraceStatus(Status, "PrepareHardware exit 0x%08X\n", Status);

    return Status;
}

NTSTATUS
FdoDevReleaseHardware(
    _In_  WDFDEVICE     _Device,
    _In_  WDFCMRESLIST  _ResourcesTranslated
    )

{
    PFDO_EXTENSION FdoExtension;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(_ResourcesTranslated);

    DoTrace(LEVEL_INFO, TFLAG_PNP,("+PnpReleaseHardware"));

    FdoExtension = FdoGetExtension(_Device);

    FdoExtension->DeviceInitialized = FALSE;
    InterlockedExchange(&FdoExtension->PowerTransitionInProgress, 1);
    InterlockedExchange(&FdoExtension->ResetInProgress, 1);
    DeviceDisableWakeControl(_Device);

    if (FdoExtension->DevWakeReleaseWorkItem != NULL)
    {
        WdfWorkItemFlush(FdoExtension->DevWakeReleaseWorkItem);
        WdfObjectDelete(FdoExtension->DevWakeReleaseWorkItem);
        FdoExtension->DevWakeReleaseWorkItem = NULL;
    }

    if (FdoExtension->PowerLock != NULL)
    {
        WdfObjectDelete(FdoExtension->PowerLock);
        FdoExtension->PowerLock = NULL;
    }

    if ((FdoExtension->IoTargetRegOn != NULL) &&
        (FdoExtension->IoTargetDevWake != NULL))
    {
        (VOID)DeviceEnable(_Device, FALSE);
    }

    if (FdoExtension->HostWakeInterrupt != NULL)
    {
        WdfObjectDelete(FdoExtension->HostWakeInterrupt);
        FdoExtension->HostWakeInterrupt = NULL;
    }
    FdoExtension->HostWakeOperational = FALSE;

    if (FdoExtension->IoTargetDevWake)
    {
        WdfObjectDelete(FdoExtension->IoTargetDevWake);
        FdoExtension->IoTargetDevWake = NULL;
    }

    if (FdoExtension->IoTargetRegOn)
    {
        WdfObjectDelete(FdoExtension->IoTargetRegOn);
        FdoExtension->IoTargetRegOn = NULL;
    }

    if (FdoExtension->RequestIoctlSync)
    {
        WdfObjectDelete(FdoExtension->RequestIoctlSync);
        FdoExtension->RequestIoctlSync = NULL;
    }

    if (FdoExtension->RequestWaitOnError)
    {
        WdfObjectDelete(FdoExtension->RequestWaitOnError);
        FdoExtension->RequestWaitOnError = NULL;
    }

    if (FdoExtension->WaitMaskMemory)
    {
        WdfObjectDelete(FdoExtension->WaitMaskMemory);
        FdoExtension->WaitMaskMemory = NULL;
    }

    if (FdoExtension->IoTargetSerial)
    {
        WdfObjectDelete(FdoExtension->IoTargetSerial);
        FdoExtension->IoTargetSerial = NULL;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
FdoDevSelfManagedIoInit(
    _In_  WDFDEVICE  _Device
)

{
    NTSTATUS       Status;
    PFDO_EXTENSION FdoExtension;

    PAGED_CODE();

    DoTrace(LEVEL_INFO, TFLAG_PNP,("+FdoDevSelfManagedIoInit"));
    BtTrace("SelfManagedIoInit entry\n");

    Status = ReadResourcesAllocate(_Device);
    BtTraceStatus(Status, "ReadResourcesAllocate -> 0x%08X\n", Status);

    if (!NT_SUCCESS(Status)) {
        DoTrace(LEVEL_ERROR, TFLAG_IO, (" ReadResourcesAllocate failed %!STATUS!", Status));
        goto Exit;
    }

    FdoExtension = FdoGetExtension(_Device);
    FdoExtension->ReadContext.RequestState = REQUEST_COMPLETE;

    FdoExtension->ReadPumpRunning = TRUE;
    Status = ReadH4Packet(&FdoExtension->ReadContext,
                           FdoExtension->ReadRequest,
                           FdoExtension->ReadMemory,
                           FdoExtension->ReadBuffer,
                           sizeof(FdoExtension->ReadBuffer),
                           INITIAL_H4_READ_SIZE);

    BtTraceStatus(Status, "read pump start -> 0x%08X%s\n", Status,
            (Status == STATUS_PENDING) ? " (pending, which is normal)" : "");

    if (Status == STATUS_PENDING) {
        Status = STATUS_SUCCESS;
    }

    if (!NT_SUCCESS(Status)) {
        FdoExtension->ReadPumpRunning = FALSE;
        DoTrace(LEVEL_ERROR, TFLAG_IO, (" ReadH4Packet failed %!STATUS!", Status));
        goto Exit;
    }

Exit:

    BtTraceStatus(Status, "SelfManagedIoInit exit 0x%08X\n", Status);
    return Status;
}

VOID
FdoDevSelfManagedIoCleanup(
    _In_  WDFDEVICE  _Device
    )

{
    PAGED_CODE();

    DoTrace(LEVEL_INFO, TFLAG_PNP,("+FdoDevSelfManagedIoCleanup"));

    ReadResourcesFree(_Device);

    return;
}

NTSTATUS
FdoDevD0Entry(
    _In_   WDFDEVICE              _Device,
    _In_   WDF_POWER_DEVICE_STATE _PreviousState
    )

{
    PFDO_EXTENSION FdoExtension = FdoGetExtension(_Device);
    NTSTATUS       Status = STATUS_SUCCESS;
    BOOLEAN ResumeFromIdle = FdoExtension->SuspendedForIdle;

    UNREFERENCED_PARAMETER(_PreviousState);

    DoTrace(LEVEL_INFO, TFLAG_UART, ("+FdoDevD0Entry"));
    InterlockedExchange(
        &FdoExtension->PowerTransitionInProgress,
        1);

    FdoExtension->OutOfSyncErrorCount = 0;

    if (ResumeFromIdle) {
        Status = DevicePowerOn(_Device);
        if (!NT_SUCCESS(Status)) {
            goto Done;
        }
    } else if (!IsDeviceInitialized(FdoExtension)) {
        if (FdoExtension->GPIOConnectionCount != 0) {
            Status = DeviceEnable(_Device, TRUE);
            if (!NT_SUCCESS(Status)) {
                DoTrace(LEVEL_ERROR, TFLAG_PNP,("DeviceEnable failed %!STATUS!", Status));
                goto Done;
            }
        }
    }

    if (ResumeFromIdle || !IsDeviceInitialized(FdoExtension)) {
        Status = WdfIoTargetStart(FdoExtension->IoTargetSerial);
        if (!NT_SUCCESS(Status)) {
            DoTrace(LEVEL_ERROR, TFLAG_PNP, ("WdfIoTargetStart failed %!STATUS!", Status));
            goto Done;
        }
    }

    if (!ResumeFromIdle && !IsDeviceInitialized(FdoExtension)) {
        FdoExtension->DeviceInitialized = DeviceInitialize(
            FdoExtension,
            FdoExtension->IoTargetSerial,
            FdoExtension->RequestIoctlSync,
            TRUE);
        if (!IsDeviceInitialized(FdoExtension)) {
            Status = STATUS_DEVICE_NOT_READY;
            DoTrace(LEVEL_ERROR, TFLAG_PNP, ("DeviceInitialize failed!"));
            goto Done;
        }
    }

    if ((FdoExtension->ReadRequest != NULL) &&
        !FdoExtension->ReadPumpRunning) {
        DoTrace(LEVEL_INFO, TFLAG_IO, (" Restarting read pump"));
        FdoExtension->ReadContext.RequestState = REQUEST_COMPLETE;
        FdoExtension->ReadPumpRunning = TRUE;
        Status = ReadH4Packet(
            &FdoExtension->ReadContext,
            FdoExtension->ReadRequest,
            FdoExtension->ReadMemory,
            FdoExtension->ReadBuffer,
            sizeof(FdoExtension->ReadBuffer),
            INITIAL_H4_READ_SIZE);
        if (Status == STATUS_PENDING) {
            Status = STATUS_SUCCESS;
        }
        if (!NT_SUCCESS(Status)) {
            DoTrace(LEVEL_ERROR, TFLAG_IO, ("ReadH4Packet [0] failed %!STATUS!", Status));
            goto Done;
        }
    }

    if (ResumeFromIdle) {
        DeviceScheduleSleep(FdoExtension);
    }

Done:
    if (!NT_SUCCESS(Status)) {
        FdoExtension->DeviceInitialized = FALSE;
    }
    FdoExtension->SuspendedForIdle = FALSE;
    InterlockedExchange(
        &FdoExtension->PowerTransitionInProgress,
        0);

    DoTrace(LEVEL_INFO, TFLAG_UART, ("-FdoDevD0Entry %!STATUS!", Status));

    return Status;
}

NTSTATUS
FdoDevD0Exit(
    _In_   WDFDEVICE              _Device,
    _In_   WDF_POWER_DEVICE_STATE _TargetState
    )

{
    PFDO_EXTENSION FdoExtension = FdoGetExtension(_Device);
    NTSTATUS Status = STATUS_SUCCESS;
    BOOLEAN PreserveController;

    PAGED_CODE();

    DoTrace(LEVEL_INFO, TFLAG_UART, ("+FdoDevD0Exit D0-> D%d", _TargetState-WdfPowerDeviceD0));
    InterlockedExchange(
        &FdoExtension->PowerTransitionInProgress,
        1);

    PreserveController =
        (BOOLEAN)(FdoExtension->HostWakeOperational &&
                  DeviceSleepModeRequested() &&
                  (_TargetState == WdfPowerDeviceD2));
    if (!PreserveController) {
        DeviceDisableWakeControl(_Device);
    }
    if (!PreserveController) {
        FdoExtension->DeviceInitialized = FALSE;
    }

    WdfIoTargetStop(
        FdoExtension->IoTargetSerial,
        WdfIoTargetCancelSentIo);

    if (!PreserveController) {
        ReadPacketListsPurge(FdoExtension);
    }

    FdoExtension->SuspendedForIdle = PreserveController;
    if (FdoExtension->IoTargetDevWake != NULL)
    {
        Status = PreserveController ?
            DevicePowerOff(_Device) :
            DeviceEnable(_Device, FALSE);
        if (!NT_SUCCESS(Status))
        {
            DoTrace(LEVEL_ERROR, TFLAG_POWER,
                    ("D0 exit power sequencing failed %!STATUS!", Status));
        }
    }

    if (!PreserveController) {
        InterlockedExchange(&FdoExtension->TxWakeReferences, 0);
    }

    DoTrace(LEVEL_INFO, TFLAG_UART, ("-FdoDevD0Exit %!STATUS!", Status));

    return Status;
}

NTSTATUS
HCIContextValidate(
    ULONG _Index,
    PBTHX_HCI_READ_WRITE_CONTEXT _HCIContext
    )

{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Index;

    DoTrace(LEVEL_INFO, TFLAG_HCI,("+HCIContextValidate"));

    switch ((BTHX_HCI_PACKET_TYPE) _HCIContext->Type)
    {
    case HciPacketCommand:
        {
            PHCI_COMMAND_PACKET HciCommand = (PHCI_COMMAND_PACKET) _HCIContext->Data;
            DoTrace(LEVEL_INFO, TFLAG_HCI, (" -> HCI Command [%d] OpCode: 0x%x, nParams: %d ---->",
                _Index,
                HciCommand->Opcode,
                HciCommand->ParamsCount));

            for (Index = 0; Index < MinToPrint((ULONG) HciCommand->ParamsCount, MAX_COMMAND_PARAMS_TO_DISPLAY); Index++)
            {
                DoTrace(LEVEL_INFO, TFLAG_HCI, ("    [%d] 0x%.2x",
                    Index, HciCommand->Params[Index]));
            }

            if (!WithinRange(MIN_HCI_CMD_SIZE, _HCIContext->DataLen, MAX_HCI_CMD_SIZE))
            {
                Status = STATUS_INVALID_PARAMETER;
                DoTrace(LEVEL_ERROR, TFLAG_HCI,(" HciPacketCommand %!STATUS!", Status));
                break;
            }
        }
        break;

    case HciPacketEvent:
        {
            PHCI_EVENT_PACKET HciEvent = (PHCI_EVENT_PACKET) _HCIContext->Data;
            DoTrace(LEVEL_INFO, TFLAG_HCI, (" <- HCI Event [%d] EventCode: 0x%x (%S), nParams: %d",
                    _Index,
                    HciEvent->EventCode,
                    HciEvent->EventCode == CommandComplete ? L"Complete" :
                    HciEvent->EventCode == CommandStatus ? L"Status(Async)!!" : L"??",
                    HciEvent->ParamsCount));

            for (Index = 0; Index < MinToPrint((ULONG) HciEvent->ParamsCount, MAX_EVENT_PARAMS_TO_DISPLAY); Index++)
            {
                DoTrace(LEVEL_VERBOSE, TFLAG_HCI, ("    [%d] 0x%.2x",
                        Index, HciEvent->Params[Index]));
            }

            if (!WithinRange(MIN_HCI_EVENT_SIZE, _HCIContext->DataLen, MAX_HCI_EVENT_SIZE))
            {
                Status = STATUS_INVALID_PARAMETER;
                DoTrace(LEVEL_ERROR, TFLAG_HCI,(" HciPacketEvent %!STATUS!", Status));
                break;
            }
        }
        break;

    case HciPacketAclData:
        {
            PHCI_ACLDATA_PACKET AclData = (PHCI_ACLDATA_PACKET) _HCIContext->Data;
            DoTrace(LEVEL_INFO, TFLAG_HCI, (" HCI Data [%d] (Handle:0x%x, PB:%x, BC:%x, Length:%d)",
                _Index,
                AclData->ConnectionHandle,
                AclData->PBFlag,
                AclData->BCFlag,
                AclData->DataLength));

            for (Index = 0; Index < (ULONG) (AclData->DataLength > 8 ? 8 : AclData->DataLength); Index++)
            {
                DoTrace(LEVEL_VERBOSE, TFLAG_HCI, ("    [%d] 0x%.2x",
                        Index, AclData->Data[Index]));
            }

            if (!WithinRange(1, AclData->DataLength, MAX_HCI_ACLDATA_SIZE))
            {
                Status = STATUS_INVALID_PARAMETER;
                DoTrace(LEVEL_ERROR, TFLAG_HCI,(" HciPacketAclData data (%d) exceeds its max %d, %!STATUS!",
                        AclData->DataLength, MAX_HCI_ACLDATA_SIZE, Status));
                break;
            }
        }
        break;

    default:
        DoTrace(LEVEL_ERROR, TFLAG_HCI, (" Packet type %d unexpected!", _HCIContext->Type));
        Status = STATUS_INVALID_PARAMETER;
        break;
    }

    NT_ASSERT(NT_SUCCESS(Status) && L"Invlaid data is detected!");

    DoTrace(LEVEL_INFO, TFLAG_HCI,("-HCIContextValidate %!STATUS!", Status));

    return Status;
}

NTSTATUS
FdoWriteDeviceIO(
    _In_  WDFREQUEST      _RequestFromBthport,
    _In_  WDFDEVICE       _Device,
    _In_  PFDO_EXTENSION  _FdoExtension,
    _In_  PBTHX_HCI_READ_WRITE_CONTEXT _HCIContext
    )

{
    WDF_OBJECT_ATTRIBUTES ObjAttributes;
    NTSTATUS Status;
    WDFREQUEST RequestToUART;
    PUART_WRITE_CONTEXT   TransferContext = NULL;
    ULONG  DataLength;
    PVOID   Data = NULL;

    DoTrace(LEVEL_INFO, TFLAG_DATA,("+FdoWriteDeviceIO"));

    if (!IsDeviceInitialized(_FdoExtension))
    {
        Status = STATUS_DEVICE_NOT_READY;
        DoTrace(LEVEL_ERROR, TFLAG_IO, (" FdoWriteDeviceIO: cannot attach IO %!STATUS!", Status));
        goto Done;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&ObjAttributes,
                                            UART_WRITE_CONTEXT);

    Status = WdfObjectAllocateContext(_RequestFromBthport,
                                      &ObjAttributes,
                                      &TransferContext);
    if (!NT_SUCCESS(Status)) {
        DoTrace(LEVEL_ERROR, TFLAG_IO, (" WdfObjectAllocateContext failed %!STATUS!", Status));
        goto Done;
    }
    TransferContext->FdoExtension = _FdoExtension;
    TransferContext->WakeReferenceHeld = FALSE;

    Status = HLP_AllocateResourceForWrite(
                    _Device,
                    _FdoExtension->IoTargetSerial,
                    &RequestToUART);
    if (!NT_SUCCESS(Status))
    {
        DoTrace(LEVEL_ERROR, TFLAG_IO,(" HLP_WriteDeviceIO %!STATUS!", Status));
        goto Done;
    }
    TransferContext->RequestToUART = RequestToUART;

    WDF_OBJECT_ATTRIBUTES_INIT(&ObjAttributes);
    ObjAttributes.ParentObject = _Device;

    Data = (PVOID) &_HCIContext->Type;
    DataLength = (ULONG) sizeof(_HCIContext->Type) + _HCIContext->DataLen;

    _Analysis_assume_(DataLength > 0);
    Status = WdfMemoryCreatePreallocated(&ObjAttributes,
                                         Data,
                                         DataLength,
                                         &TransferContext->Memory);
    if (!NT_SUCCESS(Status))
    {
        DoTrace(LEVEL_ERROR, TFLAG_IO, (" WdfMemoryCreatePreallocated failed %!STATUS!", Status));
        goto Done;
    }

    Status = WdfIoTargetFormatRequestForWrite(_FdoExtension->IoTargetSerial,
                                              RequestToUART,
                                              TransferContext->Memory,
                                              NULL,
                                              NULL);
    if (!NT_SUCCESS(Status))
    {
        DoTrace(LEVEL_ERROR, TFLAG_IO, (" WdfIoTargetFormatRequestForRead failed %!STATUS!", Status));
        goto Done;
    }

    TransferContext->FdoExtension       = _FdoExtension;
    TransferContext->HCIContext         = _HCIContext;
    TransferContext->RequestFromBthport = _RequestFromBthport;
    TransferContext->RequestCompletePath = REQUEST_PATH_NONE;
    TransferContext->RequestToUART      = RequestToUART;
    TransferContext->HCIPacket          = Data;
    TransferContext->HCIPacketLen       = DataLength;

    Status = DeviceAcquireTxWake(
        _FdoExtension,
        &TransferContext->WakeReferenceHeld);
    if (!NT_SUCCESS(Status)) {
        goto Done;
    }

    WdfObjectReference(RequestToUART);
    WdfObjectReference(_RequestFromBthport);

    WdfRequestMarkCancelable(_RequestFromBthport, CB_RequestFromBthportCancel);

    WdfRequestSetCompletionRoutine(RequestToUART, CR_WriteDeviceIO, TransferContext);

    if (!WdfRequestSend(RequestToUART, _FdoExtension->IoTargetSerial, WDF_NO_SEND_OPTIONS))
    {
        NTSTATUS StatusTemp;

        Status = WdfRequestGetStatus(RequestToUART);

        StatusTemp = WdfRequestUnmarkCancelable(_RequestFromBthport);

        WdfObjectDereference(RequestToUART);
        WdfObjectDereference(_RequestFromBthport);

        DoTrace(LEVEL_ERROR, TFLAG_IO, (" WdfRequestSend failed %!STATUS! and UnmarkCancelable %!STATUS!", Status, StatusTemp));
        goto Done;
    }
    else
    {

    }

Done:

    if (!NT_SUCCESS(Status))
    {
        if ((TransferContext != NULL) && TransferContext->WakeReferenceHeld) {
            TransferContext->WakeReferenceHeld = FALSE;
            DeviceReleaseTxWake(_FdoExtension);
        }
        HLP_FreeResourceForWrite(TransferContext);
    }

    DoTrace(LEVEL_INFO, TFLAG_IO, ("-FdoWriteDeviceIO %!STATUS!", Status));

    return Status;
}

NTSTATUS
FdoWriteToDeviceSync(
    _In_  WDFIOTARGET  _IoTargetSerial,
    _In_  WDFREQUEST   _RequestWriteSync,
    _In_  ULONG        _IoControlCode,
    _In_opt_ ULONG     _InBufferSize,
    _In_opt_ PVOID     _InBuffer,
    _Out_  PULONG_PTR  _BytesWritten
)

{
    NTSTATUS Status = STATUS_SUCCESS;
    WDF_REQUEST_REUSE_PARAMS RequestReuseParams;
    WDF_REQUEST_SEND_OPTIONS  Options;
    WDF_MEMORY_DESCRIPTOR MemoryDescriptor;
    ULONG_PTR BytesWritten = 0;
    BOOLEAN HasInputParam = FALSE;

    PAGED_CODE();

    DoTrace(LEVEL_INFO, TFLAG_IO,("+FdoWriteToDeviceSync"));

    WDF_REQUEST_REUSE_PARAMS_INIT(&RequestReuseParams, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
    Status = WdfRequestReuse(_RequestWriteSync, &RequestReuseParams);
    if (!NT_SUCCESS(Status))
    {
        DoTrace(LEVEL_ERROR, TFLAG_IO, (" WdfRequestReuse failed %!STATUS!", Status));
        goto Done;
    }

    if (_InBuffer && _InBufferSize) {
        HasInputParam = TRUE;
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&MemoryDescriptor,
                                          _InBuffer,
                                          _InBufferSize);
    }

    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&Options, WDF_REL_TIMEOUT_IN_SEC(MAX_WRITE_TIMEOUT_IN_SEC));

    if (_IoControlCode)
    {
        Status = WdfIoTargetSendIoctlSynchronously(_IoTargetSerial,
                                                   NULL,
                                                   _IoControlCode,
                                                   HasInputParam ? &MemoryDescriptor : NULL,
                                                   NULL,
                                                   &Options,
                                                   &BytesWritten
                                                   );
    }
    else
    {
        Status = WdfIoTargetSendWriteSynchronously(_IoTargetSerial,
                                                   NULL,
                                                   HasInputParam ? &MemoryDescriptor : NULL,
                                                   NULL,
                                                   &Options,
                                                   &BytesWritten
                                                   );
    }

    DoTrace(LEVEL_INFO, TFLAG_IO,("-FdoWriteToDeviceSync: %d BytesWritten %!STATUS!", (ULONG) BytesWritten, Status));

    if (NT_SUCCESS(Status))
    {
        *_BytesWritten = BytesWritten;
    }

Done:
    return Status;
}

VOID
FdoIoQuDeviceControl(
    _In_  WDFQUEUE     _Queue,
    _In_  WDFREQUEST   _Request,
    _In_  size_t       _OutputBufferLength,
    _In_  size_t       _InputBufferLength,
    _In_  ULONG        _IoControlCode
    )

{
    WDFMEMORY   ReqInMemory = NULL, ReqOutMemory = NULL;
    PVOID   InBuffer = NULL, OutBuffer = NULL;
    size_t  InBufferSize = 0, OutBufferSize = 0;
    PFDO_EXTENSION   FdoExtension;
    NTSTATUS    Status = STATUS_SUCCESS;
    WDFDEVICE   Device;
    BOOLEAN CompleteRequest = FALSE;
    ULONG ControlCode = (_IoControlCode & 0x00003ffc) >> 2;
    BTHX_HCI_PACKET_TYPE PacketType;
    PBTHX_HCI_READ_WRITE_CONTEXT HCIContext;
    BOOLEAN TraceThis;

    DoTrace(LEVEL_INFO, TFLAG_IOCTL,("+IoDeviceControl - InBufLen:%d, OutBufLen:%d",
        (ULONG) _InputBufferLength, (ULONG) _OutputBufferLength));

    Device = WdfIoQueueGetDevice(_Queue);

    FdoExtension = FdoGetExtension(Device);

    if (_InputBufferLength)
    {
        Status = WdfRequestRetrieveInputMemory(_Request, &ReqInMemory);
        if (NT_SUCCESS(Status))
        {
            InBuffer = WdfMemoryGetBuffer(ReqInMemory, &InBufferSize);
        }
    }

    if (_OutputBufferLength)
    {
        Status = WdfRequestRetrieveOutputMemory(_Request, &ReqOutMemory);
        if (NT_SUCCESS(Status))
        {
            OutBuffer = WdfMemoryGetBuffer(ReqOutMemory, &OutBufferSize);
        }
    }

    TraceThis = BtShouldTraceIoctl();
    if (TraceThis) {
        BtTraceVerbose("bthx %s (0x%08X) in %u out %u\n",
                BtIoctlName(_IoControlCode), _IoControlCode,
                (ULONG)_InputBufferLength, (ULONG)_OutputBufferLength);
    }

    switch (_IoControlCode)
    {
    case IOCTL_BTHX_WRITE_HCI:
        DoTrace(LEVEL_INFO, TFLAG_IOCTL,(" IOCTL_BTHX_WRITE_HCI ---------->"));

        if (!InBuffer || InBufferSize < sizeof(BTHX_HCI_READ_WRITE_CONTEXT) ||
            !OutBuffer || OutBufferSize != sizeof(BTHX_HCI_PACKET_TYPE))
        {
            Status = STATUS_INVALID_PARAMETER;
            DoTrace(LEVEL_ERROR, TFLAG_IOCTL,(" IOCTL_BTHX_WRITE_HCI %!STATUS!", Status));
            break;
        }

        HCIContext = (PBTHX_HCI_READ_WRITE_CONTEXT) InBuffer;

        PacketType = (BTHX_HCI_PACKET_TYPE) HCIContext->Type;

        if (!BTHX_VALID_WRITE_PACKET_TYPE(PacketType))
        {
            Status = STATUS_INVALID_PARAMETER;
            DoTrace(LEVEL_ERROR, TFLAG_IOCTL,(" Mismach Write HCI packet type and IOCTL %!STATUS!", Status));
            break;
        }

        if (PacketType == HciPacketCommand)
        {
            InterlockedIncrement(&FdoExtension->CntCommandReq);
        }
        else
        {
            InterlockedIncrement(&FdoExtension->CntWriteDataReq);
        }

        Status = FdoWriteDeviceIO(_Request,
                                  Device,
                                  FdoExtension,
                                  HCIContext);
        break;

    case IOCTL_BTHX_READ_HCI:
        DoTrace(LEVEL_INFO, TFLAG_IOCTL,(" IOCTL_BTHX_READ_HCI <----------"));
        if (!IsDeviceInitialized(FdoExtension)) {
            Status = STATUS_DEVICE_NOT_READY;
            break;
        }

        if (!InBuffer || InBufferSize != sizeof(BTHX_HCI_PACKET_TYPE) ||
            !OutBuffer || OutBufferSize < sizeof(BTHX_HCI_READ_WRITE_CONTEXT))
        {
            Status = STATUS_INVALID_PARAMETER;
            DoTrace(LEVEL_ERROR, TFLAG_IOCTL,(" IOCTL_BTHX_READ_HCI %!STATUS!", Status));
            break;
        }

        PacketType = *((BTHX_HCI_PACKET_TYPE *) InBuffer);

        if (!BTHX_VALID_READ_PACKET_TYPE(PacketType))
        {
            Status = STATUS_INVALID_PARAMETER;
            DoTrace(LEVEL_ERROR, TFLAG_IOCTL,(" IOCTL_BTHX_READ_HCI %!STATUS!", Status));
            break;
        }

        if (PacketType == HciPacketEvent)
        {
            WdfSpinLockAcquire(FdoExtension->QueueAccessLock);

              Status = WdfRequestForwardToIoQueue(_Request, FdoExtension->ReadEventQueue);
              if (NT_SUCCESS(Status))
              {
                  InterlockedIncrement(&FdoExtension->EventQueueCount);
                  InterlockedIncrement(&FdoExtension->CntEventReq);
              }
            WdfSpinLockRelease(FdoExtension->QueueAccessLock);

            if (NT_SUCCESS(Status))
            {
                Status = ReadRequestComplete(FdoExtension,
                                             HciPacketEvent,
                                             0, NULL,
                                             FdoExtension->ReadEventQueue,
                                             &FdoExtension->EventQueueCount,
                                             &FdoExtension->ReadEventList,
                                             &FdoExtension->EventListCount);
            }

        }
        else if (PacketType == HciPacketAclData)
        {
            WdfSpinLockAcquire(FdoExtension->QueueAccessLock);

              Status = WdfRequestForwardToIoQueue(_Request, FdoExtension->ReadDataQueue);
              if (NT_SUCCESS(Status))
              {
                  InterlockedIncrement(&FdoExtension->DataQueueCount);
                  InterlockedIncrement(&FdoExtension->CntReadDataReq);
              }
            WdfSpinLockRelease(FdoExtension->QueueAccessLock);

            if (NT_SUCCESS(Status))
            {
                Status = ReadRequestComplete(FdoExtension,
                                             HciPacketAclData,
                                             0, NULL,
                                             FdoExtension->ReadDataQueue,
                                             &FdoExtension->DataQueueCount,
                                             &FdoExtension->ReadDataList,
                                             &FdoExtension->DataListCount);
            }
        }
        else
        {
            Status = STATUS_INVALID_PARAMETER;
            DoTrace(LEVEL_ERROR, TFLAG_IOCTL,(" IOCTL_BTHX_READ_HCI %!STATUS!", Status));
            break;
        }
        break;

    case IOCTL_BTHX_GET_VERSION:
        CompleteRequest = TRUE;
        DoTrace(LEVEL_INFO, TFLAG_IOCTL,("IOCTL_BTHX_GET_VERSION"));

        if (OutBuffer && OutBufferSize >= sizeof(BTHX_VERSION))
        {
            RtlCopyMemory(OutBuffer, &Microsoft_BTHX_DDI_Version, sizeof(BTHX_VERSION));
            BtTraceVerbose("bthx GET_VERSION -> %u\n",
                    Microsoft_BTHX_DDI_Version.Version);
            WdfRequestCompleteWithInformation(_Request, Status, sizeof(BTHX_VERSION));
            return;
        }
        else
        {
            Status = STATUS_INVALID_PARAMETER;
        }
        break;

    case IOCTL_BTHX_SET_VERSION:
        CompleteRequest = TRUE;
        DoTrace(LEVEL_INFO, TFLAG_IOCTL,("IOCTL_BTHX_SET_VERSION"));

        if (InBuffer && InBufferSize >= sizeof(BTHX_VERSION))
        {
            BTHX_VERSION SupportedVersion = *((BTHX_VERSION *)InBuffer);

            DoTrace(LEVEL_INFO, TFLAG_IOCTL,("IOCTL_BTHX_SET_VERSION 0x%x", SupportedVersion.Version));
            BtTraceVerbose("bthx SET_VERSION %u (we report %u)\n",
                    SupportedVersion.Version,
                    Microsoft_BTHX_DDI_Version.Version);

            WdfRequestComplete(_Request, Status);
            return;
        }
        else
        {
            Status = STATUS_INVALID_PARAMETER;
        }
        break;

    case IOCTL_BTHX_QUERY_CAPABILITIES:
        CompleteRequest = TRUE;
        DoTrace(LEVEL_INFO, TFLAG_IOCTL,("IOCTL_BTHX_QUERY_CAPABILITIES"));

        if (OutBuffer && OutBufferSize >= sizeof(BTHX_CAPABILITIES))
        {
            BTHX_CAPABILITIES *pCaps = (BTHX_CAPABILITIES *) OutBuffer;

            RtlCopyMemory(pCaps, &FdoExtension->BthXCaps, sizeof(BTHX_CAPABILITIES));
            BtTraceVerbose("bthx QUERY_CAPABILITIES -> maxAclIn %u, sco %u, scoChannels %u,\n"
                    "     idleCapable %u, wakeCapable %u\n",
                    pCaps->MaxAclTransferInSize,
                    (ULONG)pCaps->ScoSupport,
                    pCaps->MaxScoChannels,
                    (ULONG)pCaps->IsDeviceIdleCapable,
                    (ULONG)pCaps->IsDeviceWakeCapable);
            WdfRequestCompleteWithInformation(_Request, Status, sizeof(BTHX_CAPABILITIES));
            return;
        }
        else
        {
            Status = STATUS_INVALID_PARAMETER;
        }
        break;

    case IOCTL_BUSENUM_SET_RADIO_ONOFF_VENDOR_SPECFIC:
        CompleteRequest = TRUE;
        DoTrace(LEVEL_INFO, TFLAG_IOCTL,("IOCTL_BUSENUM_SET_RADIO_ONOFF_VENDOR_SPECFIC"));
        if (InBuffer && InBufferSize >= sizeof(BOOLEAN)) {
            BOOLEAN IsRadioEnabled = *((BOOLEAN *) InBuffer);

            if (IsRadioEnabled) {
                if (!FdoExtension->IsRadioEnabled) {

                    Status = DevicePowerOn(Device);

                    if (NT_SUCCESS(Status)) {

                        Status = FdoCreateOneChildDevice(Device,
                                                         BT_PDO_HARDWARE_IDS,
                                                         BLUETOOTH_FUNC_IDS);
                        if (NT_SUCCESS(Status)) {
                            FdoExtension->IsRadioEnabled = TRUE;
                        }
                    }
                    DoTrace(LEVEL_INFO, TFLAG_IOCTL,(" EnableRadio %!STATUS!", Status));
                }
                else {
                    Status = STATUS_SUCCESS;
                    DoTrace(LEVEL_WARNING, TFLAG_IOCTL,(" Already enabled!"));
                }
            }
            else {
                if (FdoExtension->IsRadioEnabled) {

                    Status = FdoRemoveOneChildDevice(Device,
                                                     BLUETOOTH_FUNC_IDS);
                    if (NT_SUCCESS(Status)) {
                        FdoExtension->IsRadioEnabled = FALSE;

                        Status = DevicePowerOff(Device);
                    }

                    DoTrace(LEVEL_INFO, TFLAG_IOCTL,(" DisableRadio %!STATUS!", Status));
                }
                else {
                    Status = STATUS_SUCCESS;
                    DoTrace(LEVEL_WARNING, TFLAG_IOCTL,(" Already disabled!"));
                }
            }
        }
        else {
            Status = STATUS_INVALID_PARAMETER;
        }
        break;

    default:
        DoTrace(LEVEL_INFO, TFLAG_IOCTL,(" IOCTL_(0x%x, Func %d)", _IoControlCode, ControlCode));
        if (TraceThis) {
            BtTraceVerbose("bthx UNHANDLED ioctl 0x%08X (function %u) -> NOT_SUPPORTED\n",
                    _IoControlCode, ControlCode);
        }
        Status = STATUS_NOT_SUPPORTED;
        break;
    }

    if (TraceThis && !NT_SUCCESS(Status)) {
        BtTraceVerbose("bthx %s -> 0x%08X\n", BtIoctlName(_IoControlCode), Status);
    }

    if (!NT_SUCCESS(Status) || CompleteRequest)
    {
        WdfRequestComplete(_Request, Status);
    }

    return;
}
