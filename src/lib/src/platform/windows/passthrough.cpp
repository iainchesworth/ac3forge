#include "ac3/sinks/passthrough.hpp"

// The Windows passthrough backend. CMake compiles this directory's
// passthrough.cpp on Windows and another platform directory's everywhere
// else, so there is no #ifdef - the file's path is what says "Windows".
//
// WIN32_LEAN_AND_MEAN and NOMINMAX are set by the WIN32 block of
// src/lib/CMakeLists.txt, not by #defines here: they configure <windows.h> for
// every translation unit that pulls it in, and one setting in one place cannot
// disagree with itself the way per-file guards can.

#include <windows.h>
// windows.h must precede the audio headers.
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

#include "ac3/capture/ring_buffer.hpp"
#include "ac3/sinks/iec61937.hpp"

namespace ac3::sinks {

namespace {

using Microsoft::WRL::ComPtr;

// PKEY_Device_FriendlyName, spelled out for the same reason as in the capture
// backend: functiondiscoverykeys_devpkey.h needs a fragile include ordering.
constexpr PROPERTYKEY kPkeyDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

// KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL from ksmedia.h:
// {00000092-0000-0010-8000-00aa00389b71}. The low 16 bits of Data1 are the
// wFormatTag (0x0092 = WAVE_FORMAT_DOLBY_AC3_SPDIF).
constexpr GUID kSubtypeIec61937DolbyDigital = {
    0x00000092, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// The class and interface identifiers, spelled out for a related reason: the
// SDK declares CLSID_MMDeviceEnumerator and the IAudio* IIDs but ships no
// import library that defines them, so the only header-only way to name them
// is __uuidof - an MSVC extension that clang rejects under -Wpedantic. The
// values are the DECLSPEC_UUID / MIDL_INTERFACE strings in mmdeviceapi.h and
// audioclient.h.
constexpr CLSID kClsidMmDeviceEnumerator = {  // {bcde0395-e52f-467c-8e3d-c4579291692e}
    0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
constexpr IID kIidMmDeviceEnumerator = {  // {a95664d2-9614-4f35-a746-de8db63617e6}
    0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
constexpr IID kIidAudioClient = {  // {1cb9ad4c-dbfa-4c32-b178-c2f568a703b2}
    0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
constexpr IID kIidAudioRenderClient = {  // {f294acfc-3146-4483-a7bf-addca7c260e2}
    0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};

// The IEC 61937 carrier is a 2-channel 16-bit stream; one AC-3 frame occupies
// one 6144-byte burst = 1536 stereo frames, matching the AC-3 frame duration.
constexpr WORD kCarrierChannels = 2;
constexpr WORD kCarrierBits = 16;
constexpr DWORD kSpeakerStereo = 0x3;  // KSAUDIO_SPEAKER_STEREO

// WAVEFORMATEXTENSIBLE_IEC61937 (ksmedia.h): a WAVEFORMATEXTENSIBLE followed
// by three fields describing the *encoded* stream inside the carrier.
struct WaveFormatIec61937 {
    WAVEFORMATEXTENSIBLE FormatExt;
    DWORD dwEncodedSamplesPerSec;
    DWORD dwEncodedChannelCount;
    DWORD dwAverageBytesPerSec;
};

WaveFormatIec61937 make_ac3_format(std::uint32_t sample_rate, DWORD encoded_channels) {
    WaveFormatIec61937 format{};
    auto& wf = format.FormatExt.Format;
    wf.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.nChannels = kCarrierChannels;
    wf.nSamplesPerSec = sample_rate;
    wf.wBitsPerSample = kCarrierBits;
    wf.nBlockAlign = static_cast<WORD>(kCarrierChannels * kCarrierBits / 8);
    wf.nAvgBytesPerSec = sample_rate * wf.nBlockAlign;
    wf.cbSize = sizeof(WaveFormatIec61937) - sizeof(WAVEFORMATEX);

    format.FormatExt.Samples.wValidBitsPerSample = kCarrierBits;
    format.FormatExt.dwChannelMask = kSpeakerStereo;
    format.FormatExt.SubFormat = kSubtypeIec61937DolbyDigital;

    // Describes the AC-3 payload, not the carrier: the decoded rate and how
    // many channels the receiver will reproduce.
    format.dwEncodedSamplesPerSec = sample_rate;
    format.dwEncodedChannelCount = encoded_channels;
    format.dwAverageBytesPerSec = 0;  // 0 is permitted and means "unspecified"
    return format;
}

std::string to_utf8(const wchar_t* wide) {
    if (wide == nullptr) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

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

std::expected<ComPtr<IMMDeviceEnumerator>, PassthroughError> make_enumerator() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(kClsidMmDeviceEnumerator, nullptr, CLSCTX_ALL,
                                kIidMmDeviceEnumerator, &enumerator))) {
        return std::unexpected(PassthroughError::kComFailure);
    }
    return enumerator;
}

}  // namespace

std::string_view describe(PassthroughError error) {
    switch (error) {
        case PassthroughError::kNoBackend: return "no passthrough backend on this platform";
        case PassthroughError::kComFailure: return "a Windows audio (WASAPI/COM) call failed";
        case PassthroughError::kDeviceNotFound: return "the requested render device was not found";
        case PassthroughError::kFormatRejected:
            return "the endpoint will not accept AC-3 over IEC 61937 (enable Dolby Digital "
                   "passthrough for the device, or use an S/PDIF or HDMI output)";
        case PassthroughError::kExclusiveUnavailable:
            return "exclusive access was refused (another app holds the device, or exclusive "
                   "mode is disabled for it in Sound settings)";
        case PassthroughError::kAlreadyRunning: return "passthrough is already running";
        case PassthroughError::kNotRunning: return "passthrough is not running";
    }
    return "unknown passthrough error";
}

std::expected<std::vector<RenderDeviceInfo>, PassthroughError> enumerate_render_devices(
    std::uint32_t sample_rate) {
    ComScope com;
    if (!com.ok()) {
        return std::unexpected(PassthroughError::kComFailure);
    }
    auto enumerator = make_enumerator();
    if (!enumerator) {
        return std::unexpected(enumerator.error());
    }

    std::string default_id;
    ComPtr<IMMDevice> default_device;
    if (SUCCEEDED((*enumerator)->GetDefaultAudioEndpoint(eRender, eConsole, &default_device))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(default_device->GetId(&id))) {
            default_id = to_utf8(id);
            CoTaskMemFree(id);
        }
    }

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED((*enumerator)->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
        return std::unexpected(PassthroughError::kComFailure);
    }
    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) {
        return std::unexpected(PassthroughError::kComFailure);
    }

    auto format = make_ac3_format(sample_rate, 6);
    std::vector<RenderDeviceInfo> devices;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) {
            continue;
        }
        RenderDeviceInfo info;
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

        ComPtr<IAudioClient> client;
        if (SUCCEEDED(device->Activate(kIidAudioClient, CLSCTX_ALL, nullptr, &client))) {
            // IsFormatSupported is the only honest way to ask "can this
            // endpoint bitstream AC-3?" - the answer depends on the driver,
            // the physical connector and the user's per-device settings.
            info.supports_ac3_passthrough =
                client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                          &format.FormatExt.Format, nullptr) == S_OK;

            // Control probe with an ordinary exclusive-mode PCM format, so a
            // "no" above can be attributed to the device rather than to
            // exclusive mode being unavailable at all.
            WAVEFORMATEX pcm{};
            pcm.wFormatTag = WAVE_FORMAT_PCM;
            pcm.nChannels = kCarrierChannels;
            pcm.nSamplesPerSec = sample_rate;
            pcm.wBitsPerSample = kCarrierBits;
            pcm.nBlockAlign = static_cast<WORD>(kCarrierChannels * kCarrierBits / 8);
            pcm.nAvgBytesPerSec = sample_rate * pcm.nBlockAlign;
            pcm.cbSize = 0;
            info.supports_exclusive_pcm =
                client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &pcm, nullptr) == S_OK;
        }
        devices.push_back(std::move(info));
    }
    return devices;
}

struct PassthroughSink::Impl {
    std::unique_ptr<capture::ByteRingBuffer> queue;
    std::jthread worker;
    std::atomic_bool running{false};
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> rendered{0};
    std::atomic<std::uint64_t> underruns{0};
};

PassthroughSink::PassthroughSink() : impl_(std::make_unique<Impl>()) {}

PassthroughSink::~PassthroughSink() {
    stop();
}

bool PassthroughSink::running() const {
    return impl_->running.load(std::memory_order_acquire);
}

PassthroughStats PassthroughSink::stats() const {
    return {.bursts_submitted = impl_->submitted.load(std::memory_order_relaxed),
            .bursts_rendered = impl_->rendered.load(std::memory_order_relaxed),
            .underruns = impl_->underruns.load(std::memory_order_relaxed)};
}

bool PassthroughSink::can_submit() const {
    if (!impl_->queue) {
        return false;
    }
    return impl_->queue->capacity() - impl_->queue->available() > iec61937::kBurstBytes;
}

bool PassthroughSink::submit(std::span<const std::byte> burst) {
    if (!running() || !impl_->queue || burst.size() != iec61937::kBurstBytes) {
        return false;
    }
    if (!can_submit()) {
        return false;
    }
    const auto wrote = impl_->queue->write(burst);
    if (wrote != burst.size()) {
        return false;
    }
    impl_->submitted.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void PassthroughSink::stop() {
    if (impl_->worker.joinable()) {
        impl_->worker.request_stop();
        impl_->worker.join();
    }
    impl_->running.store(false, std::memory_order_release);
}

std::expected<void, PassthroughError> PassthroughSink::start(const std::string& device_id,
                                                             std::uint32_t sample_rate) {
    if (running()) {
        return std::unexpected(PassthroughError::kAlreadyRunning);
    }

    ComScope com;
    if (!com.ok()) {
        return std::unexpected(PassthroughError::kComFailure);
    }
    auto enumerator = make_enumerator();
    if (!enumerator) {
        return std::unexpected(enumerator.error());
    }

    ComPtr<IMMDevice> device;
    if (device_id.empty()) {
        if (FAILED((*enumerator)->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
            return std::unexpected(PassthroughError::kDeviceNotFound);
        }
    } else {
        const int wide_len = MultiByteToWideChar(CP_UTF8, 0, device_id.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(wide_len > 0 ? wide_len - 1 : 0), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, device_id.c_str(), -1, wide.data(), wide_len);
        if (FAILED((*enumerator)->GetDevice(wide.c_str(), &device))) {
            return std::unexpected(PassthroughError::kDeviceNotFound);
        }
    }

    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(kIidAudioClient, CLSCTX_ALL, nullptr, &client))) {
        return std::unexpected(PassthroughError::kComFailure);
    }

    auto format = make_ac3_format(sample_rate, 6);
    if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &format.FormatExt.Format,
                                  nullptr) != S_OK) {
        return std::unexpected(PassthroughError::kFormatRejected);
    }

    REFERENCE_TIME default_period = 0;
    REFERENCE_TIME minimum_period = 0;
    if (FAILED(client->GetDevicePeriod(&default_period, &minimum_period))) {
        return std::unexpected(PassthroughError::kComFailure);
    }
    REFERENCE_TIME period = default_period;

    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period,
                                    &format.FormatExt.Format, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        // The documented realignment dance: ask what buffer size the driver
        // actually wants, convert it back to a period, and re-Initialize on a
        // fresh client (an initialised one cannot be reconfigured).
        UINT32 aligned_frames = 0;
        if (FAILED(client->GetBufferSize(&aligned_frames)) || aligned_frames == 0) {
            return std::unexpected(PassthroughError::kComFailure);
        }
        period = static_cast<REFERENCE_TIME>(
            10000.0 * 1000 * aligned_frames / sample_rate + 0.5);
        client.Reset();
        if (FAILED(device->Activate(kIidAudioClient, CLSCTX_ALL, nullptr, &client))) {
            return std::unexpected(PassthroughError::kComFailure);
        }
        hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                period, period, &format.FormatExt.Format, nullptr);
    }
    if (hr == AUDCLNT_E_DEVICE_IN_USE || hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) {
        return std::unexpected(PassthroughError::kExclusiveUnavailable);
    }
    if (FAILED(hr)) {
        return std::unexpected(PassthroughError::kComFailure);
    }

    UINT32 buffer_frames = 0;
    if (FAILED(client->GetBufferSize(&buffer_frames))) {
        return std::unexpected(PassthroughError::kComFailure);
    }

    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ready == nullptr || FAILED(client->SetEventHandle(ready))) {
        if (ready != nullptr) {
            CloseHandle(ready);
        }
        return std::unexpected(PassthroughError::kComFailure);
    }

    ComPtr<IAudioRenderClient> render;
    if (FAILED(client->GetService(kIidAudioRenderClient, &render))) {
        CloseHandle(ready);
        return std::unexpected(PassthroughError::kComFailure);
    }

    // Room for roughly a second of bursts, so a caller encoding slightly
    // ahead of real time never has to spin.
    impl_->queue = std::make_unique<capture::ByteRingBuffer>(iec61937::kBurstBytes * 40);
    impl_->submitted.store(0, std::memory_order_relaxed);
    impl_->rendered.store(0, std::memory_order_relaxed);
    impl_->underruns.store(0, std::memory_order_relaxed);
    impl_->running.store(true, std::memory_order_release);

    const std::size_t frame_bytes = format.FormatExt.Format.nBlockAlign;

    impl_->worker = std::jthread([this, client, render, ready, buffer_frames,
                                  frame_bytes](const std::stop_token& stop) mutable {
        ComScope thread_com;
        DWORD mmcss_index = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_index);

        std::vector<std::byte> chunk;
        client->Start();

        while (!stop.stop_requested()) {
            if (WaitForSingleObject(ready, 200) != WAIT_OBJECT_0) {
                continue;
            }
            BYTE* target = nullptr;
            if (FAILED(render->GetBuffer(buffer_frames, &target))) {
                break;
            }
            const std::size_t wanted = static_cast<std::size_t>(buffer_frames) * frame_bytes;
            chunk.resize(wanted);
            const auto got = impl_->queue->read(chunk);
            if (got < wanted) {
                // Nothing queued: emit silence for the remainder. A receiver
                // that sees a gap in the burst stream usually drops lock, so
                // this is counted, not hidden.
                std::fill(chunk.begin() + static_cast<std::ptrdiff_t>(got), chunk.end(),
                          std::byte{0});
                impl_->underruns.fetch_add(1, std::memory_order_relaxed);
            }
            std::memcpy(target, chunk.data(), wanted);
            render->ReleaseBuffer(buffer_frames, 0);
            impl_->rendered.fetch_add(got / iec61937::kBurstBytes, std::memory_order_relaxed);
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
