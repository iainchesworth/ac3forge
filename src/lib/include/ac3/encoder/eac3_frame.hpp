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
#include "ac3/meta/drc.hpp"
#include "ac3/meta/mixing.hpp"

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
    // Annex E Table E1.2: Ch2's dialnorm, required when acmod is kDualMono
    // (1+1) — the two programmes are levelled independently.
    std::optional<int> dialnorm2 = std::nullopt;
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

    // --- dynamic range and mixing metadata ---------------------------------
    // As in AC-3: std::nullopt keeps dynrnge clear in every block and compre
    // clear in bsi, so a metadata-free stream is bit-identical to before.
    std::optional<meta::Profile> drc = std::nullopt;
    // §7.7.2 heavy compression. Only an INDEPENDENT substream can carry it:
    // §E3.8.5 repurposes a dependent's compre as the end-of-programme marker,
    // so the eight bits it drags in there are not a gain any decoder will use.
    //
    // This is the one part of the metadata layer with no external oracle.
    // FFmpeg's E-AC-3 header parser reads compre and then SKIPS the word, so
    // -heavy_compr changes nothing on an E-AC-3 stream however good the
    // metadata is - unlike -drc_scale, which honours dynrng here as it does in
    // AC-3. What holds it up instead: the word format and the generator are
    // shared with the AC-3 path, which ffmpeg does verify, and the field's
    // placement is checked bit by bit (tests/test_drc.cpp, tools/eac3_parse.py).
    std::optional<meta::HeavyConfig> heavy = std::nullopt;
    // The mixmdate group (Table E1.2). E-AC-3 dropped bsi's cmixlev and
    // surmixlev entirely, so without this a stream carries no downmix levels
    // at all and a receiver falls back on its own defaults.
    std::optional<meta::MixMetadata> mixing = std::nullopt;
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

// §E2.3.1.3: frmsiz is 11 bits and holds (words - 1), so 2048 is the largest
// word count this format can ever signal, whatever bitrate produced it.
inline constexpr std::uint32_t kMaxFrameWords = 2048;

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

// The §7.7 words for one frame, separated from FrameConfig because they change
// every frame and from the encoder because every substream of an access unit
// has to carry the SAME ones - see AccessUnitEncoder.
struct FrameMetadata {
    std::array<std::uint8_t, kBlocksPerFrame> dynrng{};
    std::optional<std::uint8_t> compr = std::nullopt;
    // Ch2's own words, present only when the substream's acmod is kDualMono.
    std::array<std::uint8_t, kBlocksPerFrame> dynrng2{};
    std::optional<std::uint8_t> compr2 = std::nullopt;
};

// Real audio through the same container. The coding profile is deliberately
// the one reference encoders use, because those are the paths reference
// decoders are exercised on: frame-level exponent strategies (Table E2.10
// code 0 - D15 in block 0, reused for the other five) and frame-level SNR
// offsets. Long blocks only; the Annex E tools are opt-in per FrameConfig.
class FrameEncoder {
public:
    explicit FrameEncoder(const FrameConfig& config);

    // channels: the full-bandwidth channels in AC-3 order (Table 5.8),
    // followed by LFE last when config.lfe is set. Each span holds exactly
    // kSamplesPerFrame samples, nominally in [-1, 1).
    [[nodiscard]] std::expected<std::vector<std::byte>, FrameError> encode_frame(
        std::span<const std::span<const float>> channels, AuxPayload aux = {});

    // As above, but with the §7.7 words supplied instead of derived. This is
    // the access-unit path: a substream that measured only its own channels
    // would reach a different gain from its siblings, and a decoder applies
    // each substream's word to that substream's channels - so disagreement
    // does not average out, it tilts the mix.
    [[nodiscard]] std::expected<std::vector<std::byte>, FrameError> encode_frame(
        std::span<const std::span<const float>> channels, const FrameMetadata& metadata, AuxPayload aux = {});

    [[nodiscard]] const FrameConfig& config() const { return config_; }
    [[nodiscard]] int channel_count() const {
        return fullbw_channel_count(config_.acmod) + (config_.lfe ? 1 : 0);
    }

private:
    FrameConfig config_;
    std::array<std::array<double, 256>, 6> history_{};  // MDCT overlap per channel
    // Smoothed across frames: see the AC-3 FrameEncoder for why they cannot be
    // per-frame objects.
    std::optional<meta::RangeController> range_;
    std::optional<meta::HeavyCompressor> heavy_;
    // Ch2's own controllers, present only when acmod is kDualMono.
    std::optional<meta::RangeController> range2_;
    std::optional<meta::HeavyCompressor> heavy2_;
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
    // The programme's own controllers, measured on the INDEPENDENT substream's
    // channels. That substream is by definition a self-sufficient rendering of
    // the whole programme (§E1.3.1), so measuring it measures the programme -
    // and the answer does not then depend on how many dependents ride along.
    std::optional<meta::RangeController> range_;
    std::optional<meta::HeavyCompressor> heavy_;
    // Ch2's own controllers, present only when the independent substream's
    // acmod is kDualMono. Dual mono never has dependents (1+1 has no
    // bed/dependent split to make), so "the independent substream" and "the
    // whole programme" are the same two channels here too.
    std::optional<meta::RangeController> range2_;
    std::optional<meta::HeavyCompressor> heavy2_;
    // Its own copy of the independent substream's MDCT overlap - the previous
    // access unit's last 256 samples per channel. The substream encoder keeps
    // the same window for its transform; this copy exists because the peak
    // §7.7.2 bounds has to be measured before any substream runs.
    std::array<std::array<double, 256>, 6> tail_{};
};

}  // namespace ac3::eac3
