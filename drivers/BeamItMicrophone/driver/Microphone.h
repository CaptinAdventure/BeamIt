#pragma once

#include <ntifs.h>
#include <portcls.h>
#include "../shared/PcmQueue.h"

namespace beamit::mic {

constexpr ULONG pool_tag = 'MiMB';

struct DeviceState {
    KSPIN_LOCK lock;
    PFILE_OBJECT owner;
    ULONG owner_pid;
    bool online;
    bool powered;
    bool capturing;
    LONG stream_count;
    UINT64 epoch;
    PcmQueue queue;
    BeamItMicStats stats;
};

extern DeviceState state;
extern PDEVICE_OBJECT control_device;

void set_available(bool online, bool powered);
void set_capturing(bool capturing);
NTSTATUS create_control(PDRIVER_OBJECT driver);
void delete_control();
NTSTATUS control_dispatch(PDEVICE_OBJECT device, PIRP irp);
NTSTATUS register_audio(PDEVICE_OBJECT device, PIRP irp, PRESOURCELIST resources);

} // namespace beamit::mic
