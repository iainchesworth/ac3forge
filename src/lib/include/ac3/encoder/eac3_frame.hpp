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

    // --- Annex E coding tools -----------------------------------------------
    // Channel coupling (§E3.3, §7.4). Needs two full-bandwidth channels to
    // share anything, so it is ignored for mono. Above the coupling frequency
    // the coupled channels stop carrying coefficients of their own and a
    // single shared channel plus per-channel per-band coordinates stands in
    // for them; chbwcod then disappears, because the coupling frequency IS
    // the coded bandwidth of every coupled channel.
    bool coupling = false;
    // Coupling begin frequency code (§5.4.3.11), 0-15: the region starts at
    // coefficient 37 + 12 * cplbegf. Negative picks a rate-dependent default,
    // which is the useful behaviour - the whole point of coupling is to buy
    // bits at rates that cannot afford two full-bandwidth channels, so the
    // right frequency falls as the rate does.
    int cplbegf = -1;

    // Spectral extension (§E3.6). Above the extension frequency nothing is
    // coded at all: the decoder copies a lower band up, blends it with noise
    // and scales it to the banded envelope the encoder measured. It is
    // cheaper than coupling - scale factors only, no shared channel - and
    // correspondingly cruder, so the two stack: independent low, coupled mid,
    // synthesized high.
    bool spx = false;
    // Spectral extension begin frequency code (§E2.3.3.5), 0-7. Synthesis
    // starts at coefficient 25 + 12 * spx_begin_subbnd(spxbegf), which is
    // non-linear in spxbegf. Negative picks a rate-dependent default.
    //
    // With coupling also in use this value FIXES the coupling end frequency:
    // §E3.3.1 stops transmitting cplendf and derives it from spxbegf, so that
    // coupling ends exactly where synthesis begins. cplbegf is clamped down if
    // it would leave the coupling region empty, and coupling is dropped
    // outright if there is no room for it at all.
    int spxbegf = -1;
    // Spectral extension attenuation (§E3.6.4.2.3): a five-tap notch across
    // the seam where the coded band meets the synthesized one, and across
    // every point where the copy wraps back to its source. Only meaningful
    // when spx is set. It costs six bits per channel per frame.
    bool spx_atten = true;
    // The attenuation depth (§E2.3.2.25), 0-31: deeper with the code. Negative
    // matches the notch to how big a step the seam actually is.
    int spxattencod = -1;

    // Adaptive hybrid transform (§E3.4). A second transform stage - a 6-point
    // DCT down each spectral bin across the frame's six blocks - which for
    // stationary material collapses six coefficients into essentially one.
    // It brings a finer allocation table and vector quantisation with it, and
    // it restructures the frame: an AHT channel's whole frame of mantissas is
    // packed into block 0 and the other five carry nothing for it.
    //
    // It is not free for material that moves between blocks, so it is decided
    // per channel per frame; setting this permits it rather than forces it.
    bool aht = false;
    // Gain-adaptive quantization mode (§E3.4.4.2, Table E3.3), 0-3. Negative
    // lets the encoder pick the cheapest per channel, which is the useful
    // behaviour; pinning it to 0 turns GAQ off while leaving the rest of AHT
    // alone, which is how the tool's contribution gets measured on its own.
    int gaqmod = -1;
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
// offsets. Long blocks only; the Annex E tools are opt-in per FrameConfig.
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
