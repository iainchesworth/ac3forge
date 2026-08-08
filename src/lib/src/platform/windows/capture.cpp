#include "ac3/capture/capture.hpp"

// The Windows capture backend. CMake compiles this directory's capture.cpp on
// Windows and another platform directory's everywhere else, so there is no
// #ifdef here - the file's path is what says "Windows".

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
// windows.h must precede the audio headers.
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

namespace ac3::capture {

namespace {

using Microsoft::WRL::ComPtr;

// 100-ns units: the unit every WASAPI duration is expressed in.
constexpr REFERENCE_TIME kBufferDuration = 200'000;  // 20 ms
constexpr DWORD kPollIntervalMs = 5;

// PKEY_Device_FriendlyName, spelled out rather than pulling in
// functiondiscoverykeys_devpkey.h: that header needs DEFINE_PROPERTYKEY to
// already be defined by an <initguid.h>-style include ordering, which is
// easy to break and drags GUID definitions into this translation unit.
// {a45c254e-df1c-4efd-8020-67d146a850e0}, property id 14.
constexpr PROPERTYKEY kPkeyDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::string to_utf8(const wchar_t* wide) {
    if (wide == nullptr) {
        return {};
    }
    const int needed =
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

// COM lifetime for one thread. WASAPI is apartment-sensitive, so every thread
// that touches an interface initialises and uninitialises its own.
class ComScope {
public:
    ComScope() : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr_)) {
            CoUninitialize();
        }
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    [[nodiscard]] bool ok() const { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }

private:
    HRESULT hr_;
};

// How the endpoint hands us samples. WASAPI shared mode is almost always
// 32-bit float, but exclusive-capable devices can report packed integers.
enum class SampleFormat { kFloat32, kPcm16, kPcm24, kPcm32, kUnsupported };

SampleFormat classify(const WAVEFORMATEX* format) {
    GUID subformat{};
    WORD bits = format->wBitsPerSample;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        subformat = ext->SubFormat;
    } else if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        subformat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    } else if (format->wFormatTag == WAVE_FORMAT_PCM) {
        subformat = KSDATAFORMAT_SUBTYPE_PCM;
    } else {
        return SampleFormat::kUnsupported;
    }

    if (IsEqualGUID(subformat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && bits == 32) {
        return SampleFormat::kFloat32;
    }
    if (IsEqualGUID(subformat, KSDATAFORMAT_SUBTYPE_PCM)) {
        switch (bits) {
            case 16: return SampleFormat::kPcm16;
            case 24: return SampleFormat::kPcm24;
            case 32: return SampleFormat::kPcm32;
            default: break;
        }
    }
    return SampleFormat::kUnsupported;
}

// Convert one endpoint packet into normalised float, in place into `out`.
void convert(const BYTE* data, std::size_t sample_count, SampleFormat format,
             std::vector<float>& out) {
    out.resize(sample_count);
    switch (format) {
        case SampleFormat::kFloat32: {
            std::memcpy(out.data(), data, sample_count * sizeof(float));
            break;
        }
        case SampleFormat::kPcm16: {
            const auto* pcm = reinterpret_cast<const std::int16_t*>(data);
            for (std::size_t i = 0; i < sample_count; ++i) {
                out[i] = static_cast<float>(pcm[i]) / 32768.0f;
            }
            break;
        }
        case SampleFormat::kPcm24: {
            for (std::size_t i = 0; i < sample_count; ++i) {
                const BYTE* p = data + i * 3;
                const auto value = static_cast<std::int32_t>(
                    (static_cast<std::uint32_t>(p[0]) << 8) |
                    (static_cast<std::uint32_t>(p[1]) << 16) |
                    (static_cast<std::uint32_t>(p[2]) << 24));
                out[i] = static_cast<float>(value) / 2147483648.0f;
            }
            break;
        }
        case SampleFormat::kPcm32: {
            const auto* pcm = reinterpret_cast<const std::int32_t*>(data);
            for (std::size_t i = 0; i < sample_count; ++i) {
                out[i] = static_cast<float>(pcm[i]) / 2147483648.0f;
            }
            break;
        }
        case SampleFormat::kUnsupported:
            std::ranges::fill(out, 0.0f);
            break;
    }
}

std::expected<ComPtr<IMMDeviceEnumerator>, CaptureError> make_enumerator() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    const HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                        IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        return std::unexpected(CaptureError::kComFailure);
    }
    return enumerator;
}

void append_devices(IMMDeviceEnumerator* enumerator, EDataFlow flow, DeviceKind kind,
                    std::vector<DeviceInfo>& out) {
    ComPtr<IMMDevice> default_device;
    std::string default_id;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &default_device))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(default_device->GetId(&id))) {
            default_id = to_utf8(id);
            CoTaskMemFree(id);
        }
    }

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection))) {
        return;
    }
    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) {
        return;
    }

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) {
            continue;
        }
        DeviceInfo info;
        info.kind = kind;

        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id))) {
            info.id = to_utf8(id);
            CoTaskMemFree(id);
        }
        info.is_default = !info.id.empty() && info.id == default_id;

        ComPtr<IPropertyStore> properties;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
            PROPVARIANT name;
            PropVariantInit(&name);
            if (SUCCEEDED(properties->GetValue(kPkeyDeviceFriendlyName, &name)) &&
                name.vt == VT_LPWSTR) {
                info.name = to_utf8(name.pwszVal);
            }
            PropVariantClear(&name);
        }

        // The mixer format tells the caller the rate and channel count it
        // will actually receive, before committing to a capture.
        ComPtr<IAudioClient> client;
        if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED(client->GetMixFormat(&mix)) && mix != nullptr) {
                info.sample_rate = mix->nSamplesPerSec;
                info.channels = mix->nChannels;
                CoTaskMemFree(mix);
            }
        }
        if (kind == DeviceKind::kLoopback) {
            info.name += " (loopback)";
        }
        out.push_back(std::move(info));
    }
}

}  // namespace

std::string_view describe(CaptureError error) {
    switch (error) {
        case CaptureError::kNoBackend: return "no capture backend on this platform";
        case CaptureError::kComFailure: return "a Windows audio (WASAPI/COM) call failed";
        case CaptureError::kDeviceNotFound: return "the requested capture device was not found";
        case CaptureError::kFormatUnsupported: return "the device sample format is unsupported";
        case CaptureError::kAlreadyRunning: return "capture is already running";
    }
    return "unknown capture error";
}

std::expected<std::vector<DeviceInfo>, CaptureError> enumerate_devices() {
    ComScope com;
    if (!com.ok()) {
        return std::unexpected(CaptureError::kComFailure);
    }
    auto enumerator = make_enumerator();
    if (!enumerator) {
        return std::unexpected(enumerator.error());
    }

    std::vector<DeviceInfo> devices;
    append_devices(enumerator->Get(), eCapture, DeviceKind::kInput, devices);
    append_devices(enumerator->Get(), eRender, DeviceKind::kLoopback, devices);
    return devices;
}

struct Capture::Impl {
    std::unique_ptr<RingBuffer> ring;
    std::jthread worker;
    std::atomic_bool running{false};
    std::atomic<std::uint64_t> frames_captured{0};
    std::atomic<std::uint64_t> frames_silence{0};
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
};

Capture::Capture() : impl_(std::make_unique<Impl>()) {}

Capture::~Capture() {
    stop();
}

bool Capture::running() const {
    return impl_->running.load(std::memory_order_acquire);
}

std::uint32_t Capture::sample_rate() const {
    return impl_->sample_rate;
}

std::uint16_t Capture::channels() const {
    return impl_->channels;
}

CaptureStats Capture::stats() const {
    return {.frames_captured = impl_->frames_captured.load(std::memory_order_relaxed),
            .frames_silence_filled = impl_->frames_silence.load(std::memory_order_relaxed),
            .frames_dropped = impl_->ring ? impl_->ring->dropped() /
                                                std::max<std::size_t>(impl_->channels, 1)
                                          : 0};
}

RingBuffer* Capture::buffer() {
    return impl_->ring.get();
}

void Capture::stop() {
    if (impl_->worker.joinable()) {
        impl_->worker.request_stop();
        impl_->worker.join();
    }
    impl_->running.store(false, std::memory_order_release);
}

std::expected<void, CaptureError> Capture::start(const std::string& device_id, DeviceKind kind,
                                                 std::size_t ring_capacity_samples) {
    if (running()) {
        return std::unexpected(CaptureError::kAlreadyRunning);
    }

    // Open the device on this thread so format negotiation failures are
    // reported synchronously, then hand the client to the capture thread.
    ComScope com;
    if (!com.ok()) {
        return std::unexpected(CaptureError::kComFailure);
    }
    auto enumerator = make_enumerator();
    if (!enumerator) {
        return std::unexpected(enumerator.error());
    }
    const EDataFlow flow = kind == DeviceKind::kLoopback ? eRender : eCapture;

    ComPtr<IMMDevice> device;
    if (device_id.empty()) {
        if (FAILED((*enumerator)->GetDefaultAudioEndpoint(flow, eConsole, &device))) {
            return std::unexpected(CaptureError::kDeviceNotFound);
        }
    } else {
        const int wide_len =
            MultiByteToWideChar(CP_UTF8, 0, device_id.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(wide_len > 0 ? wide_len - 1 : 0), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, device_id.c_str(), -1, wide.data(), wide_len);
        if (FAILED((*enumerator)->GetDevice(wide.c_str(), &device))) {
            return std::unexpected(CaptureError::kDeviceNotFound);
        }
    }

    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
        return std::unexpected(CaptureError::kComFailure);
    }
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix)) || mix == nullptr) {
        return std::unexpected(CaptureError::kFormatUnsupported);
    }
    const SampleFormat format = classify(mix);
    const auto rate = mix->nSamplesPerSec;
    const auto channel_count = mix->nChannels;
    if (format == SampleFormat::kUnsupported || channel_count == 0) {
        CoTaskMemFree(mix);
        return std::unexpected(CaptureError::kFormatUnsupported);
    }

    // Loopback is polled rather than event-driven: a render endpoint signals
    // nothing at all while the machine is silent, so an event wait would
    // stall instead of letting us synthesise the silence.
    const bool loopback = kind == DeviceKind::kLoopback;
    DWORD flags = loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    const HRESULT init =
        client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration, 0, mix, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(init)) {
        return std::unexpected(CaptureError::kComFailure);
    }

    HANDLE sample_ready = nullptr;
    if (!loopback) {
        sample_ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (sample_ready == nullptr || FAILED(client->SetEventHandle(sample_ready))) {
            if (sample_ready != nullptr) {
                CloseHandle(sample_ready);
            }
            return std::unexpected(CaptureError::kComFailure);
        }
    }

    ComPtr<IAudioCaptureClient> capture;
    if (FAILED(client->GetService(IID_PPV_ARGS(&capture)))) {
        if (sample_ready != nullptr) {
            CloseHandle(sample_ready);
        }
        return std::unexpected(CaptureError::kComFailure);
    }

    impl_->ring = std::make_unique<RingBuffer>(ring_capacity_samples);
    impl_->sample_rate = rate;
    impl_->channels = channel_count;
    impl_->frames_captured.store(0, std::memory_order_relaxed);
    impl_->frames_silence.store(0, std::memory_order_relaxed);
    impl_->running.store(true, std::memory_order_release);

    impl_->worker = std::jthread([this, client, capture, sample_ready, format, loopback,
                                  rate, channel_count](const std::stop_token& stop) mutable {
        ComScope thread_com;
        // Ask MMCSS for audio scheduling so a busy desktop cannot starve the
        // capture loop into dropping packets.
        DWORD mmcss_index = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &mmcss_index);

        std::vector<float> scratch;
        std::vector<float> silence;
        client->Start();

        LARGE_INTEGER frequency{};
        LARGE_INTEGER started{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&started);
        std::uint64_t timeline_frames = 0;

        while (!stop.stop_requested()) {
            if (loopback) {
                Sleep(kPollIntervalMs);
            } else if (sample_ready != nullptr) {
                WaitForSingleObject(sample_ready, 200);
            }

            for (;;) {
                UINT32 packet = 0;
                if (FAILED(capture->GetNextPacketSize(&packet)) || packet == 0) {
                    break;
                }
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD packet_flags = 0;
                if (FAILED(capture->GetBuffer(&data, &frames, &packet_flags, nullptr, nullptr))) {
                    break;
                }
                const std::size_t samples =
                    static_cast<std::size_t>(frames) * channel_count;
                if ((packet_flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0) {
                    scratch.assign(samples, 0.0f);
                } else {
                    convert(data, samples, format, scratch);
                }
                impl_->ring->write(scratch);
                impl_->frames_captured.fetch_add(frames, std::memory_order_relaxed);
                timeline_frames += frames;
                capture->ReleaseBuffer(frames);
            }

            if (loopback) {
                // Nothing is playing: fill the gap so downstream sees a
                // continuous 48 kHz (or whatever the mixer runs at) timeline.
                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);
                const auto elapsed_frames = static_cast<std::uint64_t>(
                    static_cast<double>(now.QuadPart - started.QuadPart) /
                    static_cast<double>(frequency.QuadPart) * rate);
                if (elapsed_frames > timeline_frames + rate / 100) {
                    auto missing = elapsed_frames - timeline_frames;
                    missing = std::min<std::uint64_t>(missing, rate);  // cap a long stall
                    silence.assign(static_cast<std::size_t>(missing) * channel_count, 0.0f);
                    impl_->ring->write(silence);
                    impl_->frames_silence.fetch_add(missing, std::memory_order_relaxed);
                    timeline_frames += missing;
                }
            }
        }

        client->Stop();
        if (mmcss != nullptr) {
            AvRevertMmThreadCharacteristics(mmcss);
        }
        if (sample_ready != nullptr) {
            CloseHandle(sample_ready);
        }
    });

    return {};
}

}  // namespace ac3::capture
