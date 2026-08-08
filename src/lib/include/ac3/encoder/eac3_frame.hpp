#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/encoder/silent_frame.hpp"  // FrameError

// E-AC-3 (Dolby Digital Plus) framing - ATSC A/52:2018 Annex E, bsid 16.
//
// E-AC-3 is not a variant of the AC-3 frame; it is a different container for
// the same coding tools:
//   - syncinfo is ONLY the sync word. There is no crc1, so the GF(2) leading
//     -CRC solver AC-3 needs has no counterpart here.
//   - frmsiz is an arbitrary 11-bit word count rather than a table lookup, so
//     any frame size is directly expressible and the 44.1 kHz padding
//     alternation AC-3 needs disappears.
//   - Exponent strategies and coupling-in-use for EVERY block are hoisted
//     into a frame-level audfrm element ahead of the blocks, and several
//     per-block fields become conditional on frame-level flags.
//
// This first step emits a valid, decodable bsid-16 frame carrying digital
// silence, the same way the AC-3 work started: all SNR offsets zero, which
// §7.2.2.1.1 defines as an all-zero bit allocation, so no mantissa data
// exists and the frame is pure syntax.

namespace ac3::eac3 {

// kBsid, StreamType and the Table E2.5 chanmap live in
// ac3/core/eac3_tables.hpp: the decoder reads the same fields this writes, and
// one definition is what keeps the two agreeing on what a bit pattern means.
// This encoder writes only strmtyp 0x0 and 0x1 - 0x2, an independent substream
// whose program was previously coded as AC-3, drags in a blkid/frmsizecod
// branch nothing here would ever emit, so validate() refuses it.

struct FrameConfig {
    SampleRate sample_rate = SampleRate::k48000;
    std::uint32_t bitrate_kbps = 192;
    Acmod acmod = Acmod::k2_0;
    bool lfe = false;
    int dialnorm = 31;
    int chbwcod = 60;

    // --- substream identity (Table E1.2) -----------------------------------
    // The defaults describe the lone independent substream this encoder has
    // always emitted, so existing callers keep their exact bit layout.
    StreamType strmtyp = StreamType::kIndependent;
    // §E2.3.1.2. Independent substreams number from 0; the dependents of one
    // independent substream number from 0 in their OWN space, so a dependent's
    // id does not continue its parent's.
    int substreamid = 0;
    // Sent only by dependent substreams. std::nullopt clears chanmape, which
    // lets acmod and lfeon speak for themselves - the dependent's channels
    // then simply overwrite the matching ones in the independent substream.
    std::optional<std::uint16_t> chanmap = std::nullopt;
    // §E3.8.5. In a dependent substream compre stops meaning "a compression
    // word follows" and becomes the marker for the LAST dependent of the
    // program - it is how a decoder knows the program is complete. Set by the
    // access-unit builder; meaningless on an independent substream.
    bool last_dependent = false;
};

// Words per syncframe at a given rate. E-AC-3 signals the size directly, so
// this is just the exact bit budget rounded to whole 16-bit words.
[[nodiscard]] constexpr std::uint32_t frame_words(SampleRate sample_rate,
                                                  std::uint32_t bitrate_kbps) {
    const std::uint64_t bits = static_cast<std::uint64_t>(bitrate_kbps) * 1000 *
                               kSamplesPerFrame / sample_rate_hz(sample_rate);
    return static_cast<std::uint32_t>(bits / 16);
}

[[nodiscard]] std::expected<std::vector<std::byte>, FrameError> build_silent_frame(
    const FrameConfig& config);

// Real audio through the same container. The coding profile is deliberately
// the one reference encoders use, because those are the paths reference
// decoders are exercised on: frame-level exponent strategies (Table E2.10
// code 0 - D15 in block 0, reused for the other five) and frame-level SNR
// offsets. No coupling, no spectral extension, long blocks only.
class FrameEncoder {
public:
    explicit FrameEncoder(const FrameConfig& config) : config_(config) {}

    // channels: the full-bandwidth channels in AC-3 order (Table 5.8),
    // followed by LFE last when config.lfe is set. Each span holds exactly
    // kSamplesPerFrame samples, nominally in [-1, 1).
    [[nodiscard]] std::expected<std::vector<std::byte>, FrameError> encode_frame(
        std::span<const std::span<const float>> channels);

    [[nodiscard]] const FrameConfig& config() const { return config_; }
    [[nodiscard]] int channel_count() const {
        return fullbw_channel_count(config_.acmod) + (config_.lfe ? 1 : 0);
    }

private:
    FrameConfig config_;
    std::array<std::array<double, 256>, 6> history_{};  // MDCT overlap per channel
};

// An independent substream and the dependents that extend it. Every substream
// codes the same 1536 samples of the same program, so a dependent contributes
// only its own channels, its chanmap and its share of the bit rate.
struct AccessUnitConfig {
    FrameConfig independent{};
    std::vector<FrameConfig> dependents{};
};

// One access unit: the independent substream's frame followed by its
// dependents' in transmission order, concatenated exactly as they go on the
// wire. Nothing may sit between them and they may not be reordered - a decoder
// finds each substream by walking sync word and frmsiz, so the concatenation
// IS the framing.
struct AccessUnit {
    std::vector<std::byte> bytes;
    // Byte length of each substream frame, independent first; sums to
    // bytes.size(). Retained because crc2 is per substream, so anything that
    // re-checks a written stream has to find these boundaries again.
    std::vector<std::uint32_t> substream_bytes;

    [[nodiscard]] std::size_t substream_count() const { return substream_bytes.size(); }
    [[nodiscard]] std::span<const std::byte> substream(std::size_t index) const;
};

// Words in a whole access unit. bitrate_kbps is PER SUBSTREAM - the substreams
// share one frame period, not one frame - so the total is the sum.
[[nodiscard]] std::uint32_t access_unit_words(const AccessUnitConfig& config);

[[nodiscard]] std::expected<AccessUnit, FrameError> build_silent_access_unit(
    const AccessUnitConfig& config);

// Real audio across an independent substream and its dependents. One
// FrameEncoder per substream: each keeps its own MDCT overlap and runs its own
// SNR search against its own share of the rate.
class AccessUnitEncoder {
public:
    explicit AccessUnitEncoder(const AccessUnitConfig& config);

    // channels: every channel of the access unit grouped by substream in
    // transmission order - the independent's first (AC-3 order, Table 5.8,
    // LFE last), then each dependent's in the order its chanmap names them.
    [[nodiscard]] std::expected<AccessUnit, FrameError> encode_access_unit(
        std::span<const std::span<const float>> channels);

    [[nodiscard]] const AccessUnitConfig& config() const { return config_; }
    // Summed across substreams: the span count encode_access_unit expects.
    [[nodiscard]] int channel_count() const;

private:
    AccessUnitConfig config_;
    std::vector<FrameEncoder> substreams_;
};

}  // namespace ac3::eac3
