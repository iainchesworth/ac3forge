#pragma once

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include "ac3/audio/passthrough.hpp"

// Everything the ALSA backend does with device names and IEC 60958 channel
// status, expressed without touching libasound.
//
// It lives in its own header for one reason: it is the part of the backend
// that can be tested on a machine with no sound card, no ALSA configuration
// and - since nothing here includes <alsa/asoundlib.h> - no libasound at all.
// tests/backend/alsa/test_alsa_device_names.cpp covers it, and CMake adds
// that file to the suite in the same breath as it selects this directory.
//
// The subject is the one thing ALSA does differently from WASAPI. WASAPI has a
// non-PCM *format* (KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL) that a driver
// either accepts or refuses. ALSA has no such format: an IEC 61937 burst
// stream is opened as ordinary 16-bit stereo PCM, and what tells the receiver
// on the far end of the cable that these bytes are a Dolby Digital bitstream
// rather than music is the *channel status* carried alongside the samples -
// specifically the non-audio bit, AES0 bit 1 of IEC 60958. Getting that bit
// wrong does not fail; it plays the bitstream as noise at full scale.
//
// alsa-lib sets those bits from arguments on the device name, which is why
// this file is about strings: "iec958:CARD=PCH,DEV=0" is a device, and
// "iec958:CARD=PCH,DEV=0,AES0=0x06,AES1=0x82,AES2=0x00,AES3=0x02" is that
// device carrying a non-audio 48 kHz stream.

namespace ac3::alsa {

// The four consumer-format channel-status bytes alsa-lib takes as device-name
// arguments. Named AES0..AES3 after the IEC 60958 / AES3 subframe bytes they
// populate, which is also what alsa-lib calls them.
struct ChannelStatus {
    std::uint8_t aes0 = 0;
    std::uint8_t aes1 = 0;
    std::uint8_t aes2 = 0;
    std::uint8_t aes3 = 0;

    friend constexpr bool operator==(const ChannelStatus&, const ChannelStatus&) = default;
};

// The rate the S/PDIF or HDMI link itself runs at to carry `content_rate` of
// `format`.
//
// The same for AC-3 and different for E-AC-3, which is the detail that catches
// people: a Dolby Digital Plus burst is four times the size of an AC-3 one
// (ac3::iec61937::kEac3BurstBytes) and covers the same span of time, so the
// link has to clock four times as fast to deliver it. Microsoft's
// "Representing Formats for IEC 61937 Transmissions" states it as a
// requirement; the Windows backend applies it by building a 4x
// WAVEFORMATEXTENSIBLE, and this backend by opening the device at 4x.
[[nodiscard]] constexpr std::uint32_t carrier_rate(audio::BitstreamFormat format,
                                                   std::uint32_t content_rate) {
    return format == audio::BitstreamFormat::kEac3 ? content_rate * 4 : content_rate;
}

// Channel status for an IEC 61937 burst stream on a link running at
// `carrier_rate` - the CARRIER, not the content rate, since these bytes
// describe the wire.
//
// The constants are the IEC958_AES* macros of <alsa/asoundef.h>, written out
// rather than included so this header stays free of libasound - and commented
// with the macro each value comes from so the two can be checked against each
// other by eye.
//
// nullopt for a rate IEC 60958's consumer format has no frequency code for,
// because there is then no honest channel status to send. That rules out one
// combination the rest of this library will happily produce: E-AC-3 content at
// 32 kHz wants a 128 kHz carrier, and there is no code for 128 kHz. The sink
// refuses it rather than announcing some other rate.
[[nodiscard]] constexpr std::optional<ChannelStatus> non_audio_channel_status(
    std::uint32_t carrier) {
    std::uint8_t frequency = 0;
    switch (carrier) {
        case 44100: frequency = 0x00; break;   // IEC958_AES3_CON_FS_44100
        case 48000: frequency = 0x02; break;   // IEC958_AES3_CON_FS_48000
        case 32000: frequency = 0x03; break;   // IEC958_AES3_CON_FS_32000
        case 176400: frequency = 0x0C; break;  // IEC958_AES3_CON_FS_176400 (4x 44.1k)
        case 192000: frequency = 0x0E; break;  // IEC958_AES3_CON_FS_192000 (4x 48k)
        default: return std::nullopt;
    }
    return ChannelStatus{
        // IEC958_AES0_NONAUDIO (1<<1) - the bit this whole file exists for -
        // plus IEC958_AES0_CON_NOT_COPYRIGHT (1<<2). Bit 0 clear selects the
        // consumer format, bits 3..5 clear select no pre-emphasis.
        .aes0 = 0x06,
        // IEC958_AES1_CON_ORIGINAL (1<<7) | IEC958_AES1_CON_PCM_CODER (0x02).
        // "PCM coder" is the category code, not a claim about the payload: the
        // non-audio bit above is what says the payload is not PCM. This pair
        // is also alsa-lib's own default for the iec958 device.
        .aes1 = 0x82,
        // Source and channel number unspecified.
        .aes2 = 0x00,
        .aes3 = frequency,
    };
}

// Which alsa-lib device names take AES arguments: the `iec958` and `hdmi`
// configuration plugins, and nothing else. A raw `hw:` or `plughw:` device
// takes CARD/DEV/SUBDEV and would reject a fifth argument outright.
[[nodiscard]] constexpr bool takes_channel_status_args(std::string_view name) {
    return name == "iec958" || name.starts_with("iec958:") ||  //
           name == "hdmi" || name.starts_with("hdmi:");
}

// True when `name` already carries channel status of its own.
[[nodiscard]] constexpr bool has_channel_status_args(std::string_view name) {
    return name.find("AES0=") != std::string_view::npos;
}

// The name to hand snd_pcm_open() to bitstream through `base` on a link
// running at `carrier`.
//
// Three cases, in the order they are tested:
//
//   * `base` already names AES0 - a caller who wrote the channel status by
//     hand gets it back untouched. That is the escape hatch for a device this
//     backend's enumeration never offered, and it is deliberately unvalidated.
//   * `base` is an iec958/hdmi device - the four arguments are appended.
//   * anything else - returned unchanged, because appending would produce a
//     name alsa-lib will not parse. A raw hw: device CAN carry a bitstream,
//     but only if the channel status is set on the card's "IEC958 Playback
//     Default" control instead, which is the caller's business, not ours.
//
// nullopt when the carrier rate has no consumer frequency code AND the name is
// one that would have carried it - never for a name the rate cannot reach.
[[nodiscard]] inline std::optional<std::string> passthrough_device_name(std::string_view base,
                                                                        std::uint32_t carrier) {
    if (base.empty()) {
        return std::nullopt;
    }
    if (has_channel_status_args(base) || !takes_channel_status_args(base)) {
        return std::string{base};
    }
    const auto status = non_audio_channel_status(carrier);
    if (!status) {
        return std::nullopt;
    }
    return std::format("{},AES0=0x{:02x},AES1=0x{:02x},AES2=0x{:02x},AES3=0x{:02x}", base,
                       status->aes0, status->aes1, status->aes2, status->aes3);
}

// What kind of digital output a card's PCM is, judged by the name the driver
// gives it - "HDMI 0", "IEC958", "SPDIF", "Digital".
//
// A heuristic, and the only one available: ALSA exposes no "this connector is
// optical" property, so every desktop audio stack classifies these by name.
// Nothing depends on the guess being right, because a candidate built from it
// is then probed by opening it (see passthrough.cpp) - a misclassified PCM
// fails the probe and is reported as not passthrough-capable, which is what it
// would have been reported as anyway.
enum class DigitalOutput {
    kNone,    // an analog or otherwise non-bitstreaming PCM
    kHdmi,    // reached through the `hdmi` configuration plugin
    kSpdif,   // reached through the `iec958` plugin: optical or coaxial
};

namespace detail {

// Case-folds once and matches the same markers `classify_digital_output`
// documents; factored out so it can be tried against more than one string.
[[nodiscard]] inline DigitalOutput classify_one(std::string_view name) {
    std::string folded;
    folded.reserve(name.size());
    for (const char c : name) {
        folded.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
    // HDMI first: an HDMI PCM is usually also called a digital output, and it
    // is the more specific answer.
    if (folded.find("hdmi") != std::string::npos || folded.find("displayport") != std::string::npos) {
        return DigitalOutput::kHdmi;
    }
    for (const std::string_view marker : {"iec958", "spdif", "s/pdif", "digital"}) {
        if (folded.find(marker) != std::string::npos) {
            return DigitalOutput::kSpdif;
        }
    }
    return DigitalOutput::kNone;
}

}  // namespace detail

[[nodiscard]] inline DigitalOutput classify_digital_output(std::string_view pcm_name,
                                                             std::string_view card_id = {},
                                                             std::string_view card_name = {}) {
    if (const auto kind = detail::classify_one(pcm_name); kind != DigitalOutput::kNone) {
        return kind;
    }
    // Some drivers name the PCM itself generically and put the only marker on
    // the card instead: vc4-hdmi (Raspberry Pi's HDMI output) calls every PCM
    // "MAI PCM i2s-hifi-0" - "hdmi" only ever appears in the card's own id
    // ("vc4hdmi0") and name ("vc4-hdmi-0"). Falling back to those costs
    // nothing here: a wrong guess is still caught by the open+probe that
    // follows, per the type's own doc comment above.
    if (const auto kind = detail::classify_one(card_id); kind != DigitalOutput::kNone) {
        return kind;
    }
    return detail::classify_one(card_name);
}

// The alsa-lib device name for the `index`-th digital output of `card_id`.
//
// `index` counts that card's HDMI (or S/PDIF) PCMs in hardware-device order,
// which is how the shipped card configurations are written: HDA-Intel.conf
// defines hdmi.0, hdmi.1, ... in ascending device order, and iec958.0 for the
// card's single S/PDIF. It is a mapping, not a hardware device number - the
// hdmi.0 of an HDA card is usually hw device 3 - so it must not be confused
// with the number `hw:` would take.
[[nodiscard]] inline std::string config_device_name(DigitalOutput kind, std::string_view card_id,
                                                    unsigned index) {
    const std::string_view plugin = kind == DigitalOutput::kHdmi ? "hdmi" : "iec958";
    return std::format("{}:CARD={},DEV={}", plugin, card_id, index);
}

// The raw hardware device behind a card/device pair, bypassing every plugin.
// Used only as the control probe that separates "this output cannot
// bitstream" from "this output cannot be opened at all".
[[nodiscard]] inline std::string hw_device_name(std::string_view card_id, int device) {
    return std::format("hw:CARD={},DEV={}", card_id, device);
}

}  // namespace ac3::alsa
