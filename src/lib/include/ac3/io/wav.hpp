#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/core/tables.hpp"

// Minimal WAV reading and writing, shared by the CLI and the GUI so neither
// carries its own copy. Deliberately small: PCM16 and float32 only, which is
// everything this project produces or consumes.

namespace ac3::io {

enum class WavError : std::uint8_t {
    kCannotOpen,
    kNotRiffWave,
    kUnsupportedFormat,  // not PCM16 / float32
    kTruncated,
};

[[nodiscard]] std::string_view describe(WavError error);

struct WavData {
    std::uint32_t sample_rate = 0;
    // One vector per channel, samples normalized to [-1, 1).
    std::vector<std::vector<float>> channels;

    [[nodiscard]] std::size_t frame_count() const {
        return channels.empty() ? 0 : channels.front().size();
    }
};

[[nodiscard]] std::expected<WavData, WavError> read_wav(const std::string& path);

// A WAV file's channel order (the WAVE_FORMAT_EXTENSIBLE convention: FL, FR,
// FC, LFE, BL, BR) is not A/52 Table 5.8's (L, C, R, SL, SR, LFE), so the two
// have to be reconciled before any multichannel file reaches the encoder.
struct Ac3Layout {
    Acmod acmod = Acmod::k2_0;
    bool lfe = false;
    // wav_index[k] is the position in a WAV frame of AC-3 channel k.
    std::vector<std::size_t> wav_index;
};

// The AC-3 layout that carries a WAV of this width, or nothing when no legal
// acmod does (7 channels and up, or none at all).
[[nodiscard]] std::optional<Ac3Layout> ac3_layout_for(std::size_t wav_channels);

// The inverse permutation, in the form write_wav_f32 takes: entry i names the
// AC-3 channel that belongs at WAV position i.
[[nodiscard]] std::vector<std::size_t> wav_channel_order(Acmod acmod, bool lfe);

// Float32 WAV (format tag 3), channels interleaved in the given order.
[[nodiscard]] std::expected<void, WavError> write_wav_f32(
    const std::string& path, std::span<const std::vector<float>> channels,
    std::uint32_t sample_rate, std::span<const std::size_t> channel_order = {});

// PCM16 WAV wrapping already-formed little-endian 16-bit payload bytes. Used
// for the IEC 61937 burst carrier, where the payload must pass through
// untouched.
[[nodiscard]] std::expected<void, WavError> write_wav_pcm16_raw(
    const std::string& path, std::span<const std::byte> payload, std::uint32_t sample_rate,
    std::uint16_t channels);

}  // namespace ac3::io
