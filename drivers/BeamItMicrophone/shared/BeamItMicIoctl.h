#pragma once

#ifdef _KERNEL_MODE
#include <wdm.h>
#else
#include <windows.h>
#include <winioctl.h>
#endif

#define BEAMIT_MIC_DRIVER_VERSION 2u
#define BEAMIT_MIC_SAMPLE_RATE_HZ 48000u
#define BEAMIT_MIC_CHANNELS 2u
#define BEAMIT_MIC_BITS_PER_SAMPLE 16u
#define BEAMIT_MIC_PACKET_DURATION_MS 10u
#define BEAMIT_MIC_FRAMES_PER_PACKET ((BEAMIT_MIC_SAMPLE_RATE_HZ * BEAMIT_MIC_PACKET_DURATION_MS) / 1000u)
#define BEAMIT_MIC_BYTES_PER_FRAME (BEAMIT_MIC_CHANNELS * BEAMIT_MIC_BITS_PER_SAMPLE / 8u)
#define BEAMIT_MIC_PACKET_BYTES (BEAMIT_MIC_FRAMES_PER_PACKET * BEAMIT_MIC_BYTES_PER_FRAME)
#define BEAMIT_MIC_QUEUE_PACKETS 3u

#define BEAMIT_MIC_DEVICE_TYPE 0xB440u
#define BEAMIT_MIC_DEVICE_NAME L"\\Device\\BeamItMicCtl"
#define BEAMIT_MIC_DOS_NAME L"\\DosDevices\\BeamItMicCtl"
#define BEAMIT_MIC_USER_PATH L"\\\\.\\BeamItMicCtl"

enum BeamItMicIoctlFunction : UINT32 {
    BeamItMicFnAcquire = 0x800u,
    BeamItMicFnRelease = 0x801u,
    BeamItMicFnWritePcm = 0x802u,
    BeamItMicFnReset = 0x803u,
    BeamItMicFnGetStats = 0x804u
};

#define IOCTL_BEAMIT_MIC_ACQUIRE \
    CTL_CODE(BEAMIT_MIC_DEVICE_TYPE, BeamItMicFnAcquire, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_BEAMIT_MIC_RELEASE \
    CTL_CODE(BEAMIT_MIC_DEVICE_TYPE, BeamItMicFnRelease, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_BEAMIT_MIC_WRITE_PCM \
    CTL_CODE(BEAMIT_MIC_DEVICE_TYPE, BeamItMicFnWritePcm, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_BEAMIT_MIC_RESET \
    CTL_CODE(BEAMIT_MIC_DEVICE_TYPE, BeamItMicFnReset, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_BEAMIT_MIC_GET_STATS \
    CTL_CODE(BEAMIT_MIC_DEVICE_TYPE, BeamItMicFnGetStats, METHOD_BUFFERED, FILE_READ_DATA)

struct BeamItMicFormat {
    UINT32 sample_rate_hz;
    UINT16 channels;
    UINT16 bits_per_sample;
    UINT32 frames_per_packet;
};

struct BeamItMicAcquireRequest {
    UINT32 version;
    UINT32 process_id;
    BeamItMicFormat format;
};

enum BeamItMicPcmFlags : UINT32 {
    BeamItMicPcmDiscontinuity = 0x1u
};

struct BeamItMicPcmHeader {
    UINT32 version;
    UINT32 flags;
    UINT64 sequence;
    UINT64 sample_timestamp;
    UINT32 frame_count;
    UINT32 reserved;
};

struct BeamItMicStats {
    UINT32 owner_process_id;
    UINT32 leased;
    UINT64 packets_accepted;
    UINT64 frames_accepted;
    UINT64 packets_rejected;
    UINT64 underrun_frames;
    UINT64 overrun_frames;
    UINT32 queued_frames;
    UINT32 capturing;
};

static_assert(sizeof(BeamItMicFormat) == 12);
static_assert(sizeof(BeamItMicAcquireRequest) == 20);
static_assert(sizeof(BeamItMicPcmHeader) == 32);
static_assert(sizeof(BeamItMicStats) == 56);
