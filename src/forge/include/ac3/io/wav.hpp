#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <iosfwd>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/export.hpp"

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

[[nodiscard]] AC3FORGE_EXPORT std::string_view describe(WavError error);

struct WavData {
    std::uint32_t sample_rate = 0;
    // One vector per channel, samples normalized to [-1, 1).
    std::vector<std::vector<float>> channels;

    [[nodiscard]] std::size_t frame_count() const {
        return channels.empty() ? 0 : channels.front().size();
    }
};

[[nodiscard]] AC3FORGE_EXPORT std::expected<WavData, WavError> read_wav(const std::string& path);

// Same parse, from an already-open stream rather than a path - e.g. stdin,
// for a caller that has put it into binary mode itself (see ac3cli's "-"
// convention for stdin/stdout in place of a file argument). Both overloads
// read their whole source into memory before parsing anything, so neither
// one needs its stream to be seekable.
[[nodiscard]] AC3FORGE_EXPORT std::expected<WavData, WavError> read_wav(std::istream& in);

// The integer-exact view: PCM16 or PCM24 sample words, sign-extended into
// int32 without any scaling, one vector per channel in file order. This
// exists for the lossless TrueHD (MLP) path, where "lossless" means the
// decoder must reproduce the sample words bit for bit - a round trip
// through [-1, 1) floats is where that guarantee would quietly die.
struct WavPcmData {
    std::uint32_t sample_rate = 0;
    int bits = 0;  // 16 or 24, as stored in the file
    std::vector<std::vector<std::int32_t>> channels;

    [[nodiscard]] std::size_t frame_count() const {
        return channels.empty() ? 0 : channels.front().size();
    }
};

[[nodiscard]] AC3FORGE_EXPORT std::expected<WavPcmData, WavError> read_wav_pcm(
    const std::string& path);

// Integer PCM WAV (format tag 1), 16 or 24 bits, channels interleaved in
// stored order. Each sample must already fit the chosen width; values are
// written verbatim, never rescaled.
[[nodiscard]] AC3FORGE_EXPORT std::expected<void, WavError> write_wav_pcm(
    const std::string& path, std::span<const std::vector<std::int32_t>> channels,
    std::uint32_t sample_rate, int bits);

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
[[nodiscard]] AC3FORGE_EXPORT std::optional<Ac3Layout> ac3_layout_for(std::size_t wav_channels);

// The inverse permutation, in the form write_wav_f32 takes: entry i names the
// AC-3 channel that belongs at WAV position i.
[[nodiscard]] AC3FORGE_EXPORT std::vector<std::size_t> wav_channel_order(Acmod acmod, bool lfe);

// Float32 WAV (format tag 3), channels interleaved in the given order.
[[nodiscard]] AC3FORGE_EXPORT std::expected<void, WavError> write_wav_f32(
    const std::string& path, std::span<const std::vector<float>> channels,
    std::uint32_t sample_rate, std::span<const std::size_t> channel_order = {});

// Same write, to an already-open stream rather than a path - e.g. stdout for
// ac3cli's "-" output convention. `channels` already carries every sample,
// so the RIFF/data chunk sizes are known before the first byte goes out:
// this writes strictly forward, once, and never seeks back to patch a
// header - it works the same on a plain file and on an unseekable pipe.
[[nodiscard]] AC3FORGE_EXPORT std::expected<void, WavError> write_wav_f32(
    std::ostream& out, std::span<const std::vector<float>> channels, std::uint32_t sample_rate,
    std::span<const std::size_t> channel_order = {});

// PCM16 WAV wrapping already-formed little-endian 16-bit payload bytes. Used
// for the IEC 61937 burst carrier, where the payload must pass through
// untouched.
[[nodiscard]] AC3FORGE_EXPORT std::expected<void, WavError> write_wav_pcm16_raw(
    const std::string& path, std::span<const std::byte> payload, std::uint32_t sample_rate,
    std::uint16_t channels);

// Incremental float32 WAV writer for takes too long to hold in memory (a
// live capture session can run for an hour or more). Opens the file once,
// takes interleaved samples as they arrive, and finalizes the RIFF/data
// chunk sizes on close() - see flush_header()'s own comment for what
// happens if the process never reaches close() at all.
class AC3FORGE_EXPORT WavStreamWriter {
   public:
    WavStreamWriter();
    ~WavStreamWriter();  // closes if still open, same as an fstream would
    WavStreamWriter(const WavStreamWriter&) = delete;
    WavStreamWriter& operator=(const WavStreamWriter&) = delete;
    WavStreamWriter(WavStreamWriter&&) noexcept;
    WavStreamWriter& operator=(WavStreamWriter&&) noexcept;

    // Opens `path` and writes a float32 (format tag 3) WAV header for
    // `channels` channels at `sample_rate`, sized for zero frames pending
    // write()/close(). Refuses (kCannotOpen) if the file cannot be created,
    // (kUnsupportedFormat) if channels is 0.
    [[nodiscard]] std::expected<void, WavError> open(const std::string& path,
                                                       std::uint32_t sample_rate,
                                                       std::uint16_t channels);

    // Appends interleaved float samples - a multiple of channels() long, in
    // the caller's own channel order (this writer does not permute; a live
    // capture's raw device order is exactly what a safety copy should keep).
    // Returns false (and leaves the writer open but stalled) if the
    // underlying write fails, e.g. the disk fills - the caller decides
    // whether that is fatal to the whole session.
    [[nodiscard]] bool write(std::span<const float> interleaved);

    // Rewrites just the RIFF and data chunk size fields to match what has
    // actually been written so far, then seeks back to the write position -
    // does NOT close the file. Call this periodically during a long write
    // (every second or so is plenty). Without it, a process kill mid-session
    // leaves a WAV whose header still claims zero data bytes even though the
    // file holds real audio - most readers trust the header's data size over
    // the file's actual length, so an unpatched header would make a real
    // partial take LOOK empty. Calling this regularly means the worst a hard
    // crash can do is undersell the last fraction of a second.
    void flush_header();

    // Finalizes the header (same as flush_header()) and closes the file.
    // Safe to call when not open, and safe to call more than once.
    void close();

    [[nodiscard]] bool is_open() const;
    [[nodiscard]] std::uint16_t channels() const;
    [[nodiscard]] std::uint64_t frames_written() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// WavStreamWriter's PCM16-raw sibling: write_wav_pcm16_raw() for a payload
// whose length is not known up front - a live session's IEC 61937 burst
// carrier, where the last burst only exists once the last access unit has
// been captured. The header is written field for field as
// write_wav_pcm16_raw's (format tag 1, 16-bit), sizes patched by
// flush_header()/close() exactly as WavStreamWriter patches its own - so
// after close(), the file is byte-identical to what write_wav_pcm16_raw
// would have produced for the same payload. Bytes pass through untouched;
// the caller owns their little-endian PCM16 framing, same as the one-shot.
class AC3FORGE_EXPORT WavPcm16StreamWriter {
   public:
    WavPcm16StreamWriter();
    ~WavPcm16StreamWriter();  // closes if still open, same as an fstream would
    WavPcm16StreamWriter(const WavPcm16StreamWriter&) = delete;
    WavPcm16StreamWriter& operator=(const WavPcm16StreamWriter&) = delete;
    WavPcm16StreamWriter(WavPcm16StreamWriter&&) noexcept;
    WavPcm16StreamWriter& operator=(WavPcm16StreamWriter&&) noexcept;

    [[nodiscard]] std::expected<void, WavError> open(const std::string& path,
                                                       std::uint32_t sample_rate,
                                                       std::uint16_t channels);

    // Appends raw payload bytes. Returns false (writer open but stalled) if
    // the underlying write fails - same contract as WavStreamWriter::write.
    [[nodiscard]] bool write(std::span<const std::byte> bytes);

    // Same periodic-patch rationale as WavStreamWriter::flush_header - see
    // its comment; a long take should call this every second or so.
    void flush_header();

    void close();

    [[nodiscard]] bool is_open() const;
    [[nodiscard]] std::uint64_t bytes_written() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Incremental WAV reader, WavStreamWriter's read-side counterpart: for
// inputs too long to hold in memory. read_wav() above peaks at the whole
// file PLUS its planar float copy resident at once - fine for a fixture,
// gigabytes for a feature-length programme - where this holds one block.
// Same format support and the same sample conversion as read_wav (PCM16 /
// float32, EXTENSIBLE unwrapped, a data chunk shorter than declared is
// tolerated at its real length), so a block-at-a-time consumer sees exactly
// the samples the whole-file overloads produce. Needs a seekable file, which
// is why the whole-file overloads keep the stdin/pipe case.
class AC3FORGE_EXPORT WavStreamReader {
   public:
    WavStreamReader();
    ~WavStreamReader();
    WavStreamReader(const WavStreamReader&) = delete;
    WavStreamReader& operator=(const WavStreamReader&) = delete;
    WavStreamReader(WavStreamReader&&) noexcept;
    WavStreamReader& operator=(WavStreamReader&&) noexcept;

    // Opens `path` and parses the RIFF header. The fmt and data chunks must
    // sit within the first 64 KiB - true of every WAV this project produces
    // or has ever consumed; a file with a deeper header is refused
    // (kNotRiffWave) rather than mis-read, and read_wav still handles it.
    [[nodiscard]] std::expected<void, WavError> open(const std::string& path);

    [[nodiscard]] bool is_open() const;
    [[nodiscard]] std::uint32_t sample_rate() const;
    [[nodiscard]] std::uint16_t channels() const;
    // Frames in the data chunk (its declared size clamped to what the file
    // actually holds, same as read_wav).
    [[nodiscard]] std::uint64_t frame_count() const;

    // Reads up to `frames` frames, deinterleaved into planar [-1, 1) floats:
    // channels[ch][i], one span per channel, each at least `frames` long.
    // Returns the number of frames actually read - less than `frames` only
    // at the end of the data chunk, 0 once it is exhausted. kTruncated if
    // the underlying read fails mid-chunk.
    [[nodiscard]] std::expected<std::size_t, WavError> read_planar(
        std::span<const std::span<float>> channels, std::size_t frames);

    void close();

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ac3::io
