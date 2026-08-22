#include "ac3/io/wav.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <expected>
#include <fstream>
#include <ios>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Separate translation unit from wav.cpp for the same reason
// wav_stream_writer.cpp is: the one-shot readers there hold the whole file
// in memory and parse it in one pass, while this is a stateful object
// keeping a file handle, a parse of the header, and a read position alive
// across many calls.

namespace ac3::io {

namespace {

// The fmt and data chunks must sit inside this much of the file - see the
// class comment in wav.hpp. Every WAV this project writes puts them at
// fixed offsets 12 and 36; 64 KiB leaves room for the metadata chunks other
// producers put in front.
constexpr std::size_t kHeaderWindow = 64 * 1024;

std::uint16_t read_u16(std::span<const char> data, std::size_t at) {
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[at]) |
                                      (static_cast<std::uint8_t>(data[at + 1]) << 8));
}

std::uint32_t read_u32(std::span<const char> data, std::size_t at) {
    return static_cast<std::uint32_t>(read_u16(data, at)) |
           (static_cast<std::uint32_t>(read_u16(data, at + 2)) << 16);
}

}  // namespace

struct WavStreamReader::Impl {
    std::ifstream file;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits = 0;
    bool is_float = false;
    std::uint64_t frames_total = 0;
    std::uint64_t frames_read = 0;
    // Interleaved read scratch, reused across read_planar calls - its
    // capacity settles at one call's worth and stays there.
    std::vector<char> raw;
    bool open = false;
};

WavStreamReader::WavStreamReader() : impl_(std::make_unique<Impl>()) {}

WavStreamReader::~WavStreamReader() = default;

WavStreamReader::WavStreamReader(WavStreamReader&&) noexcept = default;
WavStreamReader& WavStreamReader::operator=(WavStreamReader&&) noexcept = default;

std::expected<void, WavError> WavStreamReader::open(const std::string& path) {
    close();
    auto& im = *impl_;
    im.file.open(path, std::ios::binary);
    if (!im.file) {
        return std::unexpected(WavError::kCannotOpen);
    }

    // File size first: the data chunk's declared size is clamped to what the
    // file actually holds, exactly as read_wav does, so a truncated capture
    // still yields its real frames rather than a kTruncated refusal.
    im.file.seekg(0, std::ios::end);
    const auto end_pos = im.file.tellg();
    if (end_pos < 0) {
        return std::unexpected(WavError::kCannotOpen);
    }
    const auto file_size = static_cast<std::uint64_t>(end_pos);
    im.file.seekg(0);

    im.raw.resize(static_cast<std::size_t>(std::min<std::uint64_t>(kHeaderWindow, file_size)));
    im.file.read(im.raw.data(), static_cast<std::streamsize>(im.raw.size()));
    if (im.file.gcount() != static_cast<std::streamsize>(im.raw.size())) {
        return std::unexpected(WavError::kCannotOpen);
    }

    // The same parse as wav.cpp's parse_wav, field for field, over the
    // header window instead of the whole file.
    const std::string_view view{im.raw.data(), im.raw.size()};
    if (im.raw.size() < 44 || view.substr(0, 4) != "RIFF" || view.substr(8, 4) != "WAVE") {
        return std::unexpected(WavError::kNotRiffWave);
    }
    const auto fmt_at = view.find("fmt ");
    const auto data_at = view.find("data");
    if (fmt_at == std::string_view::npos || data_at == std::string_view::npos ||
        fmt_at + 24 > im.raw.size() || data_at + 8 > im.raw.size()) {
        return std::unexpected(WavError::kNotRiffWave);
    }

    const std::span<const char> bytes{im.raw};
    auto format = read_u16(bytes, fmt_at + 8);
    im.channels = read_u16(bytes, fmt_at + 10);
    im.sample_rate = read_u32(bytes, fmt_at + 12);
    im.bits = read_u16(bytes, fmt_at + 22);
    if (format == 0xFFFE && fmt_at + 34 <= im.raw.size()) {
        // WAVE_FORMAT_EXTENSIBLE: the real tag is the first two bytes of the
        // SubFormat GUID in the extension.
        format = read_u16(bytes, fmt_at + 32);
    }
    im.is_float = format == 3 && im.bits == 32;
    const bool is_pcm16 = format == 1 && im.bits == 16;
    if (im.channels == 0 || (!im.is_float && !is_pcm16)) {
        return std::unexpected(WavError::kUnsupportedFormat);
    }

    const auto declared = read_u32(bytes, data_at + 4);
    const std::uint64_t payload_at = data_at + 8;
    const std::uint64_t available = file_size > payload_at ? file_size - payload_at : 0;
    const std::uint64_t payload = std::min<std::uint64_t>(declared, available);
    const std::size_t stride = static_cast<std::size_t>(im.channels) * im.bits / 8;
    if (stride == 0) {
        return std::unexpected(WavError::kUnsupportedFormat);
    }
    im.frames_total = payload / stride;
    im.frames_read = 0;
    im.file.seekg(static_cast<std::streamoff>(payload_at));
    if (!im.file) {
        return std::unexpected(WavError::kCannotOpen);
    }
    im.open = true;
    return {};
}

bool WavStreamReader::is_open() const { return impl_->open; }

std::uint32_t WavStreamReader::sample_rate() const { return impl_->sample_rate; }

std::uint16_t WavStreamReader::channels() const { return impl_->channels; }

std::uint64_t WavStreamReader::frame_count() const { return impl_->frames_total; }

std::expected<std::size_t, WavError> WavStreamReader::read_planar(
    std::span<const std::span<float>> channels, std::size_t frames) {
    auto& im = *impl_;
    if (!im.open) {
        return std::unexpected(WavError::kCannotOpen);
    }
    if (channels.size() < im.channels) {
        return std::unexpected(WavError::kUnsupportedFormat);
    }
    const std::uint64_t remaining = im.frames_total - im.frames_read;
    const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(frames, remaining));
    if (n == 0) {
        return 0;
    }
    const std::size_t sample_bytes = static_cast<std::size_t>(im.bits) / 8;
    const std::size_t stride = static_cast<std::size_t>(im.channels) * sample_bytes;
    im.raw.resize(n * stride);
    im.file.read(im.raw.data(), static_cast<std::streamsize>(im.raw.size()));
    if (im.file.gcount() != static_cast<std::streamsize>(im.raw.size())) {
        // frames_total was clamped to the file's real size at open(), so a
        // short read here means the file shrank underneath us.
        return std::unexpected(WavError::kTruncated);
    }

    // The same sample conversion as wav.cpp's parse_wav, so a block-at-a-
    // time consumer reconstructs bit-identical floats to the whole-file
    // read.
    const std::span<const char> bytes{im.raw};
    for (std::size_t frame = 0; frame < n; ++frame) {
        for (std::uint16_t ch = 0; ch < im.channels; ++ch) {
            const std::size_t at = frame * stride + static_cast<std::size_t>(ch) * sample_bytes;
            if (im.is_float) {
                float value = 0.0f;
                std::memcpy(&value, im.raw.data() + at, sizeof(value));
                channels[ch][frame] = value;
            } else {
                const auto sample = static_cast<std::int16_t>(read_u16(bytes, at));
                channels[ch][frame] = static_cast<float>(sample) / 32768.0f;
            }
        }
    }
    im.frames_read += n;
    return n;
}

void WavStreamReader::close() {
    auto& im = *impl_;
    if (im.file.is_open()) {
        im.file.close();
    }
    im.open = false;
    im.frames_total = 0;
    im.frames_read = 0;
    im.channels = 0;
    im.sample_rate = 0;
}

}  // namespace ac3::io
