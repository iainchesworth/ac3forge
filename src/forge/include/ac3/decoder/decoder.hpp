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
#include "ac3/core/mantissas.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/encoder/eac3_tools.hpp"  // eac3::BandLayout, for BlockTail below
#include "ac3/export.hpp"
#include "ac3/oba/joc.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/verify/mirror.hpp"

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
// parsed but not applied; bap-0 bins reconstruct per §7.3.4's dithflag - a
// true zero when it is clear, a dither sample (DitherGenerator, deterministic
// per decoder instance) when it is set. A coupled channel's shared bap-0
// bins are dithered independently per RECEIVING channel, after decoupling,
// per §7.3.4's own "uncorrelated" requirement - never by dithering the
// shared coupling-channel coefficient itself.
//
// E-AC-3 scope (Annex E, bsid 11-16): the whole of Tables E1.2/E1.3/E1.4 as
// syntax — every metadata payload is walked correctly whether or not its
// contents are used — plus dependent substreams, chanmap and the §E3.8.2
// render. Every coding tool Annex E adds on top of AC-3 is implemented: AHT,
// spectral extension, enhanced coupling (§E3.5) and transient pre-noise
// processing (§3.7) - individually or all stacked together. Annex E's
// default coupling band structures decode too: standard coupling falls back
// to Table E2.12, enhanced coupling to Table E2.13. Two syntax corners are
// still recognised and refused rather than mis-decoded - enhanced coupling's
// angle-interpolation flag, and a transient pre-noise correction reaching
// further back or forward than the one frame of history/lookahead buffered
// here - because no stream this project's own encoder produces exercises
// them. Transient pre-noise processing has one
// consequence for this class's own API: see decode_substream and flush()
// below. This is the only oracle 7.1.4 has: FFmpeg rejects any frame with
// substreamid != 0, so a stream with two dependent substreams cannot be
// checked against it in any container. Every substream's own dynrng/dynrng2
// words are reported on DecodedSubstream, same convention as DecodedFrame,
// and optionally applied per Eac3Decoder's own constructor — see
// DecoderConfig below.
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
    // --- self-check (ac3/verify/mirror.hpp) --------------------------------
    // The decoder's half of EncoderConfig::trace: when set, decode_frame()
    // records the same per-block, per-stream state it derived from the wire,
    // so the two models can be diffed. Null by default, at the same cost as
    // the encoder's - one branch per block. Filled INCREMENTALLY, so a frame
    // the decoder ends up refusing still leaves behind everything it read
    // before the refusal, which is the case the comparison is most useful in.
    // AC-3 only (FrameDecoder); Eac3Decoder does not write one.
    verify::FrameTrace* trace = nullptr;
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

    // As decode_frame, but the PCM lands in caller-owned planar storage
    // instead of freshly allocated vectors - the per-call cost drops from
    // one vector per channel (~37 KB a frame at 5.1) to nothing, which is
    // what a realtime consumer or the WASM demo wants. channels[ch] must
    // each hold kSamplesPerFrame floats and there must be a span for every
    // channel the frame codes - six covers every AC-3 layout; the returned
    // metadata's acmod/lfe say how many were written. The returned
    // DecodedFrame carries everything EXCEPT the PCM (its `channels` is
    // left empty). On an error return the spans' contents are unspecified
    // - exactly as discarded as the value form's partial frame was.
    [[nodiscard]] std::expected<DecodedFrame, DecodeError> decode_frame_into(
        std::span<const std::byte> frame, std::span<const std::span<float>> channels);

   private:
    // Both public forms above: `channels` empty means allocate the PCM into
    // the returned DecodedFrame, non-empty means write through the spans.
    [[nodiscard]] std::expected<DecodedFrame, DecodeError> decode_frame_core(
        std::span<const std::byte> frame, std::span<const std::span<float>> channels);

    DecoderConfig config_{};
    std::array<std::array<double, 256>, 6> delay_{};  // overlap-add state
    // §7.3.4 dither, persisting across frames like delay_ above so a long
    // stream's substituted noise does not repeat every syncframe.
    DitherGenerator dither_{};
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
    // §5.4.2.9/§E3.8.5: std::nullopt when compre was clear OR this substream
    // is a dependent one - a dependent's compre bit is repurposed to mark the
    // LAST dependent of the program rather than announce a compression word
    // (see parse_bsi's own comment), so there is no meaningful compr value to
    // report there even though the 8 bits are still present on the wire.
    std::optional<std::uint8_t> compr = std::nullopt;
    // §7.7.1.2: the EFFECTIVE word for each block, with the persistence rule
    // already resolved, same convention as DecodedFrame::dynrng - a block
    // that transmitted nothing reports what it inherited, and block 0
    // without a word reports unity. Sized to kBlocksPerFrame regardless of
    // how many blocks this syncframe actually codes (numblkscod), matching
    // blksw's own fixed-size convention above; entries at index >= nblks are
    // never written.
    std::array<std::uint8_t, kBlocksPerFrame> dynrng{};
    // Ch2's own dialnorm/compr, present only when acmod is kDualMono (1+1) -
    // the second of the two independent programmes 1+1 codes.
    std::optional<int> dialnorm2 = std::nullopt;
    std::optional<std::uint8_t> compr2 = std::nullopt;
    std::array<std::uint8_t, kBlocksPerFrame> dynrng2{};
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
    // §H.1/TS 103 420 §5.5: the OAMD payload found in one of this substream's
    // block skip fields, if any - std::nullopt for plain E-AC-3 with no
    // object audio at all, and equally for a skip field this decoder found
    // but declined to interpret (see oba::parse_payload's own comment on
    // what it refuses). Which block actually carries the container is not
    // fixed (emdf::build_container's own comment), so every block's skip
    // field is a candidate; the first one that parses wins.
    std::optional<oba::DecodedProgram> object_metadata = std::nullopt;
    // JOC's (§6) reconstructed per-object audio, one waveform per object,
    // parallel to object_metadata->objects (same index means the same
    // object) - empty when object_metadata is unset, when no JOC payload
    // rode alongside the OAMD one, or when the program shape is one JOC's
    // own object ordering cannot be lined up against object_metadata's for
    // (see Eac3Decoder::decode_substream's own comment on this - a bed
    // program AtmosEncoder itself never produces).
    std::vector<std::vector<float>> object_audio;

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
    // The independent substream's own compr, when it carries one - see
    // DecodedSubstream::compr's own comment; a dependent substream's compre
    // bit means something else entirely, so only the independent (bed)
    // substream's word is ever meaningful at the access-unit level.
    std::optional<std::uint8_t> compr = std::nullopt;
    // The independent substream's own dynrng, same reasoning as compr above -
    // every substream carries its own words and a decoder applies each to
    // that substream's own channels (see Eac3Decoder's DecoderConfig-driven
    // gain), but the bed's is the one figure worth surfacing at the
    // access-unit level for a status report. Only entries below
    // eac3::blocks_per_syncframe(numblkscod) were ever written - see
    // DecodedSubstream::dynrng's own comment on the fixed-size convention.
    std::array<std::uint8_t, kBlocksPerFrame> dynrng{};
    // The independent substream's own numblkscod (§E2.3.1.4), needed to know
    // how many of the kBlocksPerFrame entries in `dynrng` above are real
    // rather than the fixed array's unwritten tail - see
    // eac3::blocks_per_syncframe.
    int numblkscod = 3;
    // The independent substream's own object_metadata/object_audio - see
    // DecodedSubstream's own comments on both. Object audio only ever rides
    // in the bed (this project's own AtmosEncoder never sends a dependent
    // substream at all), so there is nothing to union across substreams the
    // way `layout` does below.
    std::optional<oba::DecodedProgram> object_metadata = std::nullopt;
    std::vector<std::vector<float>> object_audio;
    int substream_count = 0;
    eac3::chanmap::Layout layout;
    std::vector<std::vector<float>> channels;  // parallel to layout, except dual mono
};

class AC3FORGE_EXPORT Eac3Decoder {
   public:
    Eac3Decoder() = default;
    explicit Eac3Decoder(const DecoderConfig& config) : config_(config) {}

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

    // As decode_access_unit, but the rendered program's PCM lands in
    // caller-owned planar storage - FrameDecoder::decode_frame_into's
    // E-AC-3 counterpart, same span contract by assert. channels[slot] is
    // written in the returned layout's slot order (coded order for dual
    // mono), and the returned DecodedAccessUnit carries everything EXCEPT
    // that PCM (its `channels` stays empty; object_audio, which only an
    // Atmos bed carries, stays by value). There must be a span for every
    // slot the assembled layout renders - 16 covers §E3.8.2's cap - and
    // each must hold the unit's blocks*256 samples (kSamplesPerFrame covers
    // every numblkscod). std::nullopt - the §3.7 hold-back - leaves the
    // spans untouched; on an error return their contents are unspecified.
    //
    // What this form removes is the assembly's own allocation (up to 16
    // channels of 1536 samples, every unit) - the term that dominates a
    // stream that never uses transient pre-noise processing. A held-back
    // frame is by definition decoded before the call whose spans would
    // receive it, so its PCM is buffered internally either way and only
    // copied out here at release.
    [[nodiscard]] std::expected<std::optional<DecodedAccessUnit>, DecodeError>
    decode_access_unit_into(std::span<const std::byte> unit,
                            std::span<const std::span<float>> channels);

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
    // Both public access-unit forms above: `external` empty means allocate
    // the program PCM into the returned DecodedAccessUnit, non-empty means
    // write through the spans - the same split decode_frame_core makes.
    [[nodiscard]] std::expected<std::optional<DecodedAccessUnit>, DecodeError>
    decode_access_unit_core(std::span<const std::byte> unit,
                            std::span<const std::span<float>> external);

    DecoderConfig config_{};

    // Keyed by strmtyp and substreamid together: a dependent's id lives in its
    // own numbering space (§E2.3.1.2), so id alone does not identify a
    // substream. At most six coded channels each (3/2 plus LFE).
    std::map<int, std::array<std::array<double, 256>, 6>> delay_;
    // Keyed the same way, one per substream identity that has ever carried
    // JOC: joc::reconstruct's own matrix-ramp and per-object/per-channel
    // overlap-add state, so a moving object's audio and the frame-to-frame
    // matrix interpolation both have real continuity instead of restarting
    // cold every frame - see joc::ReconstructionState's own doc comment.
    std::map<int, joc::ReconstructionState> joc_state_;
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
    // decode_substream's frame-lifetime coefficient buffers - the AHT
    // stream store (§3.4: all six blocks decoded at block 0) and the
    // enhanced-coupling channel store (§3.5.5.1: a block's reconstruction
    // reads its neighbors). Owned here for the same reuse reasoning as the
    // scratch above, with one extra property worth the wordier comment:
    // both used to be heap-allocated and zeroed afresh on every call (98 KB
    // per frame, the two largest per-frame heap costs in the decoder)
    // whether or not the stream used either tool. They are sized lazily at
    // first use instead - a stream using neither tool never allocates them
    // - and every read of a reused buffer is made safe at the write site:
    // an AHT stream's slot is cleared before its block-0 decode fills it
    // (bins past its endmant must read zero), and enhanced-coupling reads
    // are whole-array assignments from this call or gated by this call's
    // ecpl_active flags, so a previous frame's contents are never visible.
    std::vector<std::array<std::array<double, 256>, kBlocksPerFrame>> aht_coeffs_;
    std::vector<std::array<double, 256>> ecpl_all_coeffs_;
    // One entry per block: everything decode_substream's second pass (spx
    // synthesis, rematrixing, IMDCT and PCM write) needs from pass one -
    // the .cpp's comment at the use site explains why two passes exist at
    // all. A member for the same churn reason as the buffers above: the
    // per-block geometry copies (chincpl, spxco, the enhanced-coupling
    // index sets...) land in vectors that keep their capacity across
    // frames, and `coeffs` cycles storage with the parse loop by swap
    // instead of forcing a fresh 14 KB allocation every block. The
    // enhanced-coupling fields are only assigned under cplinu &&
    // ecplinu_now and only read under the same guard - both flags ARE
    // re-assigned every block - so a reused entry's stale conditional
    // fields are never visible.
    struct BlockTail {
        std::vector<std::array<double, 256>> coeffs;  // per stream; decoupled where standard
        std::vector<bool> chincpl;
        bool cplinu = false;
        bool ecplinu_now = false;
        // Standard coupling (valid when cplinu && !ecplinu_now): decoupling
        // already ran inline in pass one, so `coeffs` is final for these
        // channels and nothing further is needed here.
        //
        // Enhanced coupling (valid when cplinu && ecplinu_now):
        int firstchincpl = -1;
        int ecpl_begin_subbnd = 0;
        int ecpl_end_subbnd = 0;
        std::array<bool, eac3::kEcplSubBands> ecpl_structure{};
        std::vector<std::vector<int>> ecplamp_raw;    // [ch][band]
        std::vector<std::vector<int>> ecplangle_raw;  // [ch][band]
        std::vector<std::vector<int>> ecplchaos_raw;  // [ch][band]
        std::vector<bool> ecpltrans;                  // [ch]
        int cplstrtmant = 0;
        int cplendmant = 0;
        // spx (§3.6)
        bool spxinu = false;
        std::vector<bool> chinspx;
        eac3::BandLayout spx_bands{};
        std::vector<std::vector<double>> spxco;
        std::vector<int> spxblnd;
        int spx_startmant = 0;
        int spx_endmant = 0;
        int spx_copystart = 0;
        // rematrixing (§7.5.4, 2/0 only) and block switching
        std::array<bool, 4> rematflg{};
        std::array<bool, eac3::chanmap::kMaxSubstreamFullbw> blksw{};
        // One slot per coded channel plus the shared coupling stream.
        std::array<int, eac3::chanmap::kMaxSubstreamChannels + 1> endmant{};
    };
    std::vector<BlockTail> tails_;
    // §7.3.4 dither (Annex E's dithflag[ch]/dithflage), shared across every
    // substream identity decode_substream ever sees - nothing about §7.3.4
    // requires per-identity separation, only that simultaneous channels'
    // noise stay uncorrelated, which independent draws from one sequential
    // generator already give.
    DitherGenerator dither_{};
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
