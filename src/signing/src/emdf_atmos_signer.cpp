#include "ac3/signing/emdf_atmos_signer.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

#include "ac3/core/bitalloc.hpp"
#include "ac3/core/bitreader.hpp"
#include "ac3/core/exponents.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/emdf/emdf.hpp"
#include "ac3/encoder/eac3_tools.hpp"
#include "hmac_sha256.hpp"

namespace ac3::signing {
namespace {

// Table E2.10 (frmchexpstr code -> six blocks' strategies). 0=reuse,1=D15,2=D25,3=D45.
constexpr std::array<std::array<std::uint8_t, 6>, 32> kE210 = {{
    {1, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 3}, {1, 0, 0, 0, 2, 0}, {1, 0, 0, 0, 3, 3},
    {2, 0, 0, 2, 0, 0}, {2, 0, 0, 2, 0, 3}, {2, 0, 0, 3, 2, 0}, {2, 0, 0, 3, 3, 3},
    {2, 0, 1, 0, 0, 0}, {2, 0, 2, 0, 0, 3}, {2, 0, 2, 0, 2, 0}, {2, 0, 2, 0, 3, 3},
    {2, 0, 3, 2, 0, 0}, {2, 0, 3, 2, 0, 3}, {2, 0, 3, 3, 2, 0}, {2, 0, 3, 3, 3, 3},
    {3, 1, 0, 0, 0, 0}, {3, 1, 0, 0, 0, 3}, {3, 2, 0, 0, 2, 0}, {3, 2, 0, 0, 3, 3},
    {3, 2, 0, 2, 0, 0}, {3, 2, 0, 2, 0, 3}, {3, 2, 0, 3, 2, 0}, {3, 2, 0, 3, 3, 3},
    {3, 3, 1, 0, 0, 0}, {3, 3, 2, 0, 0, 3}, {3, 3, 2, 0, 2, 0}, {3, 3, 2, 0, 3, 3},
    {3, 3, 3, 2, 0, 0}, {3, 3, 3, 2, 0, 3}, {3, 3, 3, 3, 2, 0}, {3, 3, 3, 3, 3, 3},
}};

constexpr std::array<int, 16> kMantBits = {0, 0, 0, 3, 0, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 16};
constexpr int kLfeEndmant = 7;

ExpStrategy exp_strategy(int code) {
    switch (code) {
        case 1: return ExpStrategy::kD15;
        case 2: return ExpStrategy::kD25;
        case 3: return ExpStrategy::kD45;
        default: return ExpStrategy::kReuse;
    }
}

int fullbw_channels(int acmod) {
    static const std::array<int, 8> t = {2, 1, 2, 3, 3, 4, 4, 5};
    return t[static_cast<std::size_t>(acmod)];
}

// A hole is a closed bit range [a,b] excised from A.
struct Hole {
    std::size_t a, b;
};

struct Parsed {
    bool has_container = false;
    std::size_t container_start = 0;        // bit offset of 0x5838
    int container_len = 0;                  // content bytes (after 32-bit header)
    std::size_t container_parsed_bits = 0;  // from container_start
    int prot_primary_code = 0, prot_secondary_code = 0;
    std::vector<Hole> holes;
};

// Walk one syncframe of the ac3forge atmos subset, recording the A-holes and the
// container. Mirrors tools/references/eac3_parse.py for this configuration.
Parsed parse(std::span<const std::byte> frame) {
    Parsed out;
    // Set the moment any per-block field this parser walks turns out to be
    // structurally invalid - see the spx validity check below for why that
    // means "bit tracking already desynced", not "reject this input". Once
    // true, out.has_container is forced false before returning, so a
    // desynced frame is never (mis)signed rather than corrupting the wrong
    // bytes with a tag computed over the wrong bit range.
    bool desynced = false;
    BitReader r{frame};
    const std::size_t total = frame.size() * 8;

    auto put_hole = [&](std::size_t a, std::size_t b) { out.holes.push_back({a, b}); };

    const std::size_t sync = r.read(16);
    (void)sync;
    const int strmtyp = int(r.read(2));
    (void)r.read(3);  // substreamid
    const std::uint32_t frmsiz = r.read(11);
    const int fscod = int(r.read(2));
    const int numblkscod = fscod == 3 ? 3 : int(r.read(2));
    if (fscod == 3) (void)r.read(2);
    const int acmod = int(r.read(3));
    const int lfeon = int(r.read(1));
    (void)r.read(5);  // bsid
    (void)r.read(5);  // dialnorm
    if (r.read(1)) (void)r.read(8);  // compre/compr
    if (acmod == 0) { (void)r.read(5); if (r.read(1)) (void)r.read(8); }
    if (strmtyp == 1) { if (r.read(1)) (void)r.read(16); }
    const int nfchans = fullbw_channels(acmod);
    const int nblks = (numblkscod == 3) ? 6 : (numblkscod + 1);
    assert(strmtyp == 0 && acmod == 7 && lfeon == 1 && numblkscod == 3);

    put_hole(0, 31);  // sync + strmtyp + substreamid + frmsiz

    if (r.read(1)) {  // mixmdate
        if (acmod > 2) (void)r.read(2);
        if ((acmod & 1) && acmod > 2) { (void)r.read(3); (void)r.read(3); }
        if (acmod & 4) { (void)r.read(3); (void)r.read(3); }
        if (lfeon) { if (r.read(1)) (void)r.read(5); }
        if (strmtyp == 0) {
            if (r.read(1)) (void)r.read(6);
            if (acmod == 0) { if (r.read(1)) (void)r.read(6); }
            if (r.read(1)) (void)r.read(6);
            const int mixdef = int(r.read(2));
            if (mixdef == 1) { (void)r.read(1); (void)r.read(1); (void)r.read(3); }
            else if (mixdef == 2) (void)r.read(12);
            else if (mixdef == 3) { int n = int(r.read(5)); (void)r.read((n + 2) * 8); }
            if (acmod < 2) {
                if (r.read(1)) { (void)r.read(8); (void)r.read(6); }
                if (acmod == 0) { if (r.read(1)) { (void)r.read(8); (void)r.read(6); } }
            }
            // frmmixcfginfoe: currently unreachable for this encoder (no
            // AtmosConfig knob ever makes mixmdate itself true at all, so
            // this whole block never executes today), but genuinely absent
            // from this parser before now - eac3_decoder.cpp reads this flag
            // bit first and only then the numblkscod-gated payload below.
            // Cheap correctness fix now rather than a silent one-bit desync
            // waiting for a future AtmosConfig.mixing field.
            if (r.read(1)) {  // frmmixcfginfoe
                if (numblkscod == 0) (void)r.read(5);
                else for (int b = 0; b < nblks; ++b) { if (r.read(1)) (void)r.read(5); }
            }
        }
    }
    {  // infomdate flag: a hole whether set or not (ac3forge sends 0)
        std::size_t p = r.bit_position();
        int info = int(r.read(1));
        put_hole(p, p);  // the flag bit
        if (info) {
            // Not emitted by ac3forge; would need the whole block holed.
            assert(false && "infomdate=1 not handled");
        }
    }
    if (strmtyp == 0 && numblkscod != 3) (void)r.read(1);  // convsync
    // addbsi object-audio extension block -> a hole
    {
        std::size_t p = r.bit_position();
        if (r.read(1)) {  // addbsie
            const int addbsil = int(r.read(6));
            const int first = int(r.read(8));
            if (first & 1) { (void)r.read(8); (void)r.read((addbsil + 1 - 2) * 8); }
            else (void)r.read(addbsil * 8);
        }
        put_hole(p, r.bit_position() - 1);
    }

    // ---- audfrm ----
    int expstre = 1;
    [[maybe_unused]] int ahte = 0;  // only the assert() below reads this; NDEBUG removes it
    if (numblkscod == 3) { expstre = int(r.read(1)); ahte = int(r.read(1)); }
    const int snroffststr = int(r.read(2));
    const int transproce = int(r.read(1));
    const int blkswe = int(r.read(1));
    const int dithflage = int(r.read(1));
    // only the assert() below reads these three; NDEBUG removes it
    [[maybe_unused]] const int bamode = int(r.read(1));
    [[maybe_unused]] const int frmfgaincode = int(r.read(1));
    [[maybe_unused]] const int dbaflde = int(r.read(1));
    int skipflde = 0;
    {  // skipflde flag -> a hole
        std::size_t p = r.bit_position();
        skipflde = int(r.read(1));
        put_hole(p, p);
    }
    const int spxattene = int(r.read(1));
    assert(ahte == 0);

    std::vector<int> cplinu(std::size_t(nblks), 0);
    if (acmod > 1) {
        cplinu[0] = int(r.read(1));
        for (int b = 1; b < nblks; ++b) {
            if (r.read(1)) cplinu[std::size_t(b)] = int(r.read(1));
            else cplinu[std::size_t(b)] = cplinu[std::size_t(b - 1)];
        }
    }
    for (int b = 0; b < nblks; ++b) assert(cplinu[std::size_t(b)] == 0);

    std::array<std::array<int, 5>, 6> chexpstr{};  // [blk][ch]
    if (expstre) {
        // per-block exponent strategy, 2 bits per channel per block
        for (int b = 0; b < nblks; ++b)
            for (int ch = 0; ch < nfchans; ++ch)
                chexpstr[std::size_t(b)][std::size_t(ch)] = int(r.read(2));
    } else {
        // frame-level (Table E2.10): one 5-bit code per channel -> six blocks
        for (int ch = 0; ch < nfchans; ++ch) {
            int code = int(r.read(5));
            for (int b = 0; b < nblks; ++b)
                chexpstr[std::size_t(b)][std::size_t(ch)] = kE210[std::size_t(code)][std::size_t(b)];
        }
    }
    std::array<int, 6> lfeexpstr{};
    if (lfeon)
        for (int b = 0; b < nblks; ++b) lfeexpstr[std::size_t(b)] = int(r.read(1));
    if (strmtyp == 0) {
        int convexpstre = (numblkscod == 3) ? 1 : int(r.read(1));
        if (convexpstre) for (int ch = 0; ch < nfchans; ++ch) (void)r.read(5);
    }
    int frmcsnroffst = 0, frmfsnroffst = 0;
    if (snroffststr == 0) { frmcsnroffst = int(r.read(6)); frmfsnroffst = int(r.read(4)); }
    if (transproce) for (int ch = 0; ch < nfchans; ++ch) { if (r.read(1)) { (void)r.read(10); (void)r.read(8); } }
    if (spxattene) for (int ch = 0; ch < nfchans; ++ch) { if (r.read(1)) (void)r.read(5); }
    if (numblkscod != 0) {
        if (r.read(1)) {  // blkstrtinfoe
            int bl = 0;
            for (std::uint32_t x = frmsiz + 1; x; x >>= 1) ++bl;  // (frmsiz+1).bit_length()
            r.skip(static_cast<std::size_t>((nblks - 1) * (4 + bl)));
        }
    }
    // dbaflde deliberately left out here - it is a real, content-driven case
    // this parser now handles per block (see the audblk loop below), not an
    // assumption to hold at zero. The other three are genuinely fixed
    // constants in this encoder's output (see eac3_frame.cpp), so asserting
    // them is a real invariant check, not a narrowed-scope guard.
    assert(snroffststr == 0 && bamode == 0 && frmfgaincode == 0);

    // ---- audblk x nblks ----
    std::array<std::vector<std::uint8_t>, 5> exps;  // decoded exponents per fbw channel
    std::array<int, 5> endmant{};
    std::vector<std::uint8_t> lfeexps;
    const BitAllocCodes codes{.sdcycod = 2, .fdcycod = 1, .sgaincod = 1,
                              .dbpbcod = 2, .floorcod = 7, .fgaincod = 4};
    const SampleRate sr = SampleRate::k48000;  // fscod 0

    // Spectral extension state, persisting block to block exactly like
    // eac3_decoder.cpp's own spxinu/chinspx/spx_bands do - a later block only
    // resends the strategy (spxstre) if it actually changed, so a block that
    // says "reuse" needs last block's values still in scope. Every helper this
    // reuses (spx_begin_subbnd, group_bands, kDefaultSpxBandStructure, ...) is
    // this project's own clean-room ac3::eac3 code, already linked in via
    // ac3::forge - the same functions eac3_decoder.cpp itself calls to decode
    // real spx content correctly. This block mirrors that decoder's parse
    // exactly, just discarding the coordinate values once their bit width is
    // known, since a hole only needs to know where the bits end.
    bool spxinu = false;
    std::array<bool, 5> chinspx{};
    int spx_band_count = 0;
    int spx_startmant = 0;

    // Delta bit allocation state, persisting block to block like everything
    // else above: deltbae[ch]==0 ("reuse") leaves a channel's segments
    // exactly as they were, so a later block that says reuse needs an
    // earlier block's real segments still in scope, not the empty default.
    // Read but never stored, this parser's independently-recomputed bap
    // (via compute_bit_allocation in tally() below) silently disagreed with
    // whatever bap the real encoder actually used to size its mantissa
    // tokens - matching only on frames where no channel ever had real delta
    // segments (dbaflde present but every block's deltbae[ch] said "no
    // delta"). Content-driven, so the resulting mantissa-count drift showed
    // up only on some frames, at whatever block first carried real segments.
    std::array<DeltaSegments, 5> delta_state{};

    for (int blk = 0; blk < nblks; ++blk) {
        if (blkswe) for (int ch = 0; ch < nfchans; ++ch) (void)r.read(1);
        if (dithflage) for (int ch = 0; ch < nfchans; ++ch) (void)r.read(1);
        if (r.read(1)) (void)r.read(8);  // dynrnge
        if (acmod == 0) { if (r.read(1)) (void)r.read(8); }

        // --- spectral extension strategy + geometry (mirrors eac3_decoder.cpp) ---
        const bool spxstre = (blk == 0) || (r.read(1) != 0);
        if (spxstre) {
            spxinu = r.read(1) != 0;
            if (spxinu) {
                if (acmod != 0) {  // acmod 0 (1/0) is the one mode with no chinspx
                    for (int ch = 0; ch < nfchans; ++ch) chinspx[std::size_t(ch)] = r.read(1) != 0;
                } else {
                    chinspx[0] = true;
                }
                (void)r.read(2);  // spxstrtf
                const int spxbegf = int(r.read(3));
                const int spxendf = int(r.read(3));
                const int begin_subbnd = eac3::spx_begin_subbnd(spxbegf);
                const int end_subbnd = eac3::spx_end_subbnd(spxendf);
                // Same validity check eac3_decoder.cpp itself makes before
                // trusting these - group_bands() asserts subbands is in
                // [1,kMaxSubBands], and std::span::first() below has its own
                // hard precondition on the count, so an invalid combination
                // has to be caught here, not left to crash further down.
                // Reaching this in practice means bit tracking has already
                // desynced somewhere upstream in this frame - not a
                // legitimate encoder output - so give up cleanly on this one
                // frame (desynced=true suppresses signing it, see below)
                // rather than pretend a clamped guess is still correct.
                if (end_subbnd <= begin_subbnd || end_subbnd > eac3::kSpxSubBands) {
                    desynced = true;
                    break;
                }
                const int subband_count = end_subbnd - begin_subbnd;
                std::array<bool, eac3::kSpxSubBands> structure{};
                if (r.read(1)) {  // spxbndstrce
                    for (int i = 1; i < subband_count; ++i)
                        structure[std::size_t(i)] = r.read(1) != 0;
                } else {
                    for (int i = 0; i < subband_count; ++i)
                        structure[std::size_t(i)] =
                            eac3::kDefaultSpxBandStructure[std::size_t(begin_subbnd + i)];
                }
                const auto spx_bands = eac3::group_bands(
                    eac3::spx_band_start(begin_subbnd), subband_count, eac3::kSpxBinsPerSubBand,
                    std::span{structure}.first(std::size_t(subband_count)));
                spx_band_count = spx_bands.count;
                spx_startmant = eac3::spx_band_start(begin_subbnd);
            }
        }
        if (spxinu) {
            for (int ch = 0; ch < nfchans; ++ch) {
                if (!chinspx[std::size_t(ch)]) continue;
                const bool send = (blk == 0) || (r.read(1) != 0);  // spxcoe[ch]
                if (!send) continue;
                (void)r.read(5);  // spxblnd
                (void)r.read(2);  // mstrspxco
                for (int bnd = 0; bnd < spx_band_count; ++bnd) {
                    (void)r.read(4);  // exp
                    (void)r.read(2);  // mant
                }
            }
        }
        // A channel spx has taken over stops carrying its own high band -
        // chbwcod is skipped for it entirely (its bandwidth is fixed by
        // spx_startmant instead), exactly per eac3_decoder.cpp's own
        // "coupled or extended channel" comment. No coupling in this
        // encoder's output (acmod != 2, asserted above), so that half of the
        // same rule is dead code here, kept only for the reason the rest of
        // this parser already carries inert cplinu checks.
        for (int ch = 0; ch < nfchans; ++ch) {
            if (spxinu && chinspx[std::size_t(ch)]) {
                endmant[std::size_t(ch)] = spx_startmant;
            }
        }
        for (int ch = 0; ch < nfchans; ++ch)
            if (chexpstr[std::size_t(blk)][std::size_t(ch)] != 0 &&
                !(spxinu && chinspx[std::size_t(ch)])) {
                int chbwcod = int(r.read(6));
                endmant[std::size_t(ch)] = (chbwcod + 12) * 3 + 37;
            }
        for (int ch = 0; ch < nfchans; ++ch) {
            if (chexpstr[std::size_t(blk)][std::size_t(ch)] == 0) continue;
            ExpStrategy st = exp_strategy(chexpstr[std::size_t(blk)][std::size_t(ch)]);
            int ngrps = exponent_group_count(st, endmant[std::size_t(ch)]);
            std::uint8_t absexp = std::uint8_t(r.read(4));
            std::vector<std::uint8_t> grps(static_cast<std::size_t>(ngrps), std::uint8_t{0});
            for (int g = 0; g < ngrps; ++g) grps[std::size_t(g)] = std::uint8_t(r.read(7));
            (void)r.read(2);  // gainrng
            exps[std::size_t(ch)].assign(std::size_t(endmant[std::size_t(ch)]), 0);
            decode_exponents(absexp, grps, st, exps[std::size_t(ch)]);
        }
        if (lfeon && lfeexpstr[std::size_t(blk)] != 0) {
            std::uint8_t absexp = std::uint8_t(r.read(4));
            std::array<std::uint8_t, 2> grps{std::uint8_t(r.read(7)), std::uint8_t(r.read(7))};
            lfeexps.assign(std::size_t(kLfeEndmant), 0);
            decode_exponents(absexp, grps, ExpStrategy::kD15, lfeexps);
        }
        // bamode==0: default codes; snroffststr==0: frame SNR. baie/snroffste
        // themselves are correctly absent here, not just skipped: this
        // project's own encoder (eac3_frame.cpp) omits both flag bits
        // entirely whenever bamode/snroffststr are 0 at the frame level,
        // rather than writing a flag that always reads false - there is
        // nothing in the bitstream at this point to consume.
        int csnroffst = frmcsnroffst, fsnroffst = frmfsnroffst;
        if (strmtyp == 0) { if (r.read(1)) (void)r.read(10); }  // convsnroffste
        // dbaflde IS a real per-block field once set at the frame level -
        // unlike baie/snroffste above, this one right here was the actual
        // gap: every block sends its own deltbaie bit once dbaflde is set
        // (§5.4.3.47-57), even a block with nothing to say, and this
        // encoder's own delta bit allocation (src/forge/src/core/bitalloc.cpp)
        // sets dbaflde whenever any channel's real spectral energy diverges
        // enough from the default allocation model's estimate to warrant a
        // correction - ordinary, content-driven behavior, not an edge case.
        // No coupling in this encoder's Atmos output (acmod != 2, matching
        // the "no coupling" note elsewhere in this function), so the
        // cpldeltbae branch below is never exercised today, but is walked
        // for the same reason the rest of this parser already carries
        // cplinu checks it currently never takes - correctness if that ever
        // changes, not speculation about it.
        if (dbaflde) {
            const bool cpl_in_use_here = cplinu[std::size_t(blk)] != 0;
            if (r.read(1)) {  // deltbaie
                if (cpl_in_use_here) (void)r.read(2);  // cpldeltbae
                std::array<int, 5> deltbae{};
                for (int ch = 0; ch < nfchans; ++ch) deltbae[std::size_t(ch)] = int(r.read(2));
                auto read_segments = [&r]() {
                    DeltaSegments segs;
                    segs.deltnseg = int(r.read(3)) + 1;
                    for (int seg = 0; seg < segs.deltnseg; ++seg) {
                        segs.deltoffst[std::size_t(seg)] = std::uint8_t(r.read(5));
                        segs.deltlen[std::size_t(seg)] = std::uint8_t(r.read(4));
                        segs.deltba[std::size_t(seg)] = std::uint8_t(r.read(3));
                    }
                    return segs;
                };
                // §5.4.3.47-57: every stream's 2-bit code first, then every
                // stream's segment data - not interleaved per stream.
                if (cpl_in_use_here) { /* cpl segment data would go here if cplinu ever true */ }
                for (int ch = 0; ch < nfchans; ++ch) {
                    // 1 = new info follows (store it); 2 = perform no delta
                    // alloc (clear it); 0 = reuse (leave delta_state[ch]
                    // exactly as an earlier block left it).
                    if (deltbae[std::size_t(ch)] == 1) {
                        delta_state[std::size_t(ch)] = read_segments();
                    } else if (deltbae[std::size_t(ch)] == 2) {
                        delta_state[std::size_t(ch)] = DeltaSegments{};
                    }
                }
            }
        }
        // skiple - only present at all when the frame-level skipflde is set
        // (eac3_frame.cpp: skipflde = metadata.empty() ? 0 : 1, and
        // put_skip_field is only called per block `if (skipflde)`). A
        // bed51-mode frame (no object container, so metadata is empty) has
        // NO skip field syntax anywhere in it - reading a phantom skiple bit
        // there was reading the first bit of real mantissa data instead,
        // desyncing everything after it and, if that phantom bit happened to
        // read 1, scanning up to 511 bytes of real audio mantissas for
        // 0x5838 and potentially writing an HMAC tag into the middle of
        // them on a false match.
        if (skipflde) {
            std::size_t sp = r.bit_position();
            int skiple = int(r.read(1));
            if (skiple) {
                int skipl = int(r.read(9));
                // scan the skip data for the EMDF sync to record the container
                std::size_t data_start = r.bit_position();
                (void)r.read(0);
                // find 0x5838 within [data_start, data_start+skipl*8), never
                // past the frame itself: this loop indexes `frame` directly
                // rather than through `r`, so it does not get BitReader's own
                // overflow()-on-read-past-end protection for free - bound it
                // by hand the same way the container payload-size loop
                // elsewhere in this file already is.
                //
                // Only ever the FIRST match, frame-wide: this encoder embeds
                // its one real container in exactly one skip field per
                // access unit, but a frame can legitimately carry several
                // skiple=1 padding/alignment fields with no container at
                // all - unconstrained 16-bit content in one of those can
                // coincidentally equal 0x5838 by chance. Before this guard,
                // such a false match in a LATER block silently overwrote an
                // already-found real container_start with garbage, which
                // then fed a bogus container_len into the out-of-bounds
                // frame access this was crashing on.
                if (!out.has_container) {
                    const std::size_t scan_limit =
                        std::min(data_start + std::size_t(skipl) * 8, total);
                    for (std::size_t bitp = data_start; bitp + 16 <= scan_limit; ++bitp) {
                        std::uint32_t w = 0;
                        for (int i = 0; i < 16; ++i) {
                            std::size_t q = bitp + std::size_t(i);
                            w = (w << 1) |
                                ((std::to_integer<std::uint32_t>(frame[q >> 3]) >> (7 - (q & 7))) &
                                 1);
                        }
                        if (w == emdf::kSyncWord) {
                            out.has_container = true;
                            out.container_start = bitp;
                            break;
                        }
                    }
                }
                r.skip(std::size_t(skipl) * 8);
                put_hole(sp, r.bit_position() - 1);  // whole skiple+skipl+skipdata
            } else {
                put_hole(sp, sp);  // 1-bit skiple hole
            }
        }
        // mantissas (constant per block for this config, but recompute generally)
        int counts1 = 0, counts2 = 0, counts4 = 0, mant = 0;
        auto tally = [&](std::span<const std::uint8_t> e, int end, int cs, int fs,
                         const DeltaSegments& delta) {
            std::vector<std::uint8_t> bap(static_cast<std::size_t>(end), std::uint8_t{0});
            BitAllocRegion region{};
            region.snr_all_zero = (cs == 0 && fs == 0);
            region.delta = delta;
            compute_bit_allocation(e.subspan(0, std::size_t(end)), sr, codes, cs, fs, bap, region);
            for (std::uint8_t bp : bap) {
                if (bp == 1) ++counts1;
                else if (bp == 2) ++counts2;
                else if (bp == 4) ++counts4;
                else if (bp) mant += kMantBits[bp];
            }
        };
        // LFE has no delta bit allocation field at all (§5.4.3.49/E2.3.2.9's
        // deltbae[ch] loop is bounded by nfchans), so it always gets the
        // empty default rather than anything from delta_state.
        for (int ch = 0; ch < nfchans; ++ch)
            tally(exps[std::size_t(ch)], endmant[std::size_t(ch)], csnroffst, fsnroffst,
                  delta_state[std::size_t(ch)]);
        if (lfeon) tally(lfeexps, kLfeEndmant, csnroffst, fsnroffst, DeltaSegments{});
        mant += 5 * ((counts1 + 2) / 3);
        mant += 7 * ((counts2 + 2) / 3);
        mant += 7 * ((counts4 + 1) / 2);
        r.skip(std::size_t(mant));
    }

    const std::size_t consumed = r.bit_position();
    // aux + crc tail
    put_hole(consumed, total - 18);
    put_hole(total - 17, total - 1);

    // Parse the container to get its length + protection codes.
    if (out.has_container) {
        BitReader cr{frame};
        cr.skip(out.container_start);
        (void)cr.read(16);  // sync
        out.container_len = int(cr.read(16));
        (void)cr.read(2);   // version
        (void)cr.read(3);   // key_id (ac3forge writes 0)
        // Bounded by cr.overflowed(): a misparsed payload_config or a
        // corrupt/unexpected container shape can desync this reader from the
        // real field boundaries, which (before this guard existed) let a
        // garbage `size` value - up to ~2^32 - drive the skip loop below into
        // a multi-billion-iteration, tens-of-seconds hang despite the reader
        // having already run off the end of `frame` (BitReader::read() past
        // end is well-defined - it sets a sticky overflow flag and returns
        // zeros - but the loop count itself was never checked against it).
        // Once overflowed, every further pid/size read is a meaningless
        // zero, so bailing out here is exactly "stop trusting a reader that
        // has left valid data", not a change to how any real payload is
        // interpreted.
        while (!cr.overflowed()) {
            int pid = int(cr.read(5));
            if (pid == 0x1F) { /* escape not used */ }
            if (pid == 0) break;
            // payload config (fixed shape TS 103 420 Table 56, ac3forge)
            (void)cr.read(1);  // smploffste=0
            (void)cr.read(1);  // duratione=0
            if (cr.read(1)) { /* groupide */ // variable_bits(2)
                while (!cr.overflowed()) { (void)cr.read(2); if (!cr.read(1)) break; }
            }
            if (cr.read(1)) (void)cr.read(8);  // codecdatae
            int discard = int(cr.read(1));
            int aligned = 0;
            // smploffste==0
            aligned = int(cr.read(1));
            if (aligned) { (void)cr.read(1); (void)cr.read(1); }
            if (aligned) { (void)cr.read(5); (void)cr.read(2); }
            (void)discard;
            // payload size (variable_bits 8)
            std::uint32_t size = 0;
            while (!cr.overflowed()) {
                size += cr.read(8);
                if (!cr.read(1)) break;
                size <<= 8;
                size += 256;
            }
            for (std::uint32_t i = 0; i < size && !cr.overflowed(); ++i) (void)cr.read(8);
        }
        out.prot_primary_code = int(cr.read(2));
        out.prot_secondary_code = int(cr.read(2));
        auto pbits = [](int code) { return code == 0 ? 0 : code == 1 ? 8 : code == 2 ? 32 : 128; };
        cr.skip(std::size_t(pbits(out.prot_primary_code) + pbits(out.prot_secondary_code)));
        out.container_parsed_bits = cr.bit_position() - out.container_start;
    }
    if (desynced) out.has_container = false;
    return out;
}

int prot_bits(int code) { return (code == 0) ? 0 : (code == 1) ? 8 : (code == 2) ? 32 : 128; }

std::uint16_t crc16(const std::byte* p, std::size_t n) {
    std::uint16_t crc = 0;
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= std::uint16_t(std::to_integer<std::uint32_t>(p[i]) << 8);
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? std::uint16_t((crc << 1) ^ 0x8005) : std::uint16_t(crc << 1);
    }
    return crc;
}

bool bit_at(std::span<const std::byte> f, std::size_t p) {
    // Matches BitReader::read_bit()'s own contract: past the end reads as
    // zero rather than indexing out of bounds. Every caller here already
    // derives its range from parse()'s own container_len/container_start,
    // so this should never actually trip for a well-formed frame - it is
    // the same defense-in-depth as the scan bound just above, for the same
    // reason: this function indexes `f` directly, outside BitReader.
    if ((p >> 3) >= f.size()) return false;
    return (std::to_integer<std::uint32_t>(f[p >> 3]) >> (7 - (p & 7))) & 1;
}

// syncframe size from frmsiz (bits 16..26): (frmsiz+1)*2 bytes. Shared by
// sign_atmos_stream and verify_atmos_stream's otherwise-identical framing
// walk.
std::size_t syncframe_size(std::span<const std::byte> at) {
    const std::uint32_t b2 = std::to_integer<std::uint32_t>(at[2]);
    const std::uint32_t b3 = std::to_integer<std::uint32_t>(at[3]);
    const std::uint32_t frmsiz = ((b2 & 0x7) << 8) | b3;
    return std::size_t(frmsiz + 1) * 2;
}

// Everything sign_atmos_frame and verify_atmos_frame both need: the parsed
// frame, the tag it computes from A||B, and where in the frame that tag
// belongs. Signing writes `digest` into the frame at `prim_off`; verifying
// reads what is already there at `prim_off` and compares. Neither the parse
// nor the HMAC construction differs between the two operations - only what
// happens with the result - so this is the one place that logic lives.
// nullopt means "no container to sign/verify", the same as parse()'s own
// has_container.
struct TagContext {
    Parsed p;
    std::array<std::byte, 32> digest;
    int np;
    std::size_t prim_off;
};

std::optional<TagContext> compute_tag_context(std::span<const std::byte> frame,
                                               const SigningKey& key) {
    Parsed p = parse(frame);
    if (!p.has_container) return std::nullopt;

    // Reconstruct A: excise holes, pack MSB-first, round to nearest 16-bit word.
    const std::size_t total = frame.size() * 8;
    std::vector<std::uint8_t> kept;
    kept.reserve(total);
    std::size_t pos = 0;
    // holes are recorded in walk order, which is ascending; sort defensively.
    auto holes = p.holes;
    std::sort(holes.begin(), holes.end(), [](const Hole& x, const Hole& y) { return x.a < y.a; });
    for (const auto& h : holes) {
        for (std::size_t q = pos; q < h.a; ++q) kept.push_back(bit_at(frame, q) ? 1 : 0);
        if (h.b + 1 > pos) pos = h.b + 1;
    }
    for (std::size_t q = pos; q < total; ++q) kept.push_back(bit_at(frame, q) ? 1 : 0);
    const std::size_t words = (kept.size() + 8) / 16;
    const std::size_t target = words * 16;
    if (target > kept.size()) kept.resize(target, 0);
    else kept.resize(target);
    std::vector<std::uint8_t> a_bytes((kept.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < kept.size(); ++i)
        if (kept[i]) a_bytes[i >> 3] |= std::uint8_t(1u << (7 - (i & 7)));

    // Build B: container content, primary+secondary tag bits zeroed - always,
    // regardless of what those bits currently hold, so verifying reproduces
    // exactly the message signing itself hashed.
    const int np = prot_bits(p.prot_primary_code);
    const int ms = prot_bits(p.prot_secondary_code);
    const std::size_t clen = std::size_t(p.container_len);
    std::vector<std::uint8_t> content(clen * 8);
    for (std::size_t k = 0; k < clen * 8; ++k)
        content[k] = bit_at(frame, p.container_start + 32 + k) ? 1 : 0;
    const std::size_t pb = p.container_parsed_bits;
    for (std::size_t k = (pb - std::size_t(np) - std::size_t(ms) - 32); k < pb - 32; ++k)
        if (k < content.size()) content[k] = 0;
    std::vector<std::uint8_t> b_bytes((content.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < content.size(); ++i)
        if (content[i]) b_bytes[i >> 3] |= std::uint8_t(1u << (7 - (i & 7)));

    // tag = HMAC(key, A||B)[:np/8], with the key supplied by the operator.
    std::vector<std::byte> msg;
    msg.reserve(a_bytes.size() + b_bytes.size());
    for (std::uint8_t x : a_bytes) msg.push_back(std::byte{x});
    for (std::uint8_t x : b_bytes) msg.push_back(std::byte{x});

    // Captured before p is moved into the result below - both are scalars so
    // a moved-from Parsed would actually still carry them intact, but that is
    // not worth relying on here.
    const std::size_t prim_off = p.container_start + pb - std::size_t(np) - std::size_t(ms);
    TagContext ctx{.p = std::move(p),
                   .digest = hmac_sha256(key.bytes(), msg),
                   .np = np,
                   .prim_off = prim_off};
    return ctx;
}

}  // namespace

bool sign_atmos_frame(std::span<std::byte> frame, const SigningKey& key) {
    if (key.empty()) return false;
    const auto ctx = compute_tag_context(frame, key);
    if (!ctx) return false;

    // write protection_bits_primary (np bits) at prim_off. Bounds-checked for
    // the same reason bit_at() is: this indexes `frame` directly. A
    // well-formed match from the single-match-per-frame fix in parse() above
    // should never actually reach the out-of-range branch, but a write past
    // the end would corrupt the wrong memory rather than just read garbage,
    // so this one fails safe by skipping instead of clamping.
    for (int i = 0; i < ctx->np; ++i) {
        std::size_t q = ctx->prim_off + std::size_t(i);
        if ((q >> 3) >= frame.size()) continue;
        const bool bit = (std::to_integer<std::uint32_t>(ctx->digest[std::size_t(i / 8)]) >>
                          (7 - (i & 7))) &
                         1;
        std::byte& byte = frame[q >> 3];
        if (bit)
            byte |= static_cast<std::byte>(1u << (7 - (q & 7)));
        else
            byte &= static_cast<std::byte>(~(1u << (7 - (q & 7))) & 0xFFu);
    }
    // recompute crc2 (last two bytes; covers everything after the 16-bit sync)
    const std::uint16_t c = crc16(frame.data() + 2, frame.size() - 4);
    frame[frame.size() - 2] = std::byte(c >> 8);
    frame[frame.size() - 1] = std::byte(c & 0xFF);
    return true;
}

int sign_atmos_stream(std::span<std::byte> stream, const SigningKey& key) {
    if (key.empty()) return 0;
    int signed_count = 0;
    std::size_t off = 0;
    while (off + 6 <= stream.size()) {
        const std::size_t size = syncframe_size(stream.subspan(off));
        if (off + size > stream.size()) break;
        if (sign_atmos_frame(stream.subspan(off, size), key)) ++signed_count;
        off += size;
    }
    return signed_count;
}

VerifyResult verify_atmos_frame(std::span<const std::byte> frame, const SigningKey& key) {
    const auto ctx = compute_tag_context(frame, key);
    if (!ctx) return VerifyResult::kNoContainer;

    // Compare the digest just computed against whatever tag bits the frame
    // already carries at prim_off - unlike sign_atmos_frame, nothing here is
    // written back.
    for (int i = 0; i < ctx->np; ++i) {
        const std::size_t q = ctx->prim_off + std::size_t(i);
        const bool actual = bit_at(frame, q);
        const bool expected = (std::to_integer<std::uint32_t>(ctx->digest[std::size_t(i / 8)]) >>
                               (7 - (i & 7))) &
                              1;
        if (actual != expected) return VerifyResult::kMismatch;
    }
    return VerifyResult::kValid;
}

VerifySummary verify_atmos_stream(std::span<const std::byte> stream, const SigningKey& key) {
    VerifySummary summary;
    std::size_t off = 0;
    while (off + 6 <= stream.size()) {
        const std::size_t size = syncframe_size(stream.subspan(off));
        if (off + size > stream.size()) break;
        switch (verify_atmos_frame(stream.subspan(off, size), key)) {
            case VerifyResult::kValid: ++summary.valid; break;
            case VerifyResult::kMismatch: ++summary.mismatch; break;
            case VerifyResult::kNoContainer: ++summary.no_container; break;
        }
        off += size;
    }
    return summary;
}

}  // namespace ac3::signing
