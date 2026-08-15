#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/export.hpp"

// The in-repo AC-3 / E-AC-3 decoder — the validation pyramid's strongest
// correctness anchor (fully normative, shares tables/bit-allocation/exponents/
// IMDCT with the encoder core).
//
// AC-3 scope (bsid <= 8): any acmod 0/0..3/2 plus LFE, long blocks,
// D15/D25/D45/reuse exponents, full bit allocation including delta bit
// allocation (§7.2.2.6), mantissa ungrouping, coupling (strategy, banded
// coordinates, phase flags and leak parameters) and 2/0 rematrixing.
// acmod 0 (1+1 dual mono) is two independent programmes sharing one
// syncframe — Ch2's dialnorm2/compr2/dynrng2 are parsed and reported
// alongside Ch1's, and each programme's §7.7 gain is applied to its own
// channel only. Block switching (§8.2.2/§7.9) is decoded too — DecodedFrame::
// blksw reports which blocks used the short transform. dynrng words are
// parsed but not applied; bap-0 bins reconstruct as zero regardless of
// dithflag (the spec lets the dither sequence be "any reasonably random
// sequence"; zeros keep decode parity deterministic).
//
// E-AC-3 scope (Annex E, bsid 11-16): the whole of Tables E1.2/E1.3/E1.4 as
// syntax — every metadata payload is walked correctly whether or not its
// contents are used — plus dependent substreams, chanmap and the §E3.8.2
// render. Every coding tool Annex E adds on top of AC-3 is implemented: AHT,
// spectral extension, enhanced coupling (§E3.5) and transient pre-noise
// processing (§3.7) - individually or all stacked together. Two syntax
// corners inside those tools are still recognised and refused rather than
// mis-decoded (enhanced coupling's angle-interpolation flag, Annex E's
// default coupling band structure), because no stream this project's own
// encoder produces exercises them. Transient pre-noise processing has one
// consequence for this class's own API: see decode_substream and flush()
// below. This is the only oracle 7.1.4 has: FFmpeg rejects any frame with
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

[[nodiscard]] AC3FORGE_EXPORT std::string_view describe(DecodeError error);

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
    // Ch2's own dialnorm/compr/dynrng (§5.4.2.16-22), present only when acmod
    // is kDualMono — the second of the two independent programmes 1+1 codes.
    std::optional<int> dialnorm2 = std::nullopt;
    std::optional<std::uint8_t> compr2 = std::nullopt;
    std::array<std::uint8_t, kBlocksPerFrame> dynrng2{};
    // §8.2.2/§7.9: per full-bandwidth channel, per block - true where that
    // block used the short (block-switched) transform. Sized to nfchans; the
    // LFE and any coupling channel never switch, so they carry no entry.
    std::vector<std::array<bool, kBlocksPerFrame>> blksw;
    // nchans x kSamplesPerFrame, AC-3 channel order, LFE last when present.
    std::vector<std::vector<float>> channels;
};

class AC3FORGE_EXPORT FrameDecoder {
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
    // Ch2's own dialnorm/compr, present only when acmod is kDualMono (1+1) -
    // the second of the two independent programmes 1+1 codes.
    std::optional<int> dialnorm2 = std::nullopt;
    std::optional<std::uint8_t> compr2 = std::nullopt;
    int numblkscod = 3;
    // §E2.3.1.8: only a dependent substream may carry one.
    std::optional<std::uint16_t> chanmap;
    // §E3.8.5: in a dependent substream compre does not announce a compression
    // word so much as mark the LAST dependent of the program — the point at
    // which a decoder knows every channel has arrived.
    bool last_dependent = false;
    // §8.2.2/§7.9: per full-bandwidth channel, per block - true where that
    // block used the short (block-switched) transform. Sized to nfchans; the
    // LFE and any coupling channel never switch, so they carry no entry.
    std::vector<std::array<bool, kBlocksPerFrame>> blksw;
    std::vector<std::vector<float>> channels;

    // The Table E2.5 map this substream's channels occupy.
    [[nodiscard]] std::uint16_t location_map() const {
        return chanmap ? *chanmap : eac3::chanmap::acmod_map(acmod, lfe);
    }
};

// One program's channels after §E3.8.2: the independent substream's bed with
// each dependent's channels laid over it, in Table E2.5 location order (which
// for a lone 5.1 bed is exactly the AC-3 channel order).
//
// Dual mono (acmod kDualMono) is the one exception: 1+1 is always a single
// substream with no bed/dependent split, and its two channels are unrelated
// programmes with no Table E2.5 location - Ch1 and Ch2, not L and R. `layout`
// is left empty (count 0) in that case, matching ac3::meta::layout_of()'s own
// "not a layout" stance, and `channels` holds Ch1 then Ch2 in coded order.
struct DecodedAccessUnit {
    SampleRate sample_rate = SampleRate::k48000;
    Acmod acmod = Acmod::k2_0;
    int dialnorm = 31;
    int substream_count = 0;
    eac3::chanmap::Layout layout;
    std::vector<std::vector<float>> channels;  // parallel to layout, except dual mono
};

class AC3FORGE_EXPORT Eac3Decoder {
   public:
    // Decodes one syncframe. Overlap-add state is kept per substream identity,
    // so the substreams of successive access units stay independent of each
    // other; a caller stepping through syncframes by hand gets the same audio
    // as one calling decode_access_unit.
    //
    // Returns std::nullopt exactly when a frame's PCM is being held back
    // pending transient pre-noise processing (§3.7): a stream's very first
    // frame that turns transproce on has nothing ready to return yet, because
    // whether a correction reaches back into it is only known once the NEXT
    // frame has been parsed. A stream that never uses the tool always gets a
    // populated result immediately - this holding-back is the exception, not
    // the common case. Call flush() once at end-of-stream to collect
    // whichever frame is still held back, if any.
    [[nodiscard]] std::expected<std::optional<DecodedSubstream>, DecodeError> decode_substream(
        std::span<const std::byte> frame);

    // Decodes one access unit — an independent substream followed by its
    // dependents, exactly as split_access_units delimits them — and renders it.
    //
    // Same std::nullopt convention as decode_substream, for the same reason:
    // assembling one access unit needs every one of its substreams ready in
    // the SAME call, and decode_substream can hold one back independently of
    // the others (§3.7's transient pre-noise processing is a per-substream
    // flag). When that happens, whichever OTHER substreams of this access
    // unit already released this call are held in an internal per-identity
    // cache until the rest catch up - so nothing already-ready is discarded,
    // and a later call finishes the assembly once every identity this
    // program uses has a result waiting. A stream that never uses the tool
    // is unaffected: every substream releases every call, so the cache never
    // holds more than one call's worth at a time and every call returns a
    // populated result immediately.
    [[nodiscard]] std::expected<std::optional<DecodedAccessUnit>, DecodeError> decode_access_unit(
        std::span<const std::byte> unit);

    // Releases whichever frames transient pre-noise processing is still
    // holding back, one per substream identity that has one pending - empty
    // if none does, which covers every stream that never used the tool.
    // Call once, after the last decode_substream/decode_access_unit call for
    // a stream, to avoid silently dropping its final frame(s). Drains BOTH
    // decode_substream's own pending frame and decode_access_unit's
    // assembly cache (see its own doc comment) - a caller that only ever
    // used decode_access_unit and wants the very last program's worth of
    // audio out of a stream that ends mid-hold-back gets raw per-substream
    // results here rather than one final assembled DecodedAccessUnit,
    // since by definition the assembly never completed.
    [[nodiscard]] std::vector<DecodedSubstream> flush();

   private:
    // Keyed by strmtyp and substreamid together: a dependent's id lives in its
    // own numbering space (§E2.3.1.2), so id alone does not identify a
    // substream. At most six coded channels each (3/2 plus LFE).
    std::map<int, std::array<std::array<double, 256>, 6>> delay_;
    // A substream identity enters this map the first time one of its frames
    // sets transproce, and stays in it (buffering one frame at a time) for
    // the rest of the stream - see decode_substream's own doc comment.
    std::map<int, DecodedSubstream> pending_;
    // decode_access_unit's own assembly cache: a substream identity's
    // RELEASED (by decode_substream) results, oldest first, waiting for
    // every other identity the same call's frames named to also have one -
    // see decode_access_unit's own doc comment. A queue rather than a single
    // slot: one identity can release several times while another is still
    // catching up (a dependent that never uses the tool releases every call,
    // while the independent using it lags by one), and an already-queued,
    // not-yet-assembled result must never be overwritten by a later one for
    // the same identity - that would silently splice two different points
    // in time into one access unit.
    std::map<int, std::deque<DecodedSubstream>> pending_au_parts_;

    // decode_substream's own per-block IMDCT/enhanced-coupling scratch
    // (PREfast's C6262, alert #63): reused across every (block, channel)
    // iteration of a call instead of stack-declared per iteration, the same
    // reasoning as FrameEncoder's MDCT scratch members. Each is fully
    // overwritten before being read, so nothing needs to persist beyond one
    // decode_substream call - unlike delay_ above, these don't need to be
    // keyed by substream identity.
    std::array<double, 512> imdct_scratch_{};
    std::array<double, 256> ecpl_spectrum_real_{};
    std::array<double, 256> ecpl_spectrum_imag_{};
};

// Split a raw elementary stream into syncframes by sync word and declared
// size. Handles both generations; bsid at bit 40 decides which.
[[nodiscard]] AC3FORGE_EXPORT std::expected<std::vector<std::span<const std::byte>>, DecodeError>
split_frames(std::span<const std::byte> stream);

// Group those syncframes into access units. A new one begins at each
// independent substream, and the spans returned are the concatenations the
// bitstream itself defines.
[[nodiscard]] AC3FORGE_EXPORT std::expected<std::vector<std::span<const std::byte>>, DecodeError>
split_access_units(std::span<const std::byte> stream);

// bsid at bit 40, without committing to either layout. Fails only if the span
// is too short to hold a header.
[[nodiscard]] AC3FORGE_EXPORT std::expected<int, DecodeError> stream_bsid(
    std::span<const std::byte> frame);

}  // namespace ac3
