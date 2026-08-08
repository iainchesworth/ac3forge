#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

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

inline constexpr int kBsid = 16;

// §E2.3.1.1, Table E2.1. Type 0x2 - an independent substream whose program
// was previously coded as AC-3 - is deliberately absent: it drags in a
// blkid/frmsizecod branch nothing here would ever write.
enum class StreamType : std::uint8_t {
    kIndependent = 0,  // decodable alone; begins an access unit
    kDependent = 1,    // extends the independent substream it follows
};

// §E2.3.1.8, Table E2.5 - the custom channel map. Bit 0 is stored in the MOST
// significant bit of the 16-bit field ("Bit 0, which indicates the presence of
// the left channel, is stored in the most significant bit"), so location n has
// mask 1 << (15 - n). The spec's own worked example - bits 3, 4 and 6 giving
// Ls, Rs, Lrs, Rrs - is 0x1A00, which only comes out right under that
// numbering.
//
// Six of the sixteen locations name a PAIR of channels rather than one, so a
// map's population count is not its channel count.
namespace chanmap {

inline constexpr std::uint16_t kLeft = 0x8000;          // bit 0
inline constexpr std::uint16_t kCentre = 0x4000;        // bit 1
inline constexpr std::uint16_t kRight = 0x2000;         // bit 2
inline constexpr std::uint16_t kLeftSurround = 0x1000;  // bit 3
inline constexpr std::uint16_t kRightSurround = 0x0800; // bit 4
inline constexpr std::uint16_t kLcRc = 0x0400;          // bit 5  (pair)
inline constexpr std::uint16_t kLrsRrs = 0x0200;        // bit 6  (pair)
inline constexpr std::uint16_t kCs = 0x0100;            // bit 7
inline constexpr std::uint16_t kTs = 0x0080;            // bit 8
inline constexpr std::uint16_t kLsdRsd = 0x0040;        // bit 9  (pair)
inline constexpr std::uint16_t kLwRw = 0x0020;          // bit 10 (pair)
inline constexpr std::uint16_t kVhlVhr = 0x0010;        // bit 11 (pair)
inline constexpr std::uint16_t kVhc = 0x0008;           // bit 12
inline constexpr std::uint16_t kLtsRts = 0x0004;        // bit 13 (pair)
inline constexpr std::uint16_t kLfe2 = 0x0002;          // bit 14
inline constexpr std::uint16_t kLfe = 0x0001;           // bit 15

inline constexpr std::uint16_t kPairs =
    kLcRc | kLrsRrs | kLsdRsd | kLwRw | kVhlVhr | kLtsRts;

// Coded channels a map accounts for. §E2.3.1.8 requires this to equal the
// channels the substream's acmod and lfeon code, and the coded order to
// follow the enabled bits from bit 0 downwards.
[[nodiscard]] constexpr int channel_count(std::uint16_t map) {
    return std::popcount(map) +
           std::popcount(static_cast<std::uint16_t>(map & kPairs));
}

// Canonical 7.1: the dependent replaces the bed's surrounds and adds the two
// rear surrounds. This is the spec's own example (bits 3, 4, 6 with acmod 2/2).
inline constexpr std::uint16_t k71Rear = kLeftSurround | kRightSurround | kLrsRrs;
// 5.1.2: two height channels supplementing an untouched 5.1 bed.
inline constexpr std::uint16_t k512Height = kVhlVhr;
// Four ceiling channels - front and rear height. Both are PAIR locations, so
// two bits account for four channels.
inline constexpr std::uint16_t kTopQuad = kVhlVhr | kLtsRts;

static_assert(k71Rear == 0x1A00, "Table E2.5 bit 0 must be the MSB");
static_assert(channel_count(k71Rear) == 4);
static_assert(channel_count(k512Height) == 2);
static_assert(channel_count(kTopQuad) == 4);

// A substream codes at most 3/2 plus LFE (Table 5.8), so ONE dependent adds at
// most five full-bandwidth channels. chanmap does not lift that: §E2.3.1.8
// requires the locations it names to equal the coded channel count, so a pair
// bit spends two coded channels rather than conjuring one. Hence 5.1.4 needs
// four new channels and fits a single dependent, while 7.1.4 needs six and
// cannot - it is the reason kTopQuad rides beside k71Rear in a second
// dependent rather than merging into one.

}  // namespace chanmap

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

    // TS 103 420 §8.3. An object-audio stream sets flag_ec3_extension_type_a in
    // the addbsi field of whichever substream carries the EMDF container, and
    // follows it with the number of bed, ISF and dynamic objects (§8.3.2.2 caps
    // it at 16). This is the only Atmos marker a decoder can read without
    // hunting through the aux data for the container, and it is what FFmpeg
    // keys its "Dolby Digital Plus + Dolby Atmos" report off. std::nullopt
    // writes addbsie == 0, which is what every stream here did before.
    std::optional<int> oba_complexity_index = std::nullopt;
};

// Words per syncframe at a given rate. E-AC-3 signals the size directly, so
// this is just the exact bit budget rounded to whole 16-bit words.
[[nodiscard]] constexpr std::uint32_t frame_words(SampleRate sample_rate,
                                                  std::uint32_t bitrate_kbps) {
    const std::uint64_t bits = static_cast<std::uint64_t>(bitrate_kbps) * 1000 *
                               kSamplesPerFrame / sample_rate_hz(sample_rate);
    return static_cast<std::uint32_t>(bits / 16);
}

// An EMDF container (ac3::emdf::build_container) to carry in this frame's aux
// data, or an empty span for none.
//
// A/52 §5.4.4.1 puts aux user data at the END of the auxbits field, immediately
// before auxdatal, "so a decoder can find and unpack the auxdatal user bits
// without knowing the value of nauxbits" - nauxbits being unknowable until the
// whole frame has been decoded. So the container is not appended after the
// padding; the padding is what gets pushed in front of it.
using AuxPayload = std::span<const std::byte>;

[[nodiscard]] std::expected<std::vector<std::byte>, FrameError> build_silent_frame(
    const FrameConfig& config, AuxPayload aux = {});

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
        std::span<const std::span<const float>> channels, AuxPayload aux = {});

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

// TS 103 420 §8.2 fixes which substream carries the container: the LAST
// dependent substream if the access unit has any, otherwise the independent
// one. The object metadata describes the whole program, so it may not arrive
// before every substream that contributes to it.
[[nodiscard]] std::expected<AccessUnit, FrameError> build_silent_access_unit(
    const AccessUnitConfig& config, AuxPayload aux = {});

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
        std::span<const std::span<const float>> channels, AuxPayload aux = {});

    [[nodiscard]] const AccessUnitConfig& config() const { return config_; }
    // Summed across substreams: the span count encode_access_unit expects.
    [[nodiscard]] int channel_count() const;

private:
    AccessUnitConfig config_;
    std::vector<FrameEncoder> substreams_;
};

}  // namespace ac3::eac3
