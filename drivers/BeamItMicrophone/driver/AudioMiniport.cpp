#include "Microphone.h"
#include "../shared/CaptureClock.h"

#include <ksmedia.h>

inline void* __cdecl operator new(size_t, void* memory) noexcept { return memory; }
void __cdecl operator delete(void* memory, size_t) noexcept {
    if (memory) ExFreePoolWithTag(memory, beamit::mic::pool_tag);
}

namespace beamit::mic {
namespace {

const GUID microphone_name = {0x25cefe60, 0x79bf, 0x4192, {0xb0, 0x43, 0x17, 0x2c, 0xdb, 0xdd, 0x04, 0x80}};
constexpr ULONG bytes_per_second = BEAMIT_MIC_SAMPLE_RATE_HZ * BEAMIT_MIC_BYTES_PER_FRAME;
constexpr ULONG timer_period_100ns = 10'000; // One ms; audio positions come from QPC, not timer tick counts.

KSDATAFORMAT_WAVEFORMATEXTENSIBLE device_format = {
    {sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE), 0, BEAMIT_MIC_BYTES_PER_FRAME, 0,
     STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO), STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
     STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)},
    {{WAVE_FORMAT_EXTENSIBLE, BEAMIT_MIC_CHANNELS, BEAMIT_MIC_SAMPLE_RATE_HZ,
      bytes_per_second, BEAMIT_MIC_BYTES_PER_FRAME, BEAMIT_MIC_BITS_PER_SAMPLE,
      sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)},
     BEAMIT_MIC_BITS_PER_SAMPLE, KSAUDIO_SPEAKER_STEREO, STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM)}
};

KSDATARANGE analog_range = {
    sizeof(KSDATARANGE), 0, 0, 0, STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
    STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG), STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)};
KSDATARANGE_AUDIO pcm_range = {
    {sizeof(KSDATARANGE_AUDIO), 0, 0, 0, STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
     STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM), STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)},
    BEAMIT_MIC_CHANNELS, BEAMIT_MIC_BITS_PER_SAMPLE, BEAMIT_MIC_BITS_PER_SAMPLE,
    BEAMIT_MIC_SAMPLE_RATE_HZ, BEAMIT_MIC_SAMPLE_RATE_HZ};
PKSDATARANGE analog_ranges[]{&analog_range};
PKSDATARANGE pcm_ranges[]{&pcm_range.DataRange};

PCPIN_DESCRIPTOR topology_pins[] = {
    {0, 0, 0, nullptr, {0, nullptr, 0, nullptr, 1, analog_ranges, KSPIN_DATAFLOW_IN,
        KSPIN_COMMUNICATION_NONE, &KSNODETYPE_MICROPHONE, &microphone_name, 0}},
    {0, 0, 0, nullptr, {0, nullptr, 0, nullptr, 1, analog_ranges, KSPIN_DATAFLOW_OUT,
        KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, nullptr, 0}}
};
PCCONNECTION_DESCRIPTOR topology_connections[]{{PCFILTER_NODE, 0, PCFILTER_NODE, 1}};
PCFILTER_DESCRIPTOR topology_filter = {
    0, nullptr, sizeof(PCPIN_DESCRIPTOR), RTL_NUMBER_OF(topology_pins), topology_pins,
    0, 0, nullptr, RTL_NUMBER_OF(topology_connections), topology_connections, 0, nullptr};

NTSTATUS wave_property(PPCPROPERTY_REQUEST request);
PCPROPERTY_ITEM wave_properties[]{
    {&KSPROPSETID_Pin, KSPROPERTY_PIN_PROPOSEDATAFORMAT,
     KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT, wave_property}
};
DEFINE_PCAUTOMATION_TABLE_PROP(wave_automation, wave_properties);
PCPIN_DESCRIPTOR wave_pins[] = {
    {0, 0, 0, nullptr, {0, nullptr, 0, nullptr, 1, analog_ranges, KSPIN_DATAFLOW_IN,
        KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, nullptr, 0}},
    {1, 1, 0, nullptr, {0, nullptr, 0, nullptr, 1, pcm_ranges, KSPIN_DATAFLOW_OUT,
        KSPIN_COMMUNICATION_SINK, &KSCATEGORY_AUDIO, &KSAUDFNAME_RECORDING_CONTROL, 0}}
};
PCNODE_DESCRIPTOR wave_nodes[]{{0, nullptr, &KSNODETYPE_ADC, nullptr}};
PCCONNECTION_DESCRIPTOR wave_connections[]{
    {PCFILTER_NODE, 0, 0, 1}, {0, 0, PCFILTER_NODE, 1}
};
PCFILTER_DESCRIPTOR wave_filter = {
    0, &wave_automation, sizeof(PCPIN_DESCRIPTOR), RTL_NUMBER_OF(wave_pins), wave_pins,
    sizeof(PCNODE_DESCRIPTOR), RTL_NUMBER_OF(wave_nodes), wave_nodes,
    RTL_NUMBER_OF(wave_connections), wave_connections, 0, nullptr};

bool valid_format(PKSDATAFORMAT format) {
    if (!format || format->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX) ||
        !IsEqualGUIDAligned(format->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) ||
        !IsEqualGUIDAligned(format->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) ||
        !IsEqualGUIDAligned(format->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) return false;
    const auto wave = reinterpret_cast<PKSDATAFORMAT_WAVEFORMATEX>(format);
    const auto& pcm = wave->WaveFormatEx;
    if (pcm.nSamplesPerSec != BEAMIT_MIC_SAMPLE_RATE_HZ || pcm.nChannels != BEAMIT_MIC_CHANNELS ||
        pcm.wBitsPerSample != BEAMIT_MIC_BITS_PER_SAMPLE ||
        pcm.nBlockAlign != BEAMIT_MIC_BYTES_PER_FRAME || pcm.nAvgBytesPerSec != bytes_per_second) return false;
    if (pcm.wFormatTag == WAVE_FORMAT_PCM) return pcm.cbSize == 0;
    if (pcm.wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        format->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE) ||
        pcm.cbSize != sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) return false;
    const auto& extended = reinterpret_cast<PKSDATAFORMAT_WAVEFORMATEXTENSIBLE>(format)->WaveFormatExt;
    return extended.Samples.wValidBitsPerSample == BEAMIT_MIC_BITS_PER_SAMPLE &&
        extended.dwChannelMask == KSAUDIO_SPEAKER_STEREO &&
        IsEqualGUIDAligned(extended.SubFormat, KSDATAFORMAT_SUBTYPE_PCM);
}

NTSTATUS wave_property(PPCPROPERTY_REQUEST request) {
    if (request->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        if (request->ValueSize < sizeof(ULONG)) return STATUS_BUFFER_TOO_SMALL;
        constexpr ULONG access = KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT;
        if (request->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION)) {
            auto description = static_cast<PKSPROPERTY_DESCRIPTION>(request->Value);
            RtlZeroMemory(description, sizeof(*description));
            description->AccessFlags = access;
            description->DescriptionSize = sizeof(*description);
            description->PropTypeSet.Set = KSPROPTYPESETID_General;
            description->PropTypeSet.Id = VT_ILLEGAL;
            request->ValueSize = sizeof(*description);
        } else {
            *static_cast<ULONG*>(request->Value) = access;
            request->ValueSize = sizeof(ULONG);
        }
        return STATUS_SUCCESS;
    }
    if (request->InstanceSize < sizeof(ULONG) * 2 || !request->Instance) return STATUS_INVALID_PARAMETER;
    const auto pin = *static_cast<const ULONG*>(request->Instance);
    if (pin != 1 || request->ValueSize < sizeof(KSDATAFORMAT_WAVEFORMATEX)) return STATUS_INVALID_PARAMETER;
    const auto format = static_cast<PKSDATAFORMAT>(request->Value);
    if (format->FormatSize > request->ValueSize) return STATUS_INVALID_BUFFER_SIZE;
    return valid_format(format) ? STATUS_SUCCESS : STATUS_NO_MATCH;
}

NTSTATUS intersect(ULONG pin, PKSDATARANGE range, ULONG size, PVOID output, PULONG required) {
    if (pin != 1 || range->FormatSize < sizeof(KSDATARANGE_AUDIO) ||
        !IsEqualGUIDAligned(range->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) ||
        !IsEqualGUIDAligned(range->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) ||
        !IsEqualGUIDAligned(range->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) return STATUS_NO_MATCH;
    const auto audio = reinterpret_cast<PKSDATARANGE_AUDIO>(range);
    if (audio->MaximumChannels < BEAMIT_MIC_CHANNELS ||
        audio->MinimumBitsPerSample > BEAMIT_MIC_BITS_PER_SAMPLE ||
        audio->MaximumBitsPerSample < BEAMIT_MIC_BITS_PER_SAMPLE ||
        audio->MinimumSampleFrequency > BEAMIT_MIC_SAMPLE_RATE_HZ ||
        audio->MaximumSampleFrequency < BEAMIT_MIC_SAMPLE_RATE_HZ) return STATUS_NO_MATCH;
    *required = sizeof(device_format);
    if (!size) return STATUS_BUFFER_OVERFLOW;
    if (size < *required) return STATUS_BUFFER_TOO_SMALL;
    if (!output) return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(output, &device_format, sizeof(device_format));
    return STATUS_SUCCESS;
}

#define MIC_REFCOUNT(Class) \
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); } \
    STDMETHODIMP_(ULONG) Release() override { \
        const auto count = InterlockedDecrement(&refs_); \
        if (!count) { this->~Class(); ExFreePoolWithTag(this, pool_tag); } \
        return static_cast<ULONG>(count); \
    }

template<class T> T* allocate() {
    const auto memory = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(T), pool_tag);
    return memory ? new (memory) T() : nullptr;
}

class Topology final : public IMiniportTopology {
public:
    MIC_REFCOUNT(Topology)
    STDMETHODIMP_(NTSTATUS) QueryInterface(REFIID id, PVOID* result) override {
        *result = nullptr;
        if (!IsEqualGUIDAligned(id, IID_IUnknown) && !IsEqualGUIDAligned(id, IID_IMiniport) &&
            !IsEqualGUIDAligned(id, IID_IMiniportTopology)) return STATUS_NOINTERFACE;
        *result = static_cast<IMiniportTopology*>(this);
        AddRef();
        return STATUS_SUCCESS;
    }
    STDMETHODIMP_(NTSTATUS) Init(PUNKNOWN, PRESOURCELIST, PPORTTOPOLOGY) override { return STATUS_SUCCESS; }
    STDMETHODIMP_(NTSTATUS) GetDescription(PPCFILTER_DESCRIPTOR* result) override {
        *result = &topology_filter;
        return STATUS_SUCCESS;
    }
    STDMETHODIMP_(NTSTATUS) DataRangeIntersection(ULONG, PKSDATARANGE, PKSDATARANGE,
                                                 ULONG, PVOID, PULONG) override { return STATUS_NOT_IMPLEMENTED; }
private:
    LONG refs_{1};
};

class CaptureStream final : public IMiniportWaveRTStreamNotification, public IMiniportWaveRTInputStream {
public:
    ~CaptureStream() {
        SetState(KSSTATE_STOP);
        if (mdl_) FreeAudioBuffer(mdl_, buffer_bytes_);
        if (timer_) ExDeleteTimer(timer_, TRUE, TRUE, nullptr);
        if (port_) port_->Release();
        InterlockedDecrement(&state.stream_count);
    }
    MIC_REFCOUNT(CaptureStream)
    STDMETHODIMP_(NTSTATUS) QueryInterface(REFIID id, PVOID* result) override {
        *result = nullptr;
        if (IsEqualGUIDAligned(id, IID_IUnknown) || IsEqualGUIDAligned(id, IID_IMiniportWaveRTStream) ||
            IsEqualGUIDAligned(id, IID_IMiniportWaveRTStreamNotification))
            *result = static_cast<IMiniportWaveRTStreamNotification*>(this);
        else if (IsEqualGUIDAligned(id, IID_IMiniportWaveRTInputStream))
            *result = static_cast<IMiniportWaveRTInputStream*>(this);
        else return STATUS_NOINTERFACE;
        AddRef();
        return STATUS_SUCCESS;
    }

    NTSTATUS initialize(PPORTWAVERTSTREAM port) {
        KeInitializeSpinLock(&lock_);
        port_ = port;
        port_->AddRef();
        frequency_ = 0;
        LARGE_INTEGER frequency;
        KeQueryPerformanceCounter(&frequency);
        frequency_ = static_cast<UINT64>(frequency.QuadPart);
        timer_ = ExAllocateTimer(timer_callback, this, EX_TIMER_HIGH_RESOLUTION);
        return timer_ ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
    }
    STDMETHODIMP_(NTSTATUS) SetFormat(PKSDATAFORMAT format) override {
        return valid_format(format) ? STATUS_SUCCESS : STATUS_NO_MATCH;
    }
    STDMETHODIMP_(NTSTATUS) SetState(KSSTATE next) override {
        if (next > KSSTATE_RUN) return STATUS_INVALID_PARAMETER;
        KIRQL irql;
        KeAcquireSpinLock(&lock_, &irql);
        if (next == stream_state_) {
            KeReleaseSpinLock(&lock_, irql);
            return STATUS_SUCCESS;
        }
        if (next == KSSTATE_RUN && !buffer_) {
            KeReleaseSpinLock(&lock_, irql);
            return STATUS_INVALID_DEVICE_STATE;
        }
        const auto was_running = stream_state_ == KSSTATE_RUN;
        stream_state_ = next;
        if (was_running) {
            if (timer_) ExCancelTimer(timer_, nullptr);
            set_capturing(false);
        }
        if (next == KSSTATE_STOP) {
            total_frames_ = base_frames_ = next_read_packet_ = 0;
            if (buffer_) RtlZeroMemory(buffer_, buffer_bytes_);
        }
        if (next == KSSTATE_RUN) {
            base_frames_ = total_frames_;
            start_qpc_ = static_cast<UINT64>(KeQueryPerformanceCounter(nullptr).QuadPart);
            set_capturing(true);
            ExSetTimer(timer_, -static_cast<LONGLONG>(timer_period_100ns), timer_period_100ns, nullptr);
        }
        KeReleaseSpinLock(&lock_, irql);
        return STATUS_SUCCESS;
    }
    STDMETHODIMP_(NTSTATUS) GetPosition(PKSAUDIO_POSITION position) override {
        KIRQL irql;
        KeAcquireSpinLock(&lock_, &irql);
        const auto offset = buffer_bytes_ ? (total_frames_ * BEAMIT_MIC_BYTES_PER_FRAME) % buffer_bytes_ : 0;
        position->PlayOffset = offset;
        position->WriteOffset = offset;
        KeReleaseSpinLock(&lock_, irql);
        return STATUS_SUCCESS;
    }
    STDMETHODIMP_(NTSTATUS) AllocateAudioBuffer(ULONG requested, PMDL* mdl, PULONG actual,
                                               PULONG offset, MEMORY_CACHING_TYPE* cache) override {
        return allocate_buffer(0, requested, mdl, actual, offset, cache);
    }
    STDMETHODIMP_(NTSTATUS) AllocateBufferWithNotification(ULONG count, ULONG requested, PMDL* mdl,
                            PULONG actual, PULONG offset, MEMORY_CACHING_TYPE* cache) override {
        if (count != 1 && count != 2) return STATUS_INVALID_PARAMETER;
        return allocate_buffer(count, requested, mdl, actual, offset, cache);
    }
    STDMETHODIMP_(void) FreeAudioBuffer(PMDL mdl, ULONG) override {
        SetState(KSSTATE_STOP);
        // Synchronize every possible timer callback before unmapping user-visible DMA pages.
        if (timer_) {
            ExDeleteTimer(timer_, TRUE, TRUE, nullptr);
            timer_ = nullptr;
        }
        if (mdl && mdl == mdl_) {
            KIRQL irql;
            KeAcquireSpinLock(&lock_, &irql);
            const auto buffer = buffer_;
            const auto descriptor = mdl_;
            buffer_ = nullptr;
            mdl_ = nullptr;
            buffer_bytes_ = 0;
            notification_count_ = 0;
            KeReleaseSpinLock(&lock_, irql);
            port_->UnmapAllocatedPages(buffer, descriptor);
            port_->FreePagesFromMdl(descriptor);
        }
    }
    STDMETHODIMP_(void) FreeBufferWithNotification(PMDL mdl, ULONG size) override { FreeAudioBuffer(mdl, size); }
    STDMETHODIMP_(void) GetHWLatency(KSRTAUDIO_HWLATENCY* latency) override { RtlZeroMemory(latency, sizeof(*latency)); }
    STDMETHODIMP_(NTSTATUS) GetPositionRegister(KSRTAUDIO_HWREGISTER*) override { return STATUS_NOT_SUPPORTED; }
    STDMETHODIMP_(NTSTATUS) GetClockRegister(KSRTAUDIO_HWREGISTER*) override { return STATUS_NOT_SUPPORTED; }

    STDMETHODIMP_(NTSTATUS) RegisterNotificationEvent(PKEVENT event) override {
        KIRQL irql;
        KeAcquireSpinLock(&lock_, &irql);
        NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;
        for (auto& entry : events_) {
            if (entry == event) { status = STATUS_OBJECT_NAME_COLLISION; break; }
        }
        if (status != STATUS_OBJECT_NAME_COLLISION) {
            for (auto& entry : events_) {
                if (!entry) { entry = event; status = STATUS_SUCCESS; break; }
            }
        }
        KeReleaseSpinLock(&lock_, irql);
        return status;
    }
    STDMETHODIMP_(NTSTATUS) UnregisterNotificationEvent(PKEVENT event) override {
        KIRQL irql;
        KeAcquireSpinLock(&lock_, &irql);
        NTSTATUS status = STATUS_NOT_FOUND;
        for (auto& entry : events_) {
            if (entry == event) { entry = nullptr; status = STATUS_SUCCESS; break; }
        }
        KeReleaseSpinLock(&lock_, irql);
        return status;
    }
    STDMETHODIMP_(NTSTATUS) GetReadPacket(PULONG number, DWORD* flags, ULONG64* qpc, BOOL* more) override {
        KIRQL irql;
        KeAcquireSpinLock(&lock_, &irql);
        const auto packet_frames = notification_count_
            ? buffer_bytes_ / BEAMIT_MIC_BYTES_PER_FRAME / notification_count_ : 0;
        const auto completed = packet_frames ? total_frames_ / packet_frames : 0;
        if (!completed || next_read_packet_ >= completed) {
            KeReleaseSpinLock(&lock_, irql);
            return STATUS_DEVICE_NOT_READY;
        }
        const auto retained = notification_count_ > 1 ? notification_count_ - 1 : 1;
        const auto oldest = completed > retained ? completed - retained : 0;
        const bool lost = next_read_packet_ < oldest;
        if (lost) next_read_packet_ = oldest;
        *number = static_cast<ULONG>(next_read_packet_);
        *flags = 0; // GetReadPacket reserves Flags; packet-number gaps report overwritten capture packets.
        *qpc = packet_qpc_[next_read_packet_ % RTL_NUMBER_OF(packet_qpc_)];
        ++next_read_packet_;
        *more = next_read_packet_ < completed;
        KeReleaseSpinLock(&lock_, irql);
        return STATUS_SUCCESS;
    }

private:
    NTSTATUS allocate_buffer(ULONG notifications, ULONG requested, PMDL* mdl, PULONG actual,
                             PULONG offset, MEMORY_CACHING_TYPE* cache) {
        *mdl = nullptr;
        *actual = *offset = 0;
        const auto alignment = BEAMIT_MIC_BYTES_PER_FRAME * (notifications ? notifications : 1);
        if (mdl_ || requested < BEAMIT_MIC_PACKET_BYTES || requested > bytes_per_second / 5 ||
            requested % alignment) return STATUS_INVALID_PARAMETER;
        if (!timer_) {
            timer_ = ExAllocateTimer(timer_callback, this, EX_TIMER_HIGH_RESOLUTION);
            if (!timer_) return STATUS_INSUFFICIENT_RESOURCES;
        }
        PHYSICAL_ADDRESS high;
        high.QuadPart = MAXLONGLONG;
        const auto allocated = port_->AllocatePagesForMdl(high, requested);
        if (!allocated) return STATUS_INSUFFICIENT_RESOURCES;
        const auto mapped = port_->MapAllocatedPages(allocated, MmCached);
        if (!mapped) {
            port_->FreePagesFromMdl(allocated);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(mapped, requested);
        KIRQL irql;
        KeAcquireSpinLock(&lock_, &irql);
        mdl_ = allocated;
        buffer_ = static_cast<UCHAR*>(mapped);
        buffer_bytes_ = requested;
        notification_count_ = notifications;
        KeReleaseSpinLock(&lock_, irql);
        *mdl = allocated;
        *actual = requested;
        *cache = MmCached;
        return STATUS_SUCCESS;
    }

    static void timer_callback(PEX_TIMER, PVOID context) { static_cast<CaptureStream*>(context)->tick(); }

    void tick() {
        KIRQL irql;
        KeAcquireSpinLock(&lock_, &irql);
        if (stream_state_ != KSSTATE_RUN || !buffer_) {
            KeReleaseSpinLock(&lock_, irql);
            return;
        }
        const auto qpc = static_cast<UINT64>(KeQueryPerformanceCounter(nullptr).QuadPart);
        const auto elapsed = qpc - start_qpc_;
        const auto target = base_frames_ + elapsed_frames(elapsed, frequency_);
        const auto buffer_frames = buffer_bytes_ / BEAMIT_MIC_BYTES_PER_FRAME;
        const auto packet_frames = notification_count_ ? buffer_frames / notification_count_ : buffer_frames;
        const auto before = total_frames_;
        KeAcquireSpinLockAtDpcLevel(&state.lock);
        if (epoch_ != state.epoch) {
            epoch_ = state.epoch;
            RtlZeroMemory(buffer_, buffer_bytes_);
        }
        if (target - total_frames_ > buffer_frames) {
            total_frames_ = target - buffer_frames;
            const auto first_packet = total_frames_ / packet_frames;
            packet_qpc_[first_packet % RTL_NUMBER_OF(packet_qpc_)] =
                qpc - frame_ticks(target - first_packet * packet_frames, frequency_);
        }
        while (total_frames_ < target) {
            const auto offset = static_cast<ULONG>(total_frames_ % buffer_frames);
            auto frames = static_cast<ULONG>(target - total_frames_);
            if (frames > buffer_frames - offset) frames = buffer_frames - offset;
            const auto within_packet = static_cast<ULONG>(total_frames_ % packet_frames);
            if (frames > packet_frames - within_packet) frames = packet_frames - within_packet;
            if (!within_packet) {
                packet_qpc_[(total_frames_ / packet_frames) % RTL_NUMBER_OF(packet_qpc_)] =
                    start_qpc_ + frame_ticks(total_frames_ - base_frames_, frequency_);
            }
            auto output = reinterpret_cast<INT16*>(buffer_ + offset * BEAMIT_MIC_BYTES_PER_FRAME);
            if (state.online && state.powered && state.owner)
                state.stats.underrun_frames += state.queue.read(output, frames);
            else RtlZeroMemory(output, frames * BEAMIT_MIC_BYTES_PER_FRAME);
            total_frames_ += frames;
        }
        KeReleaseSpinLockFromDpcLevel(&state.lock);
        if (notification_count_ && total_frames_ / packet_frames > before / packet_frames) {
            KeMemoryBarrier();
            for (const auto event : events_) if (event) KeSetEvent(event, IO_NO_INCREMENT, FALSE);
        }
        KeReleaseSpinLock(&lock_, irql);
    }

    LONG refs_{1};
    KSPIN_LOCK lock_{};
    PPORTWAVERTSTREAM port_{};
    PEX_TIMER timer_{};
    PMDL mdl_{};
    UCHAR* buffer_{};
    ULONG buffer_bytes_{};
    ULONG notification_count_{};
    PKEVENT events_[4]{};
    KSSTATE stream_state_{KSSTATE_STOP};
    UINT64 frequency_{};
    UINT64 start_qpc_{};
    UINT64 packet_qpc_[4]{};
    UINT64 base_frames_{};
    UINT64 total_frames_{};
    UINT64 next_read_packet_{};
    UINT64 epoch_{};
};

class WaveMiniport final : public IMiniportWaveRT, public IAdapterPowerManagement {
public:
    MIC_REFCOUNT(WaveMiniport)
    STDMETHODIMP_(NTSTATUS) QueryInterface(REFIID id, PVOID* result) override {
        *result = nullptr;
        if (IsEqualGUIDAligned(id, IID_IUnknown) || IsEqualGUIDAligned(id, IID_IMiniport) ||
            IsEqualGUIDAligned(id, IID_IMiniportWaveRT))
            *result = static_cast<IMiniportWaveRT*>(this);
        else if (IsEqualGUIDAligned(id, IID_IAdapterPowerManagement))
            *result = static_cast<IAdapterPowerManagement*>(this);
        else return STATUS_NOINTERFACE;
        AddRef();
        return STATUS_SUCCESS;
    }
    STDMETHODIMP_(NTSTATUS) Init(PUNKNOWN, PRESOURCELIST, PPORTWAVERT) override { return STATUS_SUCCESS; }
    STDMETHODIMP_(NTSTATUS) GetDescription(PPCFILTER_DESCRIPTOR* result) override {
        *result = &wave_filter;
        return STATUS_SUCCESS;
    }
    STDMETHODIMP_(NTSTATUS) DataRangeIntersection(ULONG pin, PKSDATARANGE range, PKSDATARANGE,
                                                 ULONG size, PVOID format, PULONG required) override {
        return intersect(pin, range, size, format, required);
    }
    STDMETHODIMP_(NTSTATUS) NewStream(PMINIPORTWAVERTSTREAM* result, PPORTWAVERTSTREAM port,
                                     ULONG pin, BOOLEAN capture, PKSDATAFORMAT format) override {
        *result = nullptr;
        if (pin != 1 || !capture || !valid_format(format)) return STATUS_NO_MATCH;
        if (InterlockedCompareExchange(&state.stream_count, 1, 0)) return STATUS_INSUFFICIENT_RESOURCES;
        const auto stream = allocate<CaptureStream>();
        if (!stream) {
            InterlockedDecrement(&state.stream_count);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        const auto status = stream->initialize(port);
        if (!NT_SUCCESS(status)) stream->Release();
        else *result = static_cast<IMiniportWaveRTStreamNotification*>(stream);
        return status;
    }
    STDMETHODIMP_(NTSTATUS) GetDeviceDescription(PDEVICE_DESCRIPTION description) override {
        RtlZeroMemory(description, sizeof(*description));
        description->Version = DEVICE_DESCRIPTION_VERSION;
        description->Master = TRUE;
        description->ScatterGather = TRUE;
        description->Dma32BitAddresses = TRUE;
        description->InterfaceType = Internal;
        description->MaximumLength = bytes_per_second / 5;
        return STATUS_SUCCESS;
    }
    STDMETHODIMP_(void) PowerChangeState(POWER_STATE next) override {
        KIRQL irql;
        KeAcquireSpinLock(&state.lock, &irql);
        state.powered = next.DeviceState == PowerDeviceD0;
        if (!state.powered) {
            state.owner = nullptr;
            state.owner_pid = 0;
            state.queue.reset();
            ++state.epoch;
        }
        KeReleaseSpinLock(&state.lock, irql);
    }
    STDMETHODIMP_(NTSTATUS) QueryPowerChangeState(POWER_STATE) override { return STATUS_SUCCESS; }
    STDMETHODIMP_(NTSTATUS) QueryDeviceCapabilities(PDEVICE_CAPABILITIES) override { return STATUS_SUCCESS; }
private:
    LONG refs_{1};
};

NTSTATUS install_subdevice(PDEVICE_OBJECT device, PIRP irp, PRESOURCELIST resources,
                           REFGUID port_id, PUNKNOWN miniport, PWSTR name, PPORT* result) {
    *result = nullptr;
    PPORT port{};
    auto status = PcNewPort(&port, port_id);
    if (!NT_SUCCESS(status)) return status;
    status = port->Init(device, irp, miniport, nullptr, resources);
    if (NT_SUCCESS(status)) status = PcRegisterSubdevice(device, name, port);
    if (!NT_SUCCESS(status)) port->Release();
    else *result = port;
    return status;
}

} // namespace

NTSTATUS register_audio(PDEVICE_OBJECT device, PIRP irp, PRESOURCELIST resources) {
    const auto topology = allocate<Topology>();
    const auto wave = allocate<WaveMiniport>();
    if (!topology || !wave) {
        if (topology) topology->Release();
        if (wave) wave->Release();
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    PPORT topology_port{};
    PPORT wave_port{};
    auto status = install_subdevice(device, irp, resources, CLSID_PortTopology, topology,
                                     const_cast<PWSTR>(L"TopologyMic"), &topology_port);
    if (NT_SUCCESS(status))
        status = install_subdevice(device, irp, resources, CLSID_PortWaveRT,
            static_cast<IMiniportWaveRT*>(wave), const_cast<PWSTR>(L"WaveMic"), &wave_port);
    if (NT_SUCCESS(status)) status = PcRegisterPhysicalConnection(device, topology_port, 1, wave_port, 0);
    if (NT_SUCCESS(status)) status = PcRegisterAdapterPowerManagement(static_cast<IMiniportWaveRT*>(wave), device);
    if (wave_port) wave_port->Release();
    if (topology_port) topology_port->Release();
    wave->Release();
    topology->Release();
    return status;
}

} // namespace beamit::mic
