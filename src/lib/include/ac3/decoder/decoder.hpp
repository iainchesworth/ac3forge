#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "ac3/core/tables.hpp"

// The in-repo AC-3 decoder — the validation pyramid's strongest correctness
// anchor (fully normative, shares tables/bit-allocation/exponents/IMDCT with
// the encoder core) and later the E-AC-3 debugging tool.
//
// Scope: the bsid<=8 syntax the project's encoder emits, decoded exactly:
// any acmod 1/0..3/2 plus LFE, long blocks, D15/D25/D45/reuse exponents,
// full bit allocation, mantissa ungrouping. Deliberately unsupported (clean
// errors, not wrong audio): coupling, block switching, delta bit allocation,
// dual mono. bap-0 bins reconstruct as zero regardless of dithflag (the spec
// lets the dither sequence be "any reasonably random sequence"; zeros keep
// decode parity deterministic).
//
// The §7.7 dynamic range words are always reported and optionally applied —
// see DecoderConfig. Reporting them separately from applying them is what
// makes this useful as a check on the encoder: a test can assert on the words
// the encoder chose AND on the level change they cause, and those are two
// different claims.

namespace ac3 {

enum class DecodeError {
    kTruncated,
    kBadSyncWord,
    kBadCrc,
    kReservedValue,
    kUnsupported,
    kInvalidStream,
};

struct DecoderConfig {
    // §7.7.1's "Partial Compression": the dynrng word may be scaled so that a
    // fraction of the coded compression is applied. 0 ignores dynrng entirely
    // and reproduces the full dynamic range; 1 applies it as the encoder
    // intended. A/52 §7.7.1.1 says a consumer decoder "shall, by default,
    // implement the compression characteristic" — this one defaults to 0
    // because it exists to check what the encoder wrote, and a decoder that
    // silently rescales its output cannot be the reference for that.
    double drc_scale = 0.0;
    // §7.7.2: prefer compr over dynrng wherever a compr word exists, which is
    // what a set-top box's RF mode does. §7.7.2.1 requires falling back on
    // dynrng for any syncframe that carries no compr, so this composes with
    // drc_scale rather than replacing it.
    bool heavy_compression = false;
};

struct DecodedFrame {
    SampleRate sample_rate = SampleRate::k48000;
    std::uint32_t bitrate_kbps = 0;
    Acmod acmod = Acmod::k2_0;
    bool lfe = false;
    int dialnorm = 31;
    // §5.4.2.9: std::nullopt when compre was clear, so "no word" and "a word
    // that happens to say unity" stay distinguishable.
    std::optional<std::uint8_t> compr = std::nullopt;
    // §7.7.1.2: the EFFECTIVE word for each block, with the persistence rule
    // already resolved — a block that transmitted nothing reports what it
    // inherited, and block 0 without a word reports unity rather than
    // whatever the previous frame ended on.
    std::array<std::uint8_t, kBlocksPerFrame> dynrng{};
    // nchans x kSamplesPerFrame, AC-3 channel order, LFE last when present.
    std::vector<std::vector<float>> channels;
};

class FrameDecoder {
public:
    FrameDecoder() = default;
    explicit FrameDecoder(const DecoderConfig& config) : config_(config) {}

    // Decodes exactly one syncframe (the span must be exactly one frame).
    [[nodiscard]] std::expected<DecodedFrame, DecodeError> decode_frame(
        std::span<const std::byte> frame);

private:
    DecoderConfig config_{};
    std::array<std::array<double, 256>, 6> delay_{};  // overlap-add state
};

// Convenience: split a raw elementary stream into frames by sync/size.
[[nodiscard]] std::expected<std::vector<std::span<const std::byte>>, DecodeError> split_frames(
    std::span<const std::byte> stream);

}  // namespace ac3
