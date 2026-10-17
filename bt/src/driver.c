/*
 * SPDX-License-Identifier: MS-PL
 * Copyright (c) Microsoft Corporation All Rights Reserved
 * See bt/LICENSE-MS-PL.txt for the license text.
 */

#include "driver.h"
#include "driver.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, DriverCleanup)
#pragma alloc_text (PAGE, DriverSetDeviceCallbackEvents)
#pragma alloc_text (PAGE, DriverDeviceAdd)
#pragma alloc_text (INIT, DriverEntry)
#endif

VOID
DriverCleanup(
    _In_  WDFOBJECT _Object
    )

{
    PAGED_CODE();

    DoTrace(LEVEL_INFO, TFLAG_PNP,("+DriverCleanup"));

    WPP_CLEANUP( WdfDriverWdmGetDriverObject( _Object ));
}

VOID
DriverSetDeviceCallbackEvents(
    _In_  PWDFDEVICE_INIT  _DeviceInit
    )

{
    WDF_POWER_POLICY_EVENT_CALLBACKS PowerPolicyCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;

    PAGED_CODE();

    DoTrace(LEVEL_INFO, TFLAG_PNP,("+DriverSetDeviceCallbackEvents"));

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);

    PnpPowerCallbacks.EvtDevicePrepareHardware = FdoDevPrepareHardware;
    PnpPowerCallbacks.EvtDeviceReleaseHardware = FdoDevReleaseHardware;

    PnpPowerCallbacks.EvtDeviceD0Entry = FdoDevD0Entry;
    PnpPowerCallbacks.EvtDeviceD0Exit  = FdoDevD0Exit;
    PnpPowerCallbacks.EvtDeviceSelfManagedIoInit    = FdoDevSelfManagedIoInit;
    PnpPowerCallbacks.EvtDeviceSelfManagedIoCleanup = FdoDevSelfManagedIoCleanup;

    WdfDeviceInitSetPnpPowerEventCallbacks(_DeviceInit,
                                           &PnpPowerCallbacks);

    WDF_POWER_POLICY_EVENT_CALLBACKS_INIT(&PowerPolicyCallbacks);

    PowerPolicyCallbacks.EvtDeviceArmWakeFromS0    = FdoEvtDeviceArmWake;
    PowerPolicyCallbacks.EvtDeviceDisarmWakeFromS0 = FdoEvtDeviceDisarmWake;

    WdfDeviceInitSetPowerPolicyEventCallbacks(_DeviceInit,
                                              &PowerPolicyCallbacks);
}

NTSTATUS
DriverDeviceAdd(
    IN  WDFDRIVER        _Driver,
    IN  PWDFDEVICE_INIT  _DeviceInit
    )

{
    WDF_IO_QUEUE_CONFIG        QueueConfig;
    WDF_OBJECT_ATTRIBUTES      Attributes;
    NTSTATUS                   Status;
    WDFDEVICE                  Device;
    PFDO_EXTENSION             FdoExtension;
    WDFQUEUE                   Queue;
    PNP_BUS_INFORMATION        BusInfo;
    WDF_DEVICE_STATE           DeviceState;
#ifdef DYNAMIC_ENUM
    WDF_CHILD_LIST_CONFIG      Config;
#endif

    PAGED_CODE();

    BtLogLoadSettings(WdfFdoInitWdmGetPhysicalDevice(_DeviceInit));
    DoTrace(LEVEL_INFO, TFLAG_PNP, ("+DriverDeviceAdd: 0x%p", _Driver));
    BtTrace("DeviceAdd entry\n");

    DriverSetDeviceCallbackEvents(_DeviceInit);

    WdfDeviceInitSetDeviceType(_DeviceInit, FILE_DEVICE_BUS_EXTENDER);

#ifdef DYNAMIC_ENUM

    WDF_CHILD_LIST_CONFIG_INIT(&Config,
                                sizeof(PDO_IDENTIFICATION_DESCRIPTION),
                                FdoEvtDeviceListCreatePdo
                                );

    WdfFdoInitSetDefaultChildListConfig(_DeviceInit,
                                         &Config,
                                         WDF_NO_OBJECT_ATTRIBUTES);
#endif

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, FDO_EXTENSION);

    Status = WdfDeviceCreate(&_DeviceInit,
                             &Attributes,
                             &Device);
    if (!NT_SUCCESS(Status)) {
        DoTrace(LEVEL_ERROR, TFLAG_PNP, (" WdfDeveiceCreate failed %!STATUS!", Status));
        return Status;
    }

    WDF_DEVICE_STATE_INIT(&DeviceState);
    DeviceState.NotDisableable = WdfFalse;
    WdfDeviceSetDeviceState(Device, &DeviceState);

    FdoExtension = FdoGetExtension(Device);

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;

    Status = WdfWaitLockCreate(&Attributes, &FdoExtension->ChildLock);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig,
                                           WdfIoQueueDispatchParallel);
    QueueConfig.PowerManaged = WdfTrue;

    QueueConfig.EvtIoDeviceControl = FdoIoQuDeviceControl;

    __analysis_assume(QueueConfig.EvtIoStop != 0);
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ExecutionLevel = WdfExecutionLevelPassive;
    Status = WdfIoQueueCreate(Device,
                              &QueueConfig,
                              &Attributes,
                              &Queue);
    __analysis_assume(QueueConfig.EvtIoStop == 0);
    if (!NT_SUCCESS(Status)) {
        DoTrace(LEVEL_ERROR, TFLAG_PNP, (" WdfIoQueueCreate failed %!STATUS!", Status));
        return Status;
    }
    FdoExtension->DefaultQueue = Queue;

    Status = WdfDeviceCreateDeviceInterface(Device,
                                            &GUID_DEVINTERFACE_BLUETOOTH_RADIO_ONOFF_VENDOR_SPECIFIC,
                                            NULL    );
    if (!NT_SUCCESS(Status)) {
        DoTrace(LEVEL_ERROR, TFLAG_PNP, (" WdfDeviceCreateDeviceInterface failed %!STATUS!", Status));
        return Status;
    }

    BusInfo.BusTypeGuid = GUID_SERENUM_BUS_ENUMERATOR;
    BusInfo.LegacyBusType = PNPBus;
    BusInfo.BusNumber = 0;

    WdfDeviceSetBusInformationForChildren(Device, &BusInfo);

    DoTrace(LEVEL_INFO, TFLAG_PNP, ("-DriverDeviceAdd: exit %!STATUS!", Status));

    return Status;
}

NTSTATUS
DriverEntry(
    _In_  PDRIVER_OBJECT  _DriverObject,
    _In_  PUNICODE_STRING _RegistryPath
    )

{
    WDF_DRIVER_CONFIG   Config;
    NTSTATUS            Status;
    WDF_OBJECT_ATTRIBUTES Attributes;

    WDF_DRIVER_CONFIG_INIT(&Config, DriverDeviceAdd);
    Config.DriverPoolTag = POOLTAG_AP6256BT;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.EvtCleanupCallback = DriverCleanup;

    WPP_INIT_TRACING(_DriverObject, _RegistryPath);

    Status = WdfDriverCreate(_DriverObject,
                             _RegistryPath,
                             &Attributes,
                             &Config,
                             WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status))
    {
        DoTrace(LEVEL_ERROR, TFLAG_PNP, ("WdfDriverCreate failed %!STATUS!", Status));
        WPP_CLEANUP(_DriverObject);
    }

    BtTraceStatus(Status, "DriverEntry v%s built %s -> 0x%08X\n",
            AP6256BT_VER_STR, AP6256_BUILD_STAMP, Status);
    return Status;

}
