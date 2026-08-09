#include "ac3/sinks/monitor.hpp"

// The Windows monitor backend. CMake compiles this directory's monitor.cpp on
// Windows and another platform directory's everywhere else, so there is no
// #ifdef here - the file's path is what says "Windows".
//
// WIN32_LEAN_AND_MEAN and NOMINMAX are set by the WIN32 block of
// src/lib/CMakeLists.txt; see passthrough.cpp for why that lives there rather
// than as #defines here.

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

#include "ac3/capture/ring_buffer.hpp"

namespace ac3::sinks {

namespace {

using Microsoft::WRL::ComPtr;

// The class and interface identifiers, spelled out for the same reason as the
// capture and passthrough backends: the SDK declares these but ships no
// import library defining them, and __uuidof is an MSVC extension clang
// rejects under -Wpedantic.
constexpr CLSID kClsidMmDeviceEnumerator = {  // {bcde0395-e52f-467c-8e3d-c4579291692e}
    0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
constexpr IID kIidMmDeviceEnumerator = {  // {a95664d2-9614-4f35-a746-de8db63617e6}
    0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
constexpr IID kIidAudioClient = {  // {1cb9ad4c-dbfa-4c32-b178-c2f568a703b2}
    0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
constexpr IID kIidAudioRenderClient = {  // {f294acfc-3146-4483-a7bf-addca7c260e2}
    0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT from mmreg.h.
constexpr GUID kSubtypeIeeeFloat = {  // {00000003-0000-0010-8000-00aa00389b71}
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

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

std::expected<ComPtr<IMMDeviceEnumerator>, MonitorError> make_enumerator() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(kClsidMmDeviceEnumerator, nullptr, CLSCTX_ALL,
                                kIidMmDeviceEnumerator, &enumerator))) {
        return std::unexpected(MonitorError::kComFailure);
    }
    return enumerator;
}

// A default channel mask for a bare channel count, so a caller that does not
// know or care about speaker positions (an offline file preview, say) still
// gets a WAVEFORMATEXTENSIBLE the audio engine can place sensibly. Mirrors
// the masks KSAUDIO_SPEAKER_* would spell out for the layouts this project
// actually produces.
constexpr DWORD kSpeakerStereo = 0x3;        // FL FR
constexpr DWORD kSpeaker51 = 0x3F;           // FL FR FC LFE BL BR
constexpr DWORD kSpeaker71 = 0x63F;          // 5.1 + side left/right

DWORD default_channel_mask(std::uint16_t channels) {
    switch (channels) {
        case 1: return 0x4;  // FC
        case 2: return kSpeakerStereo;
        case 6: return kSpeaker51;
        case 8: return kSpeaker71;
        default: return 0;  // let the engine infer one
    }
}

}  // namespace

std::string_view describe(MonitorError error) {
    switch (error) {
        case MonitorError::kNoBackend: return "no monitor backend on this platform";
        case MonitorError::kComFailure: return "a Windows audio (WASAPI/COM) call failed";
        case MonitorError::kDeviceNotFound: return "the requested render device was not found";
        case MonitorError::kAlreadyRunning: return "monitor playback is already running";
        case MonitorError::kNotRunning: return "monitor playback is not running";
    }
    return "unknown monitor error";
}

struct MonitorSink::Impl {
    std::unique_ptr<capture::RingBuffer> queue;
    std::jthread worker;
    std::atomic_bool running{false};
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> rendered{0};
    std::atomic<std::uint64_t> underruns{0};
    std::uint16_t channels = 0;
};

MonitorSink::MonitorSink() : impl_(std::make_unique<Impl>()) {}

MonitorSink::~MonitorSink() {
    stop();
}

bool MonitorSink::running() const {
    return impl_->running.load(std::memory_order_acquire);
}

MonitorStats MonitorSink::stats() const {
    return {.frames_submitted = impl_->submitted.load(std::memory_order_relaxed),
            .frames_rendered = impl_->rendered.load(std::memory_order_relaxed),
            .underruns = impl_->underruns.load(std::memory_order_relaxed)};
}

bool MonitorSink::can_submit() const {
    if (!impl_->queue || impl_->channels == 0) {
        return false;
    }
    // Room for at least ~20 ms at a typical rate, in samples (interleaved).
    // A hint for callers deciding whether to spin-wait, not a correctness
    // guarantee - submit() below is what actually gates the write, because
    // this fixed threshold can be smaller than a caller's actual chunk (a
    // whole AC-3/E-AC-3 frame is ~32 ms or more).
    return impl_->queue->capacity() - impl_->queue->available() >
           static_cast<std::size_t>(impl_->channels) * 960;
}

bool MonitorSink::submit(std::span<const float> interleaved) {
    if (!running() || !impl_->queue || impl_->channels == 0 ||
        interleaved.size() % impl_->channels != 0) {
        return false;
    }
    // Checked against THIS call's actual size, not can_submit()'s generic
    // threshold: with a single producer, free space here can only grow
    // before write() runs (only the consumer thread frees space), so this
    // check-then-write cannot race into a short write. A fixed threshold
    // smaller than the chunk being pushed let write() silently perform a
    // PARTIAL write while submit() still reported failure - the caller would
    // retry the same chunk, duplicating the bytes that HAD landed and
    // desynchronising submitted/rendered accounting from what was actually
    // in the buffer.
    if (impl_->queue->capacity() - impl_->queue->available() <= interleaved.size()) {
        return false;
    }
    const auto wrote = impl_->queue->write(interleaved);
    if (wrote != interleaved.size()) {
        return false;
    }
    impl_->submitted.fetch_add(interleaved.size() / impl_->channels, std::memory_order_relaxed);
    return true;
}

void MonitorSink::stop() {
    if (impl_->worker.joinable()) {
        impl_->worker.request_stop();
        impl_->worker.join();
    }
    impl_->running.store(false, std::memory_order_release);
}

std::expected<void, MonitorError> MonitorSink::start(const std::string& device_id,
                                                      std::uint32_t sample_rate,
                                                      std::uint16_t channels,
                                                      std::uint32_t channel_mask) {
    if (running()) {
        return std::unexpected(MonitorError::kAlreadyRunning);
    }
    if (channels == 0) {
        return std::unexpected(MonitorError::kComFailure);
    }

    ComScope com;
    if (!com.ok()) {
        return std::unexpected(MonitorError::kComFailure);
    }
    auto enumerator = make_enumerator();
    if (!enumerator) {
        return std::unexpected(enumerator.error());
    }

    ComPtr<IMMDevice> device;
    if (device_id.empty()) {
        if (FAILED((*enumerator)->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
            return std::unexpected(MonitorError::kDeviceNotFound);
        }
    } else {
        const int wide_len = MultiByteToWideChar(CP_UTF8, 0, device_id.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(wide_len > 0 ? wide_len - 1 : 0), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, device_id.c_str(), -1, wide.data(), wide_len);
        if (FAILED((*enumerator)->GetDevice(wide.c_str(), &device))) {
            return std::unexpected(MonitorError::kDeviceNotFound);
        }
    }

    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(kIidAudioClient, CLSCTX_ALL, nullptr, &client))) {
        return std::unexpected(MonitorError::kComFailure);
    }

    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = channels;
    format.Format.nSamplesPerSec = sample_rate;
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = static_cast<WORD>(channels * sizeof(float));
    format.Format.nAvgBytesPerSec = sample_rate * format.Format.nBlockAlign;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = channel_mask != 0 ? channel_mask : default_channel_mask(channels);
    format.SubFormat = kSubtypeIeeeFloat;

    // Shared mode's audio engine carries its own sample-rate/channel-matrix
    // converter (unlike exclusive mode's IsFormatSupported, which is a strict
    // yes/no), so Initialize's own result is the authoritative gate here
    // rather than a separate pre-check: most devices accept an explicit
    // float32 format at any reasonable rate/channel count and the engine
    // adapts it to the mix format transparently.
    REFERENCE_TIME default_period = 0;
    REFERENCE_TIME minimum_period = 0;
    if (FAILED(client->GetDevicePeriod(&default_period, &minimum_period))) {
        return std::unexpected(MonitorError::kComFailure);
    }

    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                    default_period, 0, &format.Format, nullptr);
    if (FAILED(hr)) {
        return std::unexpected(MonitorError::kComFailure);
    }

    UINT32 buffer_frames = 0;
    if (FAILED(client->GetBufferSize(&buffer_frames))) {
        return std::unexpected(MonitorError::kComFailure);
    }

    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ready == nullptr || FAILED(client->SetEventHandle(ready))) {
        if (ready != nullptr) {
            CloseHandle(ready);
        }
        return std::unexpected(MonitorError::kComFailure);
    }

    ComPtr<IAudioRenderClient> render;
    if (FAILED(client->GetService(kIidAudioRenderClient, &render))) {
        CloseHandle(ready);
        return std::unexpected(MonitorError::kComFailure);
    }

    // Room for roughly a second of samples, so a caller decoding slightly
    // ahead of real time never has to spin.
    impl_->queue =
        std::make_unique<capture::RingBuffer>(static_cast<std::size_t>(channels) * sample_rate);
    impl_->channels = channels;
    impl_->submitted.store(0, std::memory_order_relaxed);
    impl_->rendered.store(0, std::memory_order_relaxed);
    impl_->underruns.store(0, std::memory_order_relaxed);
    impl_->running.store(true, std::memory_order_release);

    impl_->worker = std::jthread([this, client, render, ready, buffer_frames,
                                  channels](const std::stop_token& stop) mutable {
        ComScope thread_com;
        DWORD mmcss_index = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &mmcss_index);

        std::vector<float> chunk;
        client->Start();

        while (!stop.stop_requested()) {
            if (WaitForSingleObject(ready, 200) != WAIT_OBJECT_0) {
                continue;
            }
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) {
                break;
            }
            const UINT32 wanted_frames = buffer_frames - padding;
            if (wanted_frames == 0) {
                continue;
            }
            BYTE* target = nullptr;
            if (FAILED(render->GetBuffer(wanted_frames, &target))) {
                break;
            }
            const std::size_t wanted_samples = static_cast<std::size_t>(wanted_frames) * channels;
            chunk.resize(wanted_samples);
            const auto got = impl_->queue->read(chunk);
            if (got < wanted_samples) {
                // Nothing queued: emit silence for the remainder, counted
                // rather than hidden, matching PassthroughSink's discipline.
                std::fill(chunk.begin() + static_cast<std::ptrdiff_t>(got), chunk.end(), 0.0f);
                impl_->underruns.fetch_add(1, std::memory_order_relaxed);
            }
            std::memcpy(target, chunk.data(), wanted_samples * sizeof(float));
            render->ReleaseBuffer(wanted_frames, 0);
            impl_->rendered.fetch_add(got / channels, std::memory_order_relaxed);
        }

        client->Stop();
        if (mmcss != nullptr) {
            AvRevertMmThreadCharacteristics(mmcss);
        }
        CloseHandle(ready);
    });

    return {};
}

}  // namespace ac3::sinks
