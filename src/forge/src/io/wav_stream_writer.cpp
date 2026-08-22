#include "ac3/io/wav.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <ios>
#include <memory>
#include <ostream>
#include <span>
#include <string>

// Separate translation unit from wav.cpp: the one-shot writers there build
// the whole file in memory before ever opening a stream, while this one is a
// stateful object that has to keep a file handle and a running frame count
// alive across many calls. Sharing a file made both concerns harder to read
// without buying anything back.

namespace ac3::io {

namespace {

void put_u16(std::ostream& out, std::uint16_t value) {
    out.write(reinterpret_cast<const char*>(&value), 2);
}

void put_u32(std::ostream& out, std::uint32_t value) {
    out.write(reinterpret_cast<const char*>(&value), 4);
}

}  // namespace

struct WavStreamWriter::Impl {
    std::fstream file;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::uint64_t frames_written = 0;
    bool open = false;
};

WavStreamWriter::WavStreamWriter() : impl_(std::make_unique<Impl>()) {}

WavStreamWriter::~WavStreamWriter() { close(); }

WavStreamWriter::WavStreamWriter(WavStreamWriter&&) noexcept = default;
WavStreamWriter& WavStreamWriter::operator=(WavStreamWriter&&) noexcept = default;

std::expected<void, WavError> WavStreamWriter::open(const std::string& path,
                                                     std::uint32_t sample_rate,
                                                     std::uint16_t channels) {
    if (channels == 0) {
        return std::unexpected(WavError::kUnsupportedFormat);
    }

    // `in | out | trunc` is not reliably create-capable for a file that does
    // not exist yet across standard library implementations (some still
    // require `in`'s target to already exist, trunc or not). So the header
    // is written first with a plain create/truncate open, which every
    // implementation agrees makes a fresh file - then that same path is
    // reopened in read+write mode for the seek-back-and-patch flush_header()
    // needs, at which point the file is guaranteed to already exist.
    {
        std::ofstream create{path, std::ios::binary | std::ios::trunc};
        if (!create) {
            return std::unexpected(WavError::kCannotOpen);
        }
        create.write("RIFF", 4);
        put_u32(create, 36);  // data_bytes = 0 until write() advances it
        create.write("WAVE", 4);
        create.write("fmt ", 4);
        put_u32(create, 16);
        put_u16(create, 3);  // IEEE float
        put_u16(create, channels);
        put_u32(create, sample_rate);
        put_u32(create, sample_rate * static_cast<std::uint32_t>(channels) * 4);
        put_u16(create, static_cast<std::uint16_t>(channels * 4));
        put_u16(create, 32);
        create.write("data", 4);
        put_u32(create, 0);
        if (!create) {
            return std::unexpected(WavError::kCannotOpen);
        }
    }

    impl_->file.open(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!impl_->file) {
        return std::unexpected(WavError::kCannotOpen);
    }
    impl_->file.seekp(0, std::ios::end);
    impl_->sample_rate = sample_rate;
    impl_->channels = channels;
    impl_->frames_written = 0;
    impl_->open = true;
    return {};
}

bool WavStreamWriter::write(std::span<const float> interleaved) {
    if (!impl_ || !impl_->open) {
        return false;
    }
    impl_->file.write(reinterpret_cast<const char*>(interleaved.data()),
                       static_cast<std::streamsize>(interleaved.size() * sizeof(float)));
    if (!impl_->file) {
        return false;
    }
    impl_->frames_written += interleaved.size() / impl_->channels;
    return true;
}

void WavStreamWriter::flush_header() {
    if (!impl_ || !impl_->open) {
        return;
    }
    const std::uint64_t data_bytes64 =
        impl_->frames_written * static_cast<std::uint64_t>(impl_->channels) * 4;
    const auto data_bytes = static_cast<std::uint32_t>(data_bytes64);
    const std::uint32_t riff_bytes = 36 + data_bytes;

    impl_->file.seekp(4, std::ios::beg);
    put_u32(impl_->file, riff_bytes);
    impl_->file.seekp(40, std::ios::beg);
    put_u32(impl_->file, data_bytes);
    // Without this, the two size fields sit in the fstream's own buffer,
    // invisible to any other handle on the same path (including a plain
    // ifstream checking on a crashed process's leftovers) until the OS
    // decides to reclaim the buffer on its own schedule.
    impl_->file.flush();
    impl_->file.seekp(0, std::ios::end);
}

void WavStreamWriter::close() {
    if (!impl_ || !impl_->open) {
        return;
    }
    flush_header();
    impl_->file.close();
    impl_->open = false;
}

bool WavStreamWriter::is_open() const { return impl_ && impl_->open; }

std::uint16_t WavStreamWriter::channels() const { return impl_ ? impl_->channels : 0; }

std::uint64_t WavStreamWriter::frames_written() const {
    return impl_ ? impl_->frames_written : 0;
}

struct WavPcm16StreamWriter::Impl {
    std::fstream file;
    std::uint64_t bytes_written = 0;
    bool open = false;
};

WavPcm16StreamWriter::WavPcm16StreamWriter() : impl_(std::make_unique<Impl>()) {}

WavPcm16StreamWriter::~WavPcm16StreamWriter() { close(); }

WavPcm16StreamWriter::WavPcm16StreamWriter(WavPcm16StreamWriter&&) noexcept = default;
WavPcm16StreamWriter& WavPcm16StreamWriter::operator=(WavPcm16StreamWriter&&) noexcept =
    default;

std::expected<void, WavError> WavPcm16StreamWriter::open(const std::string& path,
                                                          std::uint32_t sample_rate,
                                                          std::uint16_t channels) {
    if (channels == 0) {
        return std::unexpected(WavError::kUnsupportedFormat);
    }

    // Same create-then-reopen two-step as WavStreamWriter::open above, for
    // the same `in|out|trunc` portability reason.
    {
        std::ofstream create{path, std::ios::binary | std::ios::trunc};
        if (!create) {
            return std::unexpected(WavError::kCannotOpen);
        }
        // Field for field write_wav_pcm16_raw's header (wav.cpp), sizes
        // zero until flush_header()/close() patch them.
        const std::uint32_t block_align = static_cast<std::uint32_t>(channels) * 2;
        create.write("RIFF", 4);
        put_u32(create, 36);
        create.write("WAVE", 4);
        create.write("fmt ", 4);
        put_u32(create, 16);
        put_u16(create, 1);  // PCM
        put_u16(create, channels);
        put_u32(create, sample_rate);
        put_u32(create, sample_rate * block_align);
        put_u16(create, static_cast<std::uint16_t>(block_align));
        put_u16(create, 16);
        create.write("data", 4);
        put_u32(create, 0);
        if (!create) {
            return std::unexpected(WavError::kCannotOpen);
        }
    }

    impl_->file.open(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!impl_->file) {
        return std::unexpected(WavError::kCannotOpen);
    }
    impl_->file.seekp(0, std::ios::end);
    impl_->bytes_written = 0;
    impl_->open = true;
    return {};
}

bool WavPcm16StreamWriter::write(std::span<const std::byte> bytes) {
    if (!impl_ || !impl_->open) {
        return false;
    }
    impl_->file.write(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
    if (!impl_->file) {
        return false;
    }
    impl_->bytes_written += bytes.size();
    return true;
}

void WavPcm16StreamWriter::flush_header() {
    if (!impl_ || !impl_->open) {
        return;
    }
    const auto data_bytes = static_cast<std::uint32_t>(impl_->bytes_written);
    impl_->file.seekp(4, std::ios::beg);
    put_u32(impl_->file, 36 + data_bytes);
    impl_->file.seekp(40, std::ios::beg);
    put_u32(impl_->file, data_bytes);
    impl_->file.flush();
    impl_->file.seekp(0, std::ios::end);
}

void WavPcm16StreamWriter::close() {
    if (!impl_ || !impl_->open) {
        return;
    }
    flush_header();
    impl_->file.close();
    impl_->open = false;
}

bool WavPcm16StreamWriter::is_open() const { return impl_ && impl_->open; }

std::uint64_t WavPcm16StreamWriter::bytes_written() const {
    return impl_ ? impl_->bytes_written : 0;
}

}  // namespace ac3::io
