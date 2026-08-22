#pragma once

#include <alsa/asoundlib.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>

// The handful of things capture.cpp, monitor.cpp and passthrough.cpp all need
// from libasound, kept in one place so the three cannot drift.
//
// Nothing here is a wrapper for its own sake: alsa-lib's C API is used
// directly everywhere else in this directory. What it does cover is the habits
// that are easy to get wrong once and then repeat - the snd_*_malloc/free
// pairs (which leak the moment an early return is added between them), the
// card/device walk (which has an unobvious termination condition) and the
// sample-format preference order (which two backends would otherwise each
// carry a copy of).

namespace ac3::alsa {

// An alsa-lib info struct owned by scope.
//
// The API allocates these through per-type malloc/free pairs; there is an
// alloca-based macro form too, but it hides a variable-length array behind a
// macro and makes an early return in the middle of a negotiation sequence a
// stack lifetime question. This is the boring version.
template <typename T, int (*Allocate)(T**), void (*Release)(T*)>
class Owned {
public:
    Owned() {
        if (Allocate(&pointer_) < 0) {
            pointer_ = nullptr;
        }
    }
    ~Owned() {
        if (pointer_ != nullptr) {
            Release(pointer_);
        }
    }
    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;

    [[nodiscard]] T* get() const { return pointer_; }
    explicit operator bool() const { return pointer_ != nullptr; }

private:
    T* pointer_ = nullptr;
};

using HwParams = Owned<snd_pcm_hw_params_t, snd_pcm_hw_params_malloc, snd_pcm_hw_params_free>;
using SwParams = Owned<snd_pcm_sw_params_t, snd_pcm_sw_params_malloc, snd_pcm_sw_params_free>;
using CardInfo = Owned<snd_ctl_card_info_t, snd_ctl_card_info_malloc, snd_ctl_card_info_free>;
using PcmInfo = Owned<snd_pcm_info_t, snd_pcm_info_malloc, snd_pcm_info_free>;

// A PCM handle owned by scope, so a failed negotiation cannot leak a device
// that is then unopenable until the process exits.
class Pcm {
public:
    Pcm() = default;
    explicit Pcm(snd_pcm_t* handle) : handle_(handle) {}
    ~Pcm() { close(); }
    Pcm(const Pcm&) = delete;
    Pcm& operator=(const Pcm&) = delete;
    Pcm(Pcm&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    Pcm& operator=(Pcm&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] snd_pcm_t* get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

    // Hands the handle to a thread that will outlive this object.
    [[nodiscard]] snd_pcm_t* release() { return std::exchange(handle_, nullptr); }

    void close() {
        if (handle_ != nullptr) {
            snd_pcm_close(handle_);
            handle_ = nullptr;
        }
    }

private:
    snd_pcm_t* handle_ = nullptr;
};

// Silences alsa-lib's own diagnostics for the duration of a scope.
//
// alsa-lib's default error handler prints to stderr. That is reasonable for a
// failure the user is waiting to hear about, and wrong for the failures this
// backend goes looking for on purpose: enumeration probes every digital output
// by trying to open it, so a machine with four HDMI outputs and one display
// attached would print three alarming-looking "Unknown PCM" complaints every
// time somebody ran 'ac3cli outputs'. A library has no business writing to a
// caller's stderr about something it expected and handled.
//
// Deliberately NOT applied to the open in PassthroughSink::start(): that one
// is a failure the caller wanted, and alsa-lib's line is more specific about
// the device name than any error code can be.
//
// The handler is process-global, so this is only sound while one thread is
// enumerating - which is the only way enumeration is ever called.
class QuietErrors {
public:
    QuietErrors() : saved_(snd_lib_error) { snd_lib_error_set_handler(&silent); }
    ~QuietErrors() { snd_lib_error_set_handler(saved_); }
    QuietErrors(const QuietErrors&) = delete;
    QuietErrors& operator=(const QuietErrors&) = delete;

private:
    static void silent(const char*, int, const char*, int, const char*, ...) {}

    snd_lib_error_handler_t saved_;
};

// ---------------------------------------------------------------------------
// Sample formats
//
// ALSA converts nothing on a raw hw: device - it offers what the hardware can
// do and the application deals with it - so a backend that reads or writes
// float has to be able to meet the device where it is. These four cover every
// consumer sound card; anything exotic enough to offer none of them is out of
// scope, and negotiation fails rather than guessing.
// ---------------------------------------------------------------------------
enum class SampleFormat { kFloat32, kPcm16, kPcm24Packed, kPcm32 };

struct FormatChoice {
    snd_pcm_format_t alsa = SND_PCM_FORMAT_FLOAT_LE;
    SampleFormat kind = SampleFormat::kFloat32;
    std::size_t bytes = 4;
};

// Preference order: float first because it needs no conversion at all, then
// widest integer first so a device offering several loses the least.
inline constexpr std::array<FormatChoice, 4> kFormats{{
    {SND_PCM_FORMAT_FLOAT_LE, SampleFormat::kFloat32, 4},
    {SND_PCM_FORMAT_S32_LE, SampleFormat::kPcm32, 4},
    {SND_PCM_FORMAT_S24_3LE, SampleFormat::kPcm24Packed, 3},
    {SND_PCM_FORMAT_S16_LE, SampleFormat::kPcm16, 2},
}};

// Install the first format in that order the device accepts, into `params`.
[[nodiscard]] inline std::optional<FormatChoice> choose_format(snd_pcm_t* pcm,
                                                               snd_pcm_hw_params_t* params) {
    for (const auto& candidate : kFormats) {
        if (snd_pcm_hw_params_set_format(pcm, params, candidate.alsa) >= 0) {
            return candidate;
        }
    }
    return std::nullopt;
}

// One PCM of one sound card, as the walk below reports it.
struct PcmEntry {
    int card = 0;             // ALSA card index
    std::string card_id;      // the CARD= name, e.g. "PCH" or "HDMI"
    std::string card_name;    // human-readable, e.g. "HDA Intel PCH"
    int device = 0;           // hardware device index within the card
    std::string device_name;  // human-readable, e.g. "ALC295 Analog", "HDMI 0"
};

// Visit every `stream`-direction PCM of every sound card, in card then device
// order.
//
// A machine with no sound card - a container, a CI runner, WSL - has no cards
// to walk, so `visit` is simply never called and every caller's list comes
// back empty. That is the whole of this backend's headless behaviour: not a
// special case, just an empty loop.
template <typename Visitor>
void for_each_pcm(snd_pcm_stream_t stream, Visitor&& visit) {
    const QuietErrors quiet;
    int card = -1;
    // snd_card_next() writes -1 into `card` once it runs out, which is the
    // termination condition; a zero return only means the query itself worked.
    while (snd_card_next(&card) == 0 && card >= 0) {
        const std::string control_name = std::format("hw:{}", card);
        snd_ctl_t* control = nullptr;
        if (snd_ctl_open(&control, control_name.c_str(), 0) < 0) {
            continue;
        }

        CardInfo card_info;
        if (card_info && snd_ctl_card_info(control, card_info.get()) >= 0) {
            const char* card_id = snd_ctl_card_info_get_id(card_info.get());
            const char* card_name = snd_ctl_card_info_get_name(card_info.get());

            int device = -1;
            while (snd_ctl_pcm_next_device(control, &device) == 0 && device >= 0) {
                PcmInfo pcm_info;
                if (!pcm_info) {
                    break;
                }
                snd_pcm_info_set_device(pcm_info.get(), static_cast<unsigned>(device));
                snd_pcm_info_set_subdevice(pcm_info.get(), 0);
                snd_pcm_info_set_stream(pcm_info.get(), stream);
                // Fails for a device that has no PCM in this direction - a
                // playback-only HDMI output asked about capture, say - which
                // is how the walk filters by direction.
                if (snd_ctl_pcm_info(control, pcm_info.get()) < 0) {
                    continue;
                }
                const char* device_name = snd_pcm_info_get_name(pcm_info.get());
                visit(PcmEntry{
                    .card = card,
                    .card_id = card_id != nullptr ? card_id : "",
                    .card_name = card_name != nullptr ? card_name : "",
                    .device = device,
                    .device_name = device_name != nullptr ? device_name : "",
                });
            }
        }
        snd_ctl_close(control);
    }
}

// The card alsa-lib would use for an unqualified device name, or -1 when the
// configuration does not say. Only used to decide which enumerated endpoint to
// flag as the default one.
[[nodiscard]] inline int default_card() {
    snd_config_t* root = snd_config;
    if (root == nullptr) {
        // No configuration has been loaded in this process yet.
        if (snd_config_update() < 0 || snd_config == nullptr) {
            return -1;
        }
        root = snd_config;
    }
    snd_config_t* node = nullptr;
    if (snd_config_search(root, "defaults.pcm.card", &node) < 0) {
        return -1;
    }
    long value = 0;
    if (snd_config_get_integer(node, &value) < 0) {
        return -1;
    }
    return static_cast<int>(value);
}

}  // namespace ac3::alsa
