#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/core/bitalloc.hpp"  // DeltaSegments
#include "ac3/core/tables.hpp"    // kBlocksPerFrame
#include "ac3/export.hpp"

// Encoder/decoder mirror verification.
//
// An AC-3 encoder carries a model of the decoder it is writing for: the
// exponents that decoder will reconstruct, the bit allocation it will derive
// from them, the delta correction it is holding. Every mantissa field's WIDTH
// comes out of that model, so the moment the two copies disagree the shared
// bit stream stops being parseable - not visibly, and not at the field that
// diverged. Each side keeps reading confidently at its own idea of where it
// is, and the failure surfaces some blocks later as whatever §7.10.2 guard
// the misaligned bits happen to trip first. The bug that motivated this
// (deltbaie == 0 read as "no delta" instead of "keep the previous block's",
// §5.4.3.47) presented as "exponent walks outside 0..24" two blocks
// downstream, in a different channel, and was investigated in the wrong file
// for it.
//
// So rather than adding another guard for another symptom, this compares the
// two models directly. Both sides record what they believed, per block, into
// a FrameTrace; compare() diffs them and names the first block where they
// part company.
//
// The bit offset at each block boundary is the check that does most of the
// work. It is one number per block, it needs no agreement about WHY the sides
// differ, and it catches every desync there is - any disagreement about a
// field's width, whatever caused it, shows up as an offset delta at the next
// block boundary. The per-stream exponent/bap/delta comparison is what turns
// "block 3" into "block 3, channel 1, delta segments", and it runs first at
// the block where the offsets are still equal, which is where the CAUSE lives
// rather than where the damage shows.
//
// Nothing here is on by default: a trace is written only where an
// EncoderConfig/DecoderConfig carries a non-null pointer to one, so an
// ordinary encode pays one null test per block and no allocation at all. See
// ac3/verify/selfcheck.hpp for the encode-then-decode-then-compare driver
// most callers actually want.

namespace ac3::verify {

// One coded stream's state within one block, as that side believed it. Streams
// are numbered exactly as both the encoder and the decoder already number them
// internally: the full-bandwidth channels first, then the LFE, then - when
// coupling is in use - the coupling channel as one more stream.
struct StreamTrace {
    // The DECODED exponents, indexed from bin 0 (a coupling stream's bins
    // below cplstrtmant are kMaxExponent filler on both sides). Not the
    // grouped/coded form: what is being compared is the model, and two sides
    // agreeing on the wire bytes but disagreeing on what they mean is exactly
    // the failure mode of interest.
    std::vector<std::uint8_t> exponents;
    // The bit allocation pointers derived from those exponents, same indexing.
    // This is what sizes every mantissa field, so it is the value whose
    // divergence actually breaks the stream.
    std::vector<std::uint8_t> bap;
    // §7.2.2.6: the delta correction IN FORCE for this block's allocation -
    // not the deltbae code on the wire. The distinction is the whole point:
    // the encoder writing a code whose meaning it has wrong is invisible at
    // the wire level and obvious here. Always default-constructed for the LFE,
    // which has no delta bit allocation field at all (§5.4.3.49).
    DeltaSegments delta;
};

struct BlockTrace {
    // Set when this side reached the block at all. The decoder can stop
    // early - a desynced stream usually gets refused before block 5 - and a
    // trace that simply ends is itself a finding, so absence is recorded
    // rather than inferred.
    bool entered = false;
    // Set when this side got as far as computing the block's bit allocation,
    // i.e. `streams` below is populated. Between `entered` and `allocated`
    // sits the whole of the block's side information, which is where a
    // refusing decoder tends to stop.
    bool allocated = false;
    // Bits consumed/emitted from the start of the syncframe at the moment
    // this block's audblk() begins - BitWriter::bit_count() on the encoder,
    // BitReader::bit_position() on the decoder.
    std::size_t bit_offset = 0;
    // §5.4.3.47's gate bit as written/read. Recorded separately from `delta`
    // above because the two answer different questions: this is what went on
    // the wire, `delta` is what each side did about it.
    bool deltbaie = false;
    std::vector<StreamTrace> streams;
};

// One side's view of one frame.
struct FrameTrace {
    int fbw_channels = 0;    // nfchans
    int coded_channels = 0;  // nfchans + lfe; the coupling stream sits at this index
    std::array<BlockTrace, kBlocksPerFrame> blocks{};

    // Returns the trace to its unvisited state, keeping whatever capacity the
    // vectors already hold so a per-frame loop does not reallocate.
    void reset();
};

enum class Field : std::uint8_t {
    kBlockReached,       // one side stopped before the other
    kBitOffset,          // the localiser: block-boundary bit position
    kStreamCount,        // streams differ, e.g. a disagreement about cplinu
    kDeltbaie,           // §5.4.3.47's gate bit
    kDeltaSegmentCount,  // deltnseg
    kDeltaOffset,        // deltoffst[i]
    kDeltaLength,        // deltlen[i]
    kDeltaValue,         // deltba[i]
    kAllocationReached,  // one side computed a bit allocation for the block, the other did not
    kExponentCount,      // the coded bandwidth itself differs
    kExponent,           // a decoded exponent
    kBapCount,
    kBap,  // a bit allocation pointer - the value that sizes a mantissa field
};

[[nodiscard]] AC3FORGE_EXPORT std::string_view describe(Field field);

struct Mismatch {
    std::uint64_t frame = 0;
    int block = -1;
    int stream = -1;  // -1 for a block-level field
    // A bin index for kExponent/kBap, a segment index for the kDelta* fields,
    // -1 where the field is not indexed.
    int index = -1;
    Field field = Field::kBitOffset;
    long long encoder = 0;
    long long decoder = 0;
};

// "frame 12 block 3 channel 1: bap[87] encoder=5 decoder=4", with the stream
// named the way a reader of A/52 would name it (channel N / LFE / coupling)
// rather than by its internal index.
[[nodiscard]] AC3FORGE_EXPORT std::string describe(const Mismatch& mismatch, int fbw_channels,
                                                   int coded_channels);

// Diffs the two views and returns what they disagree about, most useful first.
//
// Reporting stops at the END of the first block that disagrees. Once the two
// sides are reading different bits, every later block differs as a
// consequence, and a report listing all of them buries the one line that
// names the cause. Within that block the block-level fields come first (the
// bit offset above all), then the per-stream detail.
//
// A single stream's exponent or bap array can disagree in hundreds of places
// at once; at most kMaxPerArray of those are reported per stream per array,
// since the first few plus the block and stream identity is what a reader
// needs and the rest is the same finding repeated.
inline constexpr int kMaxPerArray = 4;

[[nodiscard]] AC3FORGE_EXPORT std::vector<Mismatch> compare(const FrameTrace& encoder,
                                                            const FrameTrace& decoder,
                                                            std::uint64_t frame_index);

// The whole set as one multi-line block, one mismatch per line, for a test
// failure message or a diagnostic dump. Empty string for an empty span.
[[nodiscard]] AC3FORGE_EXPORT std::string report(std::span<const Mismatch> mismatches,
                                                 int fbw_channels, int coded_channels);

}  // namespace ac3::verify
