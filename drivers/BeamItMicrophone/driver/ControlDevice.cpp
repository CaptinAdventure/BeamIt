#include "Microphone.h"

#include <wdmsec.h>

namespace beamit::mic {
namespace {

const GUID control_class = {0xc8d28c96, 0xb8ce, 0x4812, {0xa5, 0x6c, 0xd9, 0xdb, 0x67, 0x21, 0x64, 0x0b}};
UNICODE_STRING dos_name{};

class Locked {
public:
    _IRQL_requires_max_(DISPATCH_LEVEL)
    _IRQL_raises_(DISPATCH_LEVEL)
    _IRQL_saves_global_(MicLock, this)
    Locked() { KeAcquireSpinLock(&state.lock, &irql_); }
    _IRQL_requires_(DISPATCH_LEVEL)
    _IRQL_restores_global_(MicLock, this)
    ~Locked() { KeReleaseSpinLock(&state.lock, irql_); }
private:
    KIRQL irql_;
};

void clear_queue() {
    state.queue.reset();
    ++state.epoch;
}

void release_owner() {
    state.owner = nullptr;
    state.owner_pid = 0;
    clear_queue();
}

bool format_supported(const BeamItMicFormat& format) {
    return format.sample_rate_hz == BEAMIT_MIC_SAMPLE_RATE_HZ &&
           format.channels == BEAMIT_MIC_CHANNELS &&
           format.bits_per_sample == BEAMIT_MIC_BITS_PER_SAMPLE &&
           format.frames_per_packet == BEAMIT_MIC_FRAMES_PER_PACKET;
}

NTSTATUS ioctl(PIRP irp, PIO_STACK_LOCATION stack, ULONG_PTR& written) {
    const auto code = stack->Parameters.DeviceIoControl.IoControlCode;
    const auto input_size = stack->Parameters.DeviceIoControl.InputBufferLength;
    const auto output_size = stack->Parameters.DeviceIoControl.OutputBufferLength;
    auto* buffer = irp->AssociatedIrp.SystemBuffer;
    const auto file = stack->FileObject;
    const Locked lock;

    if (code == IOCTL_BEAMIT_MIC_GET_STATS) {
        if (output_size < sizeof(BeamItMicStats)) return STATUS_BUFFER_TOO_SMALL;
        auto stats = state.stats;
        stats.owner_process_id = state.owner_pid;
        stats.leased = state.owner ? 1 : 0;
        stats.queued_frames = state.queue.queued();
        stats.capturing = state.online && state.powered && state.capturing;
        RtlCopyMemory(buffer, &stats, sizeof(stats));
        written = sizeof(stats);
        return STATUS_SUCCESS;
    }
    if (!state.online || !state.powered) return STATUS_DEVICE_NOT_READY;
    if (code == IOCTL_BEAMIT_MIC_ACQUIRE) {
        if (input_size != sizeof(BeamItMicAcquireRequest)) return STATUS_INVALID_BUFFER_SIZE;
        const auto& request = *static_cast<const BeamItMicAcquireRequest*>(buffer);
        if (request.version != BEAMIT_MIC_DRIVER_VERSION) return STATUS_REVISION_MISMATCH;
        if (!format_supported(request.format)) return STATUS_NOT_SUPPORTED;
        if (state.owner) return STATUS_DEVICE_BUSY;
        state.owner = file;
        // Never trust the PID supplied in the request as an authorization credential.
        state.owner_pid = IoGetRequestorProcessId(irp);
        clear_queue();
        return STATUS_SUCCESS;
    }
    if (!state.owner) return STATUS_DEVICE_NOT_READY;
    if (state.owner != file) return STATUS_ACCESS_DENIED;
    switch (code) {
    case IOCTL_BEAMIT_MIC_RELEASE:
        if (input_size) return STATUS_INVALID_BUFFER_SIZE;
        release_owner();
        return STATUS_SUCCESS;
    case IOCTL_BEAMIT_MIC_RESET:
        if (input_size) return STATUS_INVALID_BUFFER_SIZE;
        clear_queue();
        return STATUS_SUCCESS;
    case IOCTL_BEAMIT_MIC_WRITE_PCM: {
        if (input_size != sizeof(BeamItMicPcmHeader) + BEAMIT_MIC_PACKET_BYTES)
            return STATUS_INVALID_BUFFER_SIZE;
        const auto& packet = *static_cast<const BeamItMicPcmHeader*>(buffer);
        if (!state.queue.valid(packet)) {
            ++state.stats.packets_rejected;
            return STATUS_INVALID_PARAMETER;
        }
        if (!state.capturing) return STATUS_DEVICE_NOT_READY;
        const auto samples = reinterpret_cast<const INT16*>(
            static_cast<const UCHAR*>(buffer) + sizeof(BeamItMicPcmHeader));
        if (packet.flags & BeamItMicPcmDiscontinuity) ++state.epoch;
        state.stats.overrun_frames += state.queue.write(packet, samples);
        ++state.stats.packets_accepted;
        state.stats.frames_accepted += packet.frame_count;
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_INVALID_DEVICE_REQUEST;
    }
}

} // namespace

void set_available(bool online, bool powered) {
    const Locked lock;
    state.online = online;
    state.powered = powered;
    if (!online || !powered) release_owner();
}

void set_capturing(bool capturing) {
    const Locked lock;
    state.capturing = capturing;
    clear_queue();
}

NTSTATUS control_dispatch(PDEVICE_OBJECT, PIRP irp) {
    const auto stack = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR written{};
    switch (stack->MajorFunction) {
    case IRP_MJ_CREATE:
        status = stack->FileObject->FileName.Length ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_SUCCESS;
        break;
    case IRP_MJ_CLEANUP: {
        const Locked lock;
        if (state.owner == stack->FileObject) release_owner();
        status = STATUS_SUCCESS;
        break;
    }
    case IRP_MJ_CLOSE:
        status = STATUS_SUCCESS;
        break;
    case IRP_MJ_DEVICE_CONTROL:
        status = ioctl(irp, stack, written);
        break;
    default:
        break;
    }
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = written;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

NTSTATUS create_control(PDRIVER_OBJECT driver) {
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, BEAMIT_MIC_DEVICE_NAME);
    UNICODE_STRING security;
    // Interactive local users may feed audio; network and anonymous identities may not.
    RtlInitUnicodeString(&security, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)");
    auto status = IoCreateDeviceSecure(driver, 0, &name, BEAMIT_MIC_DEVICE_TYPE,
        FILE_DEVICE_SECURE_OPEN, FALSE, &security, &control_class, &control_device);
    if (!NT_SUCCESS(status)) return status;
    RtlInitUnicodeString(&dos_name, BEAMIT_MIC_DOS_NAME);
    status = IoCreateSymbolicLink(&dos_name, &name);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(control_device);
        control_device = nullptr;
        return status;
    }
    control_device->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

void delete_control() {
    if (control_device) {
        IoDeleteSymbolicLink(&dos_name);
        IoDeleteDevice(control_device);
        control_device = nullptr;
    }
}

} // namespace beamit::mic
