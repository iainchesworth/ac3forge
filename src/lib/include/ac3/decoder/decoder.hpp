#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"

// The in-repo AC-3 / E-AC-3 decoder — the validation pyramid's strongest
// correctness anchor (fully normative, shares tables/bit-allocation/exponents/
// IMDCT with the encoder core).
//
// AC-3 scope (bsid <= 8): any acmod 1/0..3/2 plus LFE, long blocks,
// D15/D25/D45/reuse exponents, full bit allocation including delta bit
// allocation (§7.2.2.6), mantissa ungrouping, coupling (strategy, banded
// coordinates, phase flags and leak parameters) and 2/0 rematrixing.
// Deliberately unsupported (clean errors, not wrong audio): block switching,
// dual mono. dynrng words are parsed but not applied; bap-0 bins reconstruct
// as zero regardless of dithflag (the spec lets the dither sequence be "any
// reasonably random sequence"; zeros keep decode parity deterministic).
//
// E-AC-3 scope (Annex E, bsid 11-16): the whole of Tables E1.2/E1.3/E1.4 as
// syntax — every metadata payload is walked correctly whether or not its
// contents are used — plus dependent substreams, chanmap and the §E3.8.2
// render. The coding tools Annex E adds on top of AC-3 (AHT, spectral
// extension, enhanced coupling, transient pre-noise processing) are parsed far
// enough to be recognised and then refused, again rather than mis-decoded.
// This is the only oracle 7.1.4 has: FFmpeg rejects any frame with
// substreamid != 0, so a stream with two dependent substreams cannot be
// checked against it in any container.
//
// The §7.7 dynamic range words are always reported and optionally applied —
// see DecoderConfig. Reporting them separately from applying them is what
// makes this useful as a check on the encoder: a test can assert on the words
// the encoder chose AND on the level change they cause, and those are two
// different claims.

namespace ac3 {

enum class DecodeError : std::uint8_t {
    kTruncated,
    kBadSyncWord,
    kBadCrc,
    kReservedValue,
    kUnsupported,  // legal AC-3, but syntax this decoder declines to read
    kInvalidStream,
};

[[nodiscard]] std::string_view describe(DecodeError error);

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

// --- E-AC-3 ----------------------------------------------------------------

// One decoded syncframe of an E-AC-3 stream. `channels` are in the substream's
// own coded order (Table 5.8, LFE last); where those channels BELONG is
// `chanmap` when a dependent sent one and acmod/lfeon otherwise.
struct DecodedSubstream {
    eac3::StreamType strmtyp = eac3::StreamType::kIndependent;
    int substreamid = 0;
    SampleRate sample_rate = SampleRate::k48000;
    Acmod acmod = Acmod::k2_0;
    bool lfe = false;
    int dialnorm = 31;
    int numblkscod = 3;
    // §E2.3.1.8: only a dependent substream may carry one.
    std::optional<std::uint16_t> chanmap;
    // §E3.8.5: in a dependent substream compre does not announce a compression
    // word so much as mark the LAST dependent of the program — the point at
    // which a decoder knows every channel has arrived.
    bool last_dependent = false;
    std::vector<std::vector<float>> channels;

    // The Table E2.5 map this substream's channels occupy.
    [[nodiscard]] std::uint16_t location_map() const {
        return chanmap ? *chanmap : eac3::chanmap::acmod_map(acmod, lfe);
    }
};

// One program's channels after §E3.8.2: the independent substream's bed with
// each dependent's channels laid over it, in Table E2.5 location order (which
// for a lone 5.1 bed is exactly the AC-3 channel order).
struct DecodedAccessUnit {
    SampleRate sample_rate = SampleRate::k48000;
    int dialnorm = 31;
    int substream_count = 0;
    eac3::chanmap::Layout layout;
    std::vector<std::vector<float>> channels;  // parallel to layout
};

class Eac3Decoder {
public:
    // Decodes one syncframe. Overlap-add state is kept per substream identity,
    // so the substreams of successive access units stay independent of each
    // other; a caller stepping through syncframes by hand gets the same audio
    // as one calling decode_access_unit.
    [[nodiscard]] std::expected<DecodedSubstream, DecodeError> decode_substream(
        std::span<const std::byte> frame);

    // Decodes one access unit — an independent substream followed by its
    // dependents, exactly as split_access_units delimits them — and renders it.
    [[nodiscard]] std::expected<DecodedAccessUnit, DecodeError> decode_access_unit(
        std::span<const std::byte> unit);

private:
    // Keyed by strmtyp and substreamid together: a dependent's id lives in its
    // own numbering space (§E2.3.1.2), so id alone does not identify a
    // substream. At most six coded channels each (3/2 plus LFE).
    std::map<int, std::array<std::array<double, 256>, 6>> delay_;
};

// Split a raw elementary stream into syncframes by sync word and declared
// size. Handles both generations; bsid at bit 40 decides which.
[[nodiscard]] std::expected<std::vector<std::span<const std::byte>>, DecodeError> split_frames(
    std::span<const std::byte> stream);

// Group those syncframes into access units. A new one begins at each
// independent substream, and the spans returned are the concatenations the
// bitstream itself defines.
[[nodiscard]] std::expected<std::vector<std::span<const std::byte>>, DecodeError>
split_access_units(std::span<const std::byte> stream);

// bsid at bit 40, without committing to either layout. Fails only if the span
// is too short to hold a header.
[[nodiscard]] std::expected<int, DecodeError> stream_bsid(std::span<const std::byte> frame);

}  // namespace ac3
