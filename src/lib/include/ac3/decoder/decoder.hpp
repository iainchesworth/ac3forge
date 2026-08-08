#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/core/tables.hpp"

// The in-repo AC-3 decoder — the validation pyramid's strongest correctness
// anchor (fully normative, shares tables/bit-allocation/exponents/IMDCT with
// the encoder core) and later the E-AC-3 debugging tool.
//
// Scope: the bsid<=8 syntax the project's encoder emits, decoded exactly:
// any acmod 1/0..3/2 plus LFE, long blocks, D15/D25/D45/reuse exponents,
// full bit allocation, mantissa ungrouping, coupling (strategy, banded
// coordinates, phase flags and leak parameters) and 2/0 rematrixing.
// Deliberately unsupported (clean errors, not wrong audio): block switching,
// delta bit allocation, dual mono. dynrng words are parsed but not applied;
// bap-0 bins reconstruct as zero regardless of dithflag (the spec lets the
// dither sequence be "any reasonably random sequence"; zeros keep decode
// parity deterministic).

namespace ac3 {

enum class DecodeError {
    kTruncated,
    kBadSyncWord,
    kBadCrc,
    kReservedValue,
    kUnsupported,  // legal AC-3, but syntax this decoder declines to read
    kInvalidStream,
};

[[nodiscard]] std::string_view describe(DecodeError error);

struct DecodedFrame {
    SampleRate sample_rate = SampleRate::k48000;
    std::uint32_t bitrate_kbps = 0;
    Acmod acmod = Acmod::k2_0;
    bool lfe = false;
    int dialnorm = 31;
    // nchans x kSamplesPerFrame, AC-3 channel order, LFE last when present.
    std::vector<std::vector<float>> channels;
};

class FrameDecoder {
public:
    // Decodes exactly one syncframe (the span must be exactly one frame).
    [[nodiscard]] std::expected<DecodedFrame, DecodeError> decode_frame(
        std::span<const std::byte> frame);

private:
    std::array<std::array<double, 256>, 6> delay_{};  // overlap-add state
};

// Convenience: split a raw elementary stream into frames by sync/size.
[[nodiscard]] std::expected<std::vector<std::span<const std::byte>>, DecodeError> split_frames(
    std::span<const std::byte> stream);

}  // namespace ac3
