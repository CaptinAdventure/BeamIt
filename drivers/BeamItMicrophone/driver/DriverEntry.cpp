#include "Microphone.h"

namespace beamit::mic {

DeviceState state{};
PDEVICE_OBJECT control_device{};

namespace {
PDRIVER_DISPATCH port_dispatch[IRP_MJ_MAXIMUM_FUNCTION + 1]{};
PDRIVER_UNLOAD port_unload{};
LONG adapter_added{};

NTSTATUS start_device(PDEVICE_OBJECT device, PIRP irp, PRESOURCELIST resources) {
    auto status = register_audio(device, irp, resources);
    if (NT_SUCCESS(status) && !control_device) status = create_control(device->DriverObject);
    if (NT_SUCCESS(status)) set_available(true, true);
    return status;
}

NTSTATUS add_device(PDRIVER_OBJECT driver, PDEVICE_OBJECT physical_device) {
    if (InterlockedCompareExchange(&adapter_added, 1, 0)) return STATUS_DEVICE_BUSY;
    const auto status = PcAddAdapterDevice(driver, physical_device, start_device, 2, 0);
    if (!NT_SUCCESS(status)) InterlockedExchange(&adapter_added, 0);
    return status;
}

_Dispatch_type_(IRP_MJ_CREATE)
_Dispatch_type_(IRP_MJ_CREATE_NAMED_PIPE)
_Dispatch_type_(IRP_MJ_CLOSE)
_Dispatch_type_(IRP_MJ_READ)
_Dispatch_type_(IRP_MJ_WRITE)
_Dispatch_type_(IRP_MJ_QUERY_INFORMATION)
_Dispatch_type_(IRP_MJ_SET_INFORMATION)
_Dispatch_type_(IRP_MJ_QUERY_EA)
_Dispatch_type_(IRP_MJ_SET_EA)
_Dispatch_type_(IRP_MJ_FLUSH_BUFFERS)
_Dispatch_type_(IRP_MJ_QUERY_VOLUME_INFORMATION)
_Dispatch_type_(IRP_MJ_SET_VOLUME_INFORMATION)
_Dispatch_type_(IRP_MJ_DIRECTORY_CONTROL)
_Dispatch_type_(IRP_MJ_FILE_SYSTEM_CONTROL)
_Dispatch_type_(IRP_MJ_CLEANUP)
_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)
_Dispatch_type_(IRP_MJ_INTERNAL_DEVICE_CONTROL)
_Dispatch_type_(IRP_MJ_SHUTDOWN)
_Dispatch_type_(IRP_MJ_LOCK_CONTROL)
_Dispatch_type_(IRP_MJ_CREATE_MAILSLOT)
_Dispatch_type_(IRP_MJ_QUERY_SECURITY)
_Dispatch_type_(IRP_MJ_SET_SECURITY)
_Dispatch_type_(IRP_MJ_DEVICE_CHANGE)
_Dispatch_type_(IRP_MJ_QUERY_QUOTA)
_Dispatch_type_(IRP_MJ_SET_QUOTA)
_Dispatch_type_(IRP_MJ_PNP)
_Dispatch_type_(IRP_MJ_POWER)
_Dispatch_type_(IRP_MJ_SYSTEM_CONTROL)
DRIVER_DISPATCH dispatch;

NTSTATUS dispatch(PDEVICE_OBJECT device, PIRP irp) {
    if (device->DeviceType == BEAMIT_MIC_DEVICE_TYPE) return control_dispatch(device, irp);
    const auto stack = IoGetCurrentIrpStackLocation(irp);
    if (stack->MajorFunction == IRP_MJ_PNP) {
        switch (stack->MinorFunction) {
        case IRP_MN_STOP_DEVICE:
        case IRP_MN_SURPRISE_REMOVAL:
        case IRP_MN_REMOVE_DEVICE:
            set_available(false, false);
            if (stack->MinorFunction == IRP_MN_REMOVE_DEVICE) {
                delete_control();
                InterlockedExchange(&adapter_added, 0);
            }
            break;
        default:
            break;
        }
    }
    return port_dispatch[stack->MajorFunction](device, irp);
}

void unload(PDRIVER_OBJECT driver) {
    set_available(false, false);
    delete_control();
    if (port_unload) port_unload(driver);
}

} // namespace

} // namespace beamit::mic

extern "C" DRIVER_INITIALIZE DriverEntry;

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry_path) {
    using namespace beamit::mic;
    KeInitializeSpinLock(&state.lock);
    auto status = PcInitializeAdapterDriver(driver, registry_path, add_device);
    if (!NT_SUCCESS(status)) return status;
    port_unload = driver->DriverUnload;
    for (ULONG index = 0; index <= IRP_MJ_MAXIMUM_FUNCTION; ++index) {
        port_dispatch[index] = driver->MajorFunction[index];
        driver->MajorFunction[index] = dispatch;
    }
    driver->DriverUnload = unload;
    return STATUS_SUCCESS;
}
