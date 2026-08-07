#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Minimal WAV reading and writing, shared by the CLI and the GUI so neither
// carries its own copy. Deliberately small: PCM16 and float32 only, which is
// everything this project produces or consumes.

namespace ac3::io {

enum class WavError {
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
