#include "ac3/encoder/encoder.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

#include "ac3/core/bitalloc.hpp"
#include "ac3/core/bitwriter.hpp"
#include "ac3/core/crc16.hpp"
#include "ac3/core/exponents.hpp"
#include "ac3/core/mantissas.hpp"
#include "ac3/core/mdct.hpp"
#include "ac3/encoder/coupling.hpp"
#include "ac3/encoder/transient.hpp"
#include "ac3/internal/profiling.hpp"

#include "snr_search.hpp"

namespace ac3 {

namespace {

constexpr bool has_three_front(Acmod acmod) {
    const auto value = static_cast<std::uint8_t>(acmod);
    return (value & 0x1) != 0 && acmod != Acmod::k1_0;
}

constexpr bool has_surround(Acmod acmod) {
    return (static_cast<std::uint8_t>(acmod) & 0x4) != 0;
}

// §8.2.8: strategy by the number of blocks an exponent set serves.
constexpr ExpStrategy strategy_for_span(int span) {
    if (span <= 1) {
        return ExpStrategy::kD45;
    }
    if (span <= 3) {
        return ExpStrategy::kD25;
    }
    return ExpStrategy::kD15;
}

// Exponent-set change detection (§8.2.8: "when the variation exceeds a
// threshold, new exponents will be sent").
bool needs_new_exponents(std::span<const std::uint8_t> current,
                         std::span<const std::uint8_t> reference) {
    long long diff = 0;
    for (std::size_t i = 0; i < current.size(); ++i) {
        diff += std::abs(static_cast<int>(current[i]) - static_cast<int>(reference[i]));
    }
    return diff > 2 * static_cast<long long>(current.size());
}

// Where coupling should start when the caller does not say. Sub-band 4 - bin
// 85, 8.0 kHz at 48 kHz - is the floor, because that is roughly where
// per-channel waveform detail stops being what a listener is hearing. The
// band edge rises slowly with the PER-CHANNEL rate, since a channel that can
// afford its own high band should keep it: 5.1 at 448 kbit/s has less to
// spare per channel than stereo at 256 and couples from lower down.
//
// This is a default, not a limit - EncoderConfig::cplbegf overrides it - and
// it is the same curve the E-AC-3 encoder settled on, over the same sub-band
// geometry (§7.4.2 and §E2.2.3 number the coupling bands identically).
int default_cplbegf(std::uint32_t bitrate_kbps, int nfchans) {
    const int per_channel = static_cast<int>(bitrate_kbps) / std::max(nfchans, 1);
    return std::clamp(4 + (per_channel - 48) / 24, 4, 10);
}

// Where coupling should stop. With coupling in use every fbw channel is
// coupled, so chbwcod is not transmitted at all (§5.4.3.8) and cplendf alone
// decides the frame's bandwidth. Following the bandwidth the uncoupled path
// would have chosen keeps coupling a decision about the COST of a band of
// spectrum rather than a decision about how much of it to code - which the
// old fixed 12 (20.3 kHz at any rate) was not: at 96 kbit/s stereo it coded
// 4.5 kHz the uncoupled encoder would have dropped, and paid for the
// coordinates on top, so coupling came out behind.
//
// cplendmant is 37 + 12 * (cplendf + 3), so this rounds DOWN to a sub-band
// edge: coupling never widens the band, only ever leaves a little of it.
int default_cplendf(int chbw_endmant) {
    return std::clamp((chbw_endmant - coupling::kFirstBin) / coupling::kBinsPerSubBand - 3, 0, 15);
}

// §7.5.2: how many rematrixing bands exist, and where the last one stops.
// With coupling active the bands cannot reach above where coupling begins.
int rematrix_band_count(bool cplinu, int cplbegf) {
    if (!cplinu) {
        return 4;
    }
    if (cplbegf > 2) {
        return 4;
    }
    return cplbegf > 0 ? 3 : 2;
}

struct ExponentRun {
    int start_block = 0;
    ExpStrategy strategy = ExpStrategy::kD15;
    EncodedExponents fbw;                  // fbw and LFE channels
    EncodedCouplingExponents cpl;          // the coupling channel
    std::vector<std::uint8_t> decoded;     // the decoder-mirror exponents
    // §7.2.2.6: computed once per run (like `decoded` above) from the real
    // coefficients of every block the run spans, rather than per block - a
    // run already shares one exponent set and one bit allocation across its
    // blocks, so its delta correction is constant across them too.
    DeltaSegments delta;
};

struct StreamPlan {
    std::array<int, kBlocksPerFrame> run_of_block{};
    std::vector<ExponentRun> runs;
};

// Step 9's SNR-offset search result: the composite offset it found, and the
// mantissa bit cost AT that offset (so a caller never has to re-run
// compute_bit_allocation over every stream just to learn what its own search
// already measured on the winning probe).
struct SnrSearchResult {
    int composite = 0;
    std::uint32_t mantissa_bits = 0;
};

}  // namespace

FrameEncoder::FrameEncoder(const EncoderConfig& config) : config_(config) {
    if (config_.drc) {
        range_.emplace(*config_.drc, config_.sample_rate);
    }
    // Ch2's controller is built from drc2/heavy2, never drc/heavy - the two
    // programmes are unrelated, and dialnorm2's existing all-or-nothing rule
    // (§5.4.2.16, checked below in encode_frame) is the precedent for not
    // inheriting one programme's setting into the other's.
    if (config_.acmod == Acmod::kDualMono && config_.drc2) {
        range2_.emplace(*config_.drc2, config_.sample_rate);
    }
    if (config_.heavy) {
        heavy_.emplace(*config_.heavy, config_.sample_rate);
    }
    if (config_.acmod == Acmod::kDualMono && config_.heavy2) {
        heavy2_.emplace(*config_.heavy2, config_.sample_rate);
    }
    const int nfchans = fullbw_channel_count(config_.acmod);
    transient_detectors_.reserve(static_cast<std::size_t>(nfchans));
    for (int i = 0; i < nfchans; ++i) {
        transient_detectors_.emplace_back(config_.sample_rate);
    }
}

std::expected<std::vector<std::byte>, FrameError> FrameEncoder::encode_frame(
    std::span<const std::span<const float>> channels) {
    AC3_ZONE_SCOPED_N("ac3::FrameEncoder::encode_frame");
    const auto index = bitrate_index(config_.bitrate_kbps);
    if (!index) {
        return std::unexpected(FrameError::kInvalidBitrate);
    }
    // fscod2 is an Annex E (E-AC-3) concept; classic AC-3 has no frmsizecod
    // row for a reduced rate.
    if (is_reduced_rate(config_.sample_rate)) {
        return std::unexpected(FrameError::kInvalidBitrate);
    }
    if (config_.dialnorm < 1 || config_.dialnorm > 31) {
        return std::unexpected(FrameError::kInvalidDialnorm);
    }
    const bool dual_mono = config_.acmod == Acmod::kDualMono;
    if (dual_mono &&
        (!config_.dialnorm2 || *config_.dialnorm2 < 1 || *config_.dialnorm2 > 31)) {
        return std::unexpected(FrameError::kInvalidDialnorm);
    }
    const int nfchans = fullbw_channel_count(config_.acmod);
    const int nchans = channel_count();
    assert(static_cast<int>(channels.size()) == nchans);
    for (const auto& channel : channels) {
        assert(channel.size() == kSamplesPerFrame);
        (void)channel;
    }

    // --- 0. Dynamic range metadata (§7.7) ----------------------------------
    // Both words come from the INPUT PCM, before any coding: they describe the
    // programme, not this encoder's output, and a decoder applies them after
    // reconstruction. Doing it here also settles the words before the side
    // information is measured, since a transmitted dynrng costs nine bits.
    // For dual mono, Ch1 and Ch2 are unrelated programmes: each is measured
    // and controlled entirely on its own, never combined the way a real
    // multi-channel layout's channels are (§7.7.2.2 for compr; the same
    // reasoning applies to dynrng, which has no channel-combining rule to
    // begin with once there is no single soundfield to describe a level for).
    AC3_ZONE_BEGIN(zone_metadata, "step0_metadata");
    std::array<std::uint8_t, kBlocksPerFrame> dynrng{};
    dynrng.fill(meta::kDynrngUnity);
    std::array<std::uint8_t, kBlocksPerFrame> dynrng2{};
    dynrng2.fill(meta::kDynrngUnity);
    if (range_) {
        std::array<std::span<const float>, 5> block_view{};
        const int level_chans = dual_mono ? 1 : nfchans;
        for (int block = 0; block < kBlocksPerFrame; ++block) {
            for (int ch = 0; ch < level_chans; ++ch) {
                block_view[static_cast<std::size_t>(ch)] =
                    channels[static_cast<std::size_t>(ch)].subspan(
                        static_cast<std::size_t>(block) * kSamplesPerBlock,
                        kSamplesPerBlock);
            }
            const double level = meta::level_dbfs(
                std::span{block_view}.first(static_cast<std::size_t>(level_chans)));
            dynrng[static_cast<std::size_t>(block)] =
                range_->next(level, config_.dialnorm);
        }
    }
    if (dual_mono && range2_) {
        std::array<std::span<const float>, 1> block_view{};
        for (int block = 0; block < kBlocksPerFrame; ++block) {
            block_view[0] = channels[1].subspan(
                static_cast<std::size_t>(block) * kSamplesPerBlock, kSamplesPerBlock);
            const double level = meta::level_dbfs(std::span{block_view});
            dynrng2[static_cast<std::size_t>(block)] =
                range2_->next(level, *config_.dialnorm2);
        }
    }
    std::uint8_t compr = meta::kComprUnity;
    std::uint8_t compr2 = meta::kComprUnity;
    if (heavy_) {
        // §7.7.2 bounds the MONO DOWNMIX, so that is what gets measured - the
        // loudest single channel is not the constraint, the sum is. history_
        // still holds the previous frame's tail at this point, which is exactly
        // the extra 256 samples this frame's block 0 codes. Dual mono has no
        // downmix at all - §7.7.2.2 says compr bounds Ch1's own signal - so
        // that channel's true peak is measured directly instead.
        const double peak =
            dual_mono
                ? meta::channel_peak_dbfs(std::span{history_[0]}, channels[0])
                : meta::mono_downmix_peak_dbfs(
                      std::span{history_}.first(static_cast<std::size_t>(nfchans)),
                      channels.first(static_cast<std::size_t>(nfchans)), config_.acmod,
                      meta::coefficient(config_.cmixlev),
                      meta::coefficient(config_.surmixlev));
        compr = heavy_->next(peak, config_.dialnorm);
    }
    if (dual_mono && heavy2_) {
        const double peak2 = meta::channel_peak_dbfs(std::span{history_[1]}, channels[1]);
        compr2 = heavy2_->next(peak2, *config_.dialnorm2);
    }
    AC3_ZONE_END(zone_metadata);

    // Bandwidth: explicit config, or a bitrate-aware default. This comes
    // before the coupling decision because coupling inherits it - see
    // default_cplendf.
    //
    // Do not tune this against the checked-in fixtures. Swept 2026-08-17 over
    // chbwcod 24..60 at 192-640 kbit/s on both of them, and narrowing looks
    // like a large win on every metric this repo measures: 5.1 at 448 gains
    // 2.1 dB of SNR at chbwcod 28, and even log-spectral distance improves
    // (5.43 -> 5.26). It is an artifact. chbwcod 28 codes to 14.7 kHz, and
    // reference_51.wav carries 1.1e-4 of its energy above that (it is built
    // from FIR-smoothed noise), so discarding the top 9 kHz costs almost
    // nothing there while freeing bits everywhere else. Real programme
    // material is not band-limited like that, and a 14.7 kHz AC-3 encoder at
    // 448 kbit/s would be plainly worse to listen to while scoring better
    // here.
    //
    // The other direction was measured too, and the current rule is right:
    // forcing full bandwidth (chbwcod 60) is worth -0.004 dB at 448 - the
    // rule already reaches 59 there - and -0.83 dB at 384, where the extra
    // band costs more in quantisation noise than the energy it recovers.
    // Trading bandwidth for precision as the rate falls is what this does,
    // and it is doing it correctly.
    int chbwcod = config_.chbwcod;
    if (chbwcod < 0) {
        const int per_channel_kbps =
            static_cast<int>(config_.bitrate_kbps) / std::max(nfchans, 1);
        chbwcod = std::clamp(per_channel_kbps * 2 / 3, 24, 60);
    }
    assert(chbwcod >= 0 && chbwcod <= 60);
    const int chbw_endmant = ((chbwcod + 12) * 3) + 37;

    // --- Block switching (§8.2.2/§7.9) --------------------------------------
    // Decided before the coupling decision below, because §8.2.4.1's basic-
    // encoder guidance excludes a block-switched channel from coupling, and
    // this codebase's coupling is frame-wide all-or-nothing rather than a
    // per-channel toggle (emit_block_side_info below sends chincpl as
    // unconditionally 1 for every fbw channel) - so the only way to honour
    // that exclusion without inventing bitstream machinery this phase has no
    // room for is to leave coupling off for the WHOLE frame whenever any
    // eligible channel switches, rather than just that one channel.
    AC3_ZONE_BEGIN(zone_transients, "step0_transient_detect");
    std::vector<std::array<bool, kBlocksPerFrame>> blksw(static_cast<std::size_t>(nfchans));
    bool any_switched = false;
    for (int ch = 0; ch < nfchans; ++ch) {
        const auto& pcm = channels[static_cast<std::size_t>(ch)];
        for (int block = 0; block < kBlocksPerFrame; ++block) {
            // §8.2.2 defines blksw from the analysis window's SECOND half -
            // exactly this block period's 256 NEW samples, a contiguous
            // slice of the frame's own PCM. The window's first half was last
            // call's segment; the detector's persistent state carries it, so
            // no history splice (and no 512-sample gather) is needed here at
            // all - see TransientDetector::detect.
            const std::span<const float, kSamplesPerBlock> segment{
                pcm.data() + static_cast<std::size_t>(block) * kSamplesPerBlock,
                kSamplesPerBlock};
            const bool sw = transient_detectors_[static_cast<std::size_t>(ch)].detect(segment);
            blksw[static_cast<std::size_t>(ch)][static_cast<std::size_t>(block)] = sw;
            any_switched = any_switched || sw;
        }
    }
    AC3_ZONE_END(zone_transients);

    // --- Coupling decision -------------------------------------------------
    // Coupling needs at least two full-bandwidth channels to share anything -
    // true of dual mono's nfchans too, but sharing is exactly what its two
    // channels must never do: they are unrelated programmes, and a coupling
    // channel built from their average would leak each into the other. A
    // channel that block-switched anywhere this frame is excluded too - see
    // the block-switching pre-pass above.
    const bool cplinu = config_.coupling && nfchans >= 2 && !dual_mono && !any_switched;
    int cplbegf = 0;
    int cplendf = 0;
    int cplstrtmant = 0;
    int cplendmant = 0;
    int ncplsubnd = 0;
    std::array<bool, coupling::kSubBands> cplbndstrc{};
    coupling::BandLayout cplbands{};
    if (cplinu) {
        cplendf = config_.cplendf >= 0 ? config_.cplendf
                                       : default_cplendf(chbw_endmant);
        cplendf = std::clamp(cplendf, 0, 15);
        // The default start never runs past the end; an explicit one is
        // caught by the sub-band count below.
        cplbegf = config_.cplbegf >= 0
                      ? config_.cplbegf
                      : std::min(default_cplbegf(config_.bitrate_kbps, nfchans),
                                 cplendf + 2);
        cplbegf = std::clamp(cplbegf, 0, 15);
        // cplendf is read by adding 3, so the coded region must extend past
        // where coupling starts.
        if (coupling::sub_band_count(cplbegf, cplendf) < 1) {
            cplendf = std::min(15, cplbegf);
        }
        cplstrtmant = coupling::start_mant(cplbegf);
        cplendmant = std::min(coupling::end_mant(cplendf), 253);
        ncplsubnd = (cplendmant - cplstrtmant) / coupling::kBinsPerSubBand;
        cplbndstrc = coupling::band_structure(cplbegf, ncplsubnd);
        cplbands = coupling::group_bands(cplbegf, ncplsubnd, cplbndstrc);
    }

    // Coupled channels stop at the coupling frequency instead.
    const int fbw_endmant = cplinu ? cplstrtmant : chbw_endmant;

    // Stream layout: the fbw channels, the LFE, then the coupling channel as
    // one more stream carrying the shared high band.
    const int cpl_stream = cplinu ? nchans : -1;
    const int streams = nchans + (cplinu ? 1 : 0);
    const auto stream_start = [&](int s) { return s == cpl_stream ? cplstrtmant : 0; };
    const auto stream_end = [&](int s) {
        if (s == cpl_stream) {
            return cplendmant;
        }
        return s < nfchans ? fbw_endmant : kLfeEndmant;
    };

    // --- Frame size via the CBR accumulator --------------------------------
    const std::uint64_t ideal_bits_num =
        static_cast<std::uint64_t>(config_.bitrate_kbps) * 1000 * kSamplesPerFrame;
    const std::uint64_t denom =
        static_cast<std::uint64_t>(sample_rate_hz(config_.sample_rate)) * 16;
    rate_accumulator_ += ideal_bits_num;
    const std::uint64_t words64 = rate_accumulator_ / denom - words_emitted_;
    words_emitted_ += words64;
    const auto words = static_cast<std::uint32_t>(words64);
    const std::uint32_t base_words =
        *frame_size_words(config_.sample_rate, config_.bitrate_kbps, false);
    assert(words == base_words || words == base_words + 1);
    const bool pad = words != base_words;
    const std::uint32_t total_bytes = words * 2;
    const std::uint32_t total_bits = total_bytes * 8;
    const std::uint32_t words58 = frame_size_58_words(words);
    const BitAllocCodes codes{};  // §8.2.12 basic-encoder defaults

    // --- 1. MDCT per channel per block -------------------------------------
    AC3_ZONE_BEGIN(zone_mdct, "step1_mdct");
    std::vector<std::array<double, 256>> coeffs(
        static_cast<std::size_t>(streams) * kBlocksPerFrame);
    const auto coeffs_at = [&](int s, int block) -> std::array<double, 256>& {
        return coeffs[static_cast<std::size_t>(s) * kBlocksPerFrame +
                      static_cast<std::size_t>(block)];
    };
    for (int ch = 0; ch < nchans; ++ch) {
        for (int block = 0; block < kBlocksPerFrame; ++block) {
            auto& time = time_scratch_;
            AC3_ZONE_BEGIN(zone_gather, "step1_gather");
            for (int n = 0; n < 512; ++n) {
                const int pos = block * 256 - 256 + n;
                time[static_cast<std::size_t>(n)] =
                    pos < 0 ? history_[static_cast<std::size_t>(ch)]
                                      [static_cast<std::size_t>(pos + 256)]
                            : static_cast<double>(
                                  channels[static_cast<std::size_t>(ch)]
                                          [static_cast<std::size_t>(pos)]);
            }
            AC3_ZONE_END(zone_gather);
            auto& windowed = windowed_scratch_;
            AC3_ZONE_BEGIN(zone_window, "step1_window");
            apply_analysis_window(time, windowed);
            AC3_ZONE_END(zone_window);
            if (ch < nfchans && blksw[static_cast<std::size_t>(ch)][static_cast<std::size_t>(block)]) {
                // §7.9.2: the two half-block transforms are interleaved
                // bin-by-bin into one ordinary 256-coefficient set - from
                // here on, exponent/bitalloc/mantissa code cannot tell this
                // block apart from a long one.
                const std::span<const double, 512> full(windowed);
                auto& first = half1_scratch_;
                auto& second = half2_scratch_;
                mdct256_forward_first(full.first<256>(), first, config_.fast_mdct);
                mdct256_forward_second(full.last<256>(), second, config_.fast_mdct);
                auto& out = coeffs_at(ch, block);
                for (int k = 0; k < 128; ++k) {
                    out[static_cast<std::size_t>(2 * k)] = first[static_cast<std::size_t>(k)];
                    out[static_cast<std::size_t>(2 * k + 1)] = second[static_cast<std::size_t>(k)];
                }
            } else {
                mdct512_forward(windowed, coeffs_at(ch, block), config_.fast_mdct);
            }
        }
        for (int n = 0; n < 256; ++n) {
            history_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)] =
                static_cast<double>(
                    channels[static_cast<std::size_t>(ch)][static_cast<std::size_t>(1280 + n)]);
        }
    }
    AC3_ZONE_END(zone_mdct);

    // --- 2. Coupling: form the shared channel and its coordinates ----------
    // Coordinates are sent in blocks 0, 2 and 4 and reused in between
    // (§8.2.4.1); the coupling channel itself is the plain average of the
    // coupled channels the spec's basic encoder describes (§7.4.1), with the
    // decoder's x8 living entirely in the coordinates. One coordinate per
    // BAND, which is one or more sub-bands joined by cplbndstrc.
    std::array<bool, kBlocksPerFrame> send_coords{};
    std::vector<int> master(static_cast<std::size_t>(kBlocksPerFrame) *
                            static_cast<std::size_t>(std::max(nfchans, 1)));
    std::vector<coupling::Coordinate> coords(
        static_cast<std::size_t>(kBlocksPerFrame) * static_cast<std::size_t>(std::max(nfchans, 1)) *
        static_cast<std::size_t>(std::max(cplbands.count, 1)));
    const auto coord_at = [&](int block, int ch, int bnd) -> coupling::Coordinate& {
        return coords[(static_cast<std::size_t>(block) * static_cast<std::size_t>(nfchans) +
                       static_cast<std::size_t>(ch)) *
                          static_cast<std::size_t>(cplbands.count) +
                      static_cast<std::size_t>(bnd)];
    };
    const auto master_at = [&](int block, int ch) -> int& {
        return master[static_cast<std::size_t>(block) * static_cast<std::size_t>(nfchans) +
                      static_cast<std::size_t>(ch)];
    };

    if (cplinu) {
        AC3_ZONE_SCOPED_N("step2_coupling");
        std::vector<double> values(static_cast<std::size_t>(cplbands.count));

        // The decoder computes
        //     channel = coupling * coordinate * 8,
        // so storing coupling = sum / K makes the required coordinate r*K/8,
        // where r = sqrt(E_ch / E_sum) is the band's magnitude ratio. K is
        // never transmitted - it is folded into the coordinates - which makes
        // it look like a free parameter. It is not, in two separate ways, and
        // this encoder measured both of them the hard way.
        //
        // Scaling the shared channel UP - normalising each band, or the whole
        // coupled region, to unit peak - is tempting because it makes every
        // coordinate small and so unclampable. But §7.2.2 reads psd
        // ABSOLUTELY, against a fixed hearing threshold: a coupling channel
        // normalised to full scale is simply the loudest thing in the frame,
        // and the allocator buys it bits to match. Measured at 128 kbit/s
        // stereo, that handed the coupling channel 291 of a block's 420
        // mantissa bits - more per bin than the baseband it was supposed to
        // be subsidising - and dropped the frame's coarse SNR offset from 27
        // to 11. Coupling made the encoder run out of bits SOONER than not
        // coupling at all, while still producing frames that pass every size
        // and CRC check.
        //
        // K must also be constant across the whole frame, not per block.
        // Coordinates go out in blocks 0, 2 and 4 and are reused in 1, 3 and
        // 5, so a K carrying anything block-specific reaches the decoder
        // multiplied by the PREVIOUS block's value: the reusing blocks come
        // back wrong by the ratio of the two blocks' scales.
        //
        // §7.4.1's own answer satisfies both: the coupling channel is the
        // average of the coupled channels, K = nfchans, so the shared channel
        // sits at the natural level of one real channel - which is the level
        // the allocator's model expects - and every block shares one scale.
        const double scale = static_cast<double>(nfchans);

        for (int block = 0; block < kBlocksPerFrame; ++block) {
            send_coords[static_cast<std::size_t>(block)] = block % 2 == 0;

            auto& cpl = coeffs_at(cpl_stream, block);
            cpl.fill(0.0);
            // The raw sum for now; the division by `scale` comes after the
            // coordinates, which are measured against that same raw sum.
            for (int bin = cplstrtmant; bin < cplendmant; ++bin) {
                double sum = 0.0;
                for (int ch = 0; ch < nfchans; ++ch) {
                    sum += coeffs_at(ch, block)[static_cast<std::size_t>(bin)];
                }
                cpl[static_cast<std::size_t>(bin)] = sum;
            }

            for (int ch = 0; ch < nfchans; ++ch) {
                for (int bnd = 0; bnd < cplbands.count; ++bnd) {
                    const int low = cplbands.start[static_cast<std::size_t>(bnd)];
                    const int high =
                        std::min(low + cplbands.size[static_cast<std::size_t>(bnd)], cplendmant);
                    double power_ch = 0.0;
                    double power_sum = 0.0;
                    for (int bin = low; bin < high; ++bin) {
                        const double value =
                            coeffs_at(ch, block)[static_cast<std::size_t>(bin)];
                        const double summed = cpl[static_cast<std::size_t>(bin)];
                        power_ch += value * value;
                        power_sum += summed * summed;
                    }
                    const double ratio =
                        power_sum > 0.0 ? std::sqrt(power_ch / power_sum) : 0.0;
                    values[static_cast<std::size_t>(bnd)] = ratio * scale / 8.0;
                }
                const int chosen = coupling::choose_master(values);
                master_at(block, ch) = chosen;
                for (int bnd = 0; bnd < cplbands.count; ++bnd) {
                    coord_at(block, ch, bnd) = coupling::quantize_coordinate(
                        values[static_cast<std::size_t>(bnd)], chosen);
                }
                // Above the coupling frequency the channel carries nothing of
                // its own any more.
                for (int bin = cplstrtmant; bin < 256; ++bin) {
                    coeffs_at(ch, block)[static_cast<std::size_t>(bin)] = 0.0;
                }
            }
            // Blocks that reuse coordinates must reuse the ones actually
            // transmitted, or encoder and decoder diverge.
            if (!send_coords[static_cast<std::size_t>(block)]) {
                for (int ch = 0; ch < nfchans; ++ch) {
                    master_at(block, ch) = master_at(block - 1, ch);
                    for (int bnd = 0; bnd < cplbands.count; ++bnd) {
                        coord_at(block, ch, bnd) = coord_at(block - 1, ch, bnd);
                    }
                }
            }
            for (int bin = cplstrtmant; bin < cplendmant; ++bin) {
                cpl[static_cast<std::size_t>(bin)] /= scale;
            }
        }
    }

    // --- 3. Rematrixing (2/0 only, §7.5.3) ---------------------------------
    std::array<std::array<bool, 4>, kBlocksPerFrame> rematflg{};
    const bool rematrixing = config_.acmod == Acmod::k2_0;
    const int nrematbd = rematrix_band_count(cplinu, cplbegf);
    if (rematrixing) {
        AC3_ZONE_SCOPED_N("step3_rematrix");
        for (int block = 0; block < kBlocksPerFrame; ++block) {
            auto& left = coeffs_at(0, block);
            auto& right = coeffs_at(1, block);
            for (int band = 0; band < nrematbd; ++band) {
                const int low = kRematrixBands[static_cast<std::size_t>(band)][0];
                int high = kRematrixBands[static_cast<std::size_t>(band)][1];
                high = std::min(high, fbw_endmant - 1);
                if (low > high) {
                    continue;
                }
                double power_l = 0.0;
                double power_r = 0.0;
                double power_sum = 0.0;
                double power_diff = 0.0;
                for (int bin = low; bin <= high; ++bin) {
                    const double l = left[static_cast<std::size_t>(bin)];
                    const double r = right[static_cast<std::size_t>(bin)];
                    power_l += l * l;
                    power_r += r * r;
                    power_sum += (l + r) * (l + r);
                    power_diff += (l - r) * (l - r);
                }
                if (std::min(power_sum, power_diff) < std::min(power_l, power_r)) {
                    rematflg[static_cast<std::size_t>(block)][static_cast<std::size_t>(band)] =
                        true;
                    for (int bin = low; bin <= high; ++bin) {
                        const double l = left[static_cast<std::size_t>(bin)];
                        const double r = right[static_cast<std::size_t>(bin)];
                        left[static_cast<std::size_t>(bin)] = 0.5 * (l + r);
                        right[static_cast<std::size_t>(bin)] = 0.5 * (l - r);
                    }
                }
            }
        }
    }

    // --- 4. Fixed point + per-block raw exponents --------------------------
    AC3_ZONE_BEGIN(zone_fixed, "step4_fixed_exponents");
    std::vector<std::int32_t> fixed;
    {
        // One reservation instead of push_back growth across ~10k bins - the
        // exact total is knowable up front, and the phase-5 Tracy zones put
        // this stage second only to transient detection in the former
        // unzoned remainder.
        std::size_t total = 0;
        for (int s = 0; s < streams; ++s) {
            total += static_cast<std::size_t>(stream_end(s) - stream_start(s)) *
                     kBlocksPerFrame;
        }
        fixed.reserve(total);
    }
    std::vector<std::size_t> fixed_base(static_cast<std::size_t>(streams) * kBlocksPerFrame);
    std::vector<std::vector<std::uint8_t>> block_exps(
        static_cast<std::size_t>(streams) * kBlocksPerFrame);
    for (int s = 0; s < streams; ++s) {
        const int begin = stream_start(s);
        const int end = stream_end(s);
        for (int block = 0; block < kBlocksPerFrame; ++block) {
            const auto slot = static_cast<std::size_t>(s) * kBlocksPerFrame +
                              static_cast<std::size_t>(block);
            fixed_base[slot] = fixed.size();
            block_exps[slot].resize(static_cast<std::size_t>(end - begin));
            for (int bin = begin; bin < end; ++bin) {
                const std::int32_t f =
                    to_fixed25(coeffs_at(s, block)[static_cast<std::size_t>(bin)]);
                fixed.push_back(f);
                block_exps[slot][static_cast<std::size_t>(bin - begin)] =
                    static_cast<std::uint8_t>(exponent_from_fixed(f));
            }
        }
    }
    AC3_ZONE_END(zone_fixed);
    // Indexed from the stream's own start bin.
    const auto fixed_at = [&](int s, int block, int offset) {
        return fixed[fixed_base[static_cast<std::size_t>(s) * kBlocksPerFrame +
                                static_cast<std::size_t>(block)] +
                     static_cast<std::size_t>(offset)];
    };

    // --- 5. Exponent strategy plan per stream (§8.2.8) ---------------------
    AC3_ZONE_BEGIN(zone_strategy, "step5_exp_strategy");
    std::vector<StreamPlan> plan(static_cast<std::size_t>(streams));
    for (int s = 0; s < streams; ++s) {
        auto& p = plan[static_cast<std::size_t>(s)];
        const bool is_lfe = s < nchans && s >= nfchans;
        const bool is_cpl = s == cpl_stream;
        const int begin = stream_start(s);
        const int end = stream_end(s);

        std::vector<int> starts{0};
        if (!is_lfe) {
            const auto* reference = &block_exps[static_cast<std::size_t>(s) * kBlocksPerFrame];
            for (int block = 1; block < kBlocksPerFrame; ++block) {
                const auto& current = block_exps[static_cast<std::size_t>(s) * kBlocksPerFrame +
                                                 static_cast<std::size_t>(block)];
                // §7.9's block-switched block is isolated into its own
                // single-block run on both sides - entering forces a
                // boundary here, leaving forces one at the next block -
                // which strategy_for_span(1) below then resolves to D45
                // automatically, matching §8.2.2's "a channel that is
                // block-switched uses the D45 exponent strategy."
                const bool switch_boundary =
                    s < nfchans &&
                    (blksw[static_cast<std::size_t>(s)][static_cast<std::size_t>(block)] ||
                     blksw[static_cast<std::size_t>(s)][static_cast<std::size_t>(block - 1)]);
                if (needs_new_exponents(current, *reference) || switch_boundary) {
                    starts.push_back(block);
                    reference = &current;
                }
            }
        }
        starts.push_back(kBlocksPerFrame);

        for (std::size_t run = 0; run + 1 < starts.size(); ++run) {
            const int first = starts[run];
            const int last = starts[run + 1];
            // The coupling channel's group count must divide its bin count
            // exactly, which only D15 guarantees for every sub-band count.
            const auto strategy = (is_lfe || is_cpl) ? ExpStrategy::kD15
                                                     : strategy_for_span(last - first);

            std::vector<std::uint8_t> raw(
                block_exps[static_cast<std::size_t>(s) * kBlocksPerFrame +
                           static_cast<std::size_t>(first)]);
            for (int block = first + 1; block < last; ++block) {
                const auto& other = block_exps[static_cast<std::size_t>(s) * kBlocksPerFrame +
                                               static_cast<std::size_t>(block)];
                for (std::size_t i = 0; i < raw.size(); ++i) {
                    raw[i] = std::min(raw[i], other[i]);
                }
            }

            ExponentRun entry;
            entry.start_block = first;
            entry.strategy = strategy;
            // Exponents are indexed from bin 0 for the allocator's sake, so
            // a coupling run leaves its low bins untouched.
            entry.decoded.assign(static_cast<std::size_t>(end), kMaxExponent);
            if (is_cpl) {
                entry.cpl = encode_coupling_exponents(raw, strategy);
                decode_coupling_exponents(
                    entry.cpl.cplabsexp, entry.cpl.groups, strategy,
                    std::span{entry.decoded}.subspan(static_cast<std::size_t>(begin)));
            } else {
                entry.fbw = encode_exponents(raw, strategy);
                decode_exponents(entry.fbw.absolute, entry.fbw.groups, strategy, entry.decoded);
            }
            // §7.2.2.6: compare this run's shared exponent-derived masking
            // curve against one built from the real coefficients. `raw`
            // above (and hence this run's exponents) is the MIN exponent
            // across the run's blocks per bin, i.e. driven by whichever
            // block has the LARGEST magnitude there - so the comparison
            // needs that same per-bin max, not an average, or it would
            // measure the (intentional) gap between "loudest block" and
            // "typical block" instead of real quantization error and bias
            // toward spurious cuts on any run spanning more than one block.
            // LFE is excluded: §7.2.2.6 states plainly that "the delta bit
            // allocation option is available for each fbw channel and the
            // coupling channel" - LFE is not in that list. §5.4.3.49 confirms
            // it from the syntax side: deltbae[ch] is described as "per full
            // bandwidth channel", and audblk()'s own `for (ch = 0; ch <
            // nfchans; ch++) {deltbae[ch]}` loop never reaches the LFE slot -
            // there is no bitstream field to carry an LFE delta at all, so
            // computing one here would just diverge from what no decoder
            // could ever receive.
            //
            // The coupling channel and every fbw channel - even in a frame
            // where coupling is active - ARE in §7.2.2.6's scope, so both are
            // eligible below. `coeffs_at(cpl_stream, ...)` at this point is
            // already the §7.4.1 average of the coupled channels, divided
            // back down to their natural level (step 2 above), so the
            // real-vs-quantized-psd comparison this drives is exactly as
            // meaningful for it as for a real recorded fbw channel. The extra
            // side-info cost this can add is bounded generically further
            // below (§7.2.2.6's own scope note, step 8): delta is a pure
            // quality refinement that gets cleared and re-measured, for every
            // stream, if it would make an otherwise-fittable frame fail to
            // fit - so there is no need to withhold it here pre-emptively
            // just because coupling happens to be on this frame.
            if (!is_lfe) {
                std::vector<double> peak_mag(static_cast<std::size_t>(end), 0.0);
                for (int block = first; block < last; ++block) {
                    const auto& c = coeffs_at(s, block);
                    for (int bin = begin; bin < end; ++bin) {
                        peak_mag[static_cast<std::size_t>(bin)] =
                            std::max(peak_mag[static_cast<std::size_t>(bin)],
                                    std::abs(c[static_cast<std::size_t>(bin)]));
                    }
                }
                entry.delta = choose_delta_segments(peak_mag, entry.decoded, begin);
            }
            for (int block = first; block < last; ++block) {
                p.run_of_block[static_cast<std::size_t>(block)] = static_cast<int>(run);
            }
            p.runs.push_back(std::move(entry));
        }
    }
    AC3_ZONE_END(zone_strategy);

    // --- 6. Coupling leak seeds --------------------------------------------
    // The transmitted leaks continue the masking decay across the coupling
    // boundary; derive them from the coupling channel's own first band so the
    // allocator starts from a sensible level rather than a fixed guess.
    int cplfleak = 0;
    int cplsleak = 0;
    if (cplinu) {
        const auto& first_run = plan[static_cast<std::size_t>(cpl_stream)].runs.front();
        const int exp = first_run.decoded[static_cast<std::size_t>(cplstrtmant)];
        const int psd = 3072 - (exp << 7);
        cplfleak = std::clamp((psd - fast_gain(codes.fgaincod) - 768) >> 8, 0, 7);
        cplsleak = std::clamp((psd - slow_gain(codes.sgaincod) - 768) >> 8, 0, 7);
    }

    // --- 7. The block emitter ----------------------------------------------
    // One function writes a block's side information; the bit budget is
    // measured by running it into a throwaway writer rather than maintaining
    // a parallel formula that every new field could silently invalidate.
    std::vector<std::vector<std::uint8_t>> stream_bap(static_cast<std::size_t>(streams));
    int csnroffst = 0;
    int fsnroffst = 0;

    const auto emit_block_side_info = [&](BitWriter& w, int block) {
        const bool first = block == 0;
        for (int ch = 0; ch < nfchans; ++ch) {
            w.put(blksw[static_cast<std::size_t>(ch)][static_cast<std::size_t>(block)] ? 1 : 0,
                  1);  // blksw
        }
        for (int ch = 0; ch < nfchans; ++ch) {
            w.put(0, 1);  // dithflag
        }
        // §7.7.1.2: an absent word means "keep the previous BLOCK's", so only a
        // change needs sending. Block 0 inherits nothing - absence there is
        // defined as unity, not as the previous frame's value, which is what
        // lets a decoder join a stream mid-programme without applying a gain it
        // never received. Block 0 is sent unconditionally even when it happens
        // to be unity: skipping it would be legal and one byte smaller, but a
        // frame that states its own starting gain is easier to reason about
        // from a capture.
        const bool send_dynrng =
            config_.drc.has_value() &&
            (first || dynrng[static_cast<std::size_t>(block)] !=
                          dynrng[static_cast<std::size_t>(block) - 1]);
        w.put(send_dynrng ? 1 : 0, 1);  // dynrnge
        if (send_dynrng) {
            w.put(dynrng[static_cast<std::size_t>(block)], 8);
        }
        if (config_.acmod == Acmod::kDualMono) {
            const bool send_dynrng2 =
                config_.drc.has_value() &&
                (first || dynrng2[static_cast<std::size_t>(block)] !=
                              dynrng2[static_cast<std::size_t>(block) - 1]);
            w.put(send_dynrng2 ? 1 : 0, 1);  // dynrng2e
            if (send_dynrng2) {
                w.put(dynrng2[static_cast<std::size_t>(block)], 8);
            }
        }

        w.put(first ? 1 : 0, 1);  // cplstre
        if (first) {
            w.put(cplinu ? 1 : 0, 1);
            if (cplinu) {
                for (int ch = 0; ch < nfchans; ++ch) {
                    w.put(1, 1);  // chincpl: every fbw channel is coupled
                }
                if (config_.acmod == Acmod::k2_0) {
                    w.put(0, 1);  // phsflginu: no phase restoration
                }
                w.put(static_cast<std::uint32_t>(cplbegf), 4);
                w.put(static_cast<std::uint32_t>(cplendf), 4);
                // cplbndstrc, one bit per sub-band after the first. AC-3
                // always sends it, so the ncplsubnd - 1 bits are spent
                // whatever the structure - what the structure buys back is
                // 8 bits per band it removes, three times a frame per channel.
                for (int bnd = 1; bnd < ncplsubnd; ++bnd) {
                    w.put(cplbndstrc[static_cast<std::size_t>(bnd)] ? 1 : 0, 1);
                }
            }
        }
        if (cplinu) {
            for (int ch = 0; ch < nfchans; ++ch) {
                const bool send = send_coords[static_cast<std::size_t>(block)];
                w.put(send ? 1 : 0, 1);  // cplcoe
                if (send) {
                    w.put(static_cast<std::uint32_t>(master_at(block, ch)), 2);
                    for (int bnd = 0; bnd < cplbands.count; ++bnd) {
                        const auto coordinate = coord_at(block, ch, bnd);
                        w.put(coordinate.exp, 4);
                        w.put(coordinate.mant, 4);
                    }
                }
            }
            // phsflginu is 0, so no phase flags follow.
        }

        if (rematrixing) {
            const bool send = first || rematflg[static_cast<std::size_t>(block)] !=
                                           rematflg[static_cast<std::size_t>(block) - 1];
            w.put(send ? 1 : 0, 1);  // rematstr
            if (send) {
                for (int band = 0; band < nrematbd; ++band) {
                    w.put(rematflg[static_cast<std::size_t>(block)]
                                  [static_cast<std::size_t>(band)]
                              ? 1
                              : 0,
                          1);
                }
            }
        }

        // Exponent strategies: coupling first, then fbw, then LFE.
        const auto fresh = [&](int s) {
            const auto& p = plan[static_cast<std::size_t>(s)];
            const int run = p.run_of_block[static_cast<std::size_t>(block)];
            return p.runs[static_cast<std::size_t>(run)].start_block == block;
        };
        if (cplinu) {
            w.put(static_cast<std::uint32_t>(fresh(cpl_stream) ? ExpStrategy::kD15
                                                               : ExpStrategy::kReuse),
                  2);
        }
        for (int ch = 0; ch < nfchans; ++ch) {
            const auto& p = plan[static_cast<std::size_t>(ch)];
            const int run = p.run_of_block[static_cast<std::size_t>(block)];
            w.put(static_cast<std::uint32_t>(
                      fresh(ch) ? p.runs[static_cast<std::size_t>(run)].strategy
                                : ExpStrategy::kReuse),
                  2);
        }
        if (config_.lfe) {
            w.put(fresh(nfchans) ? 1 : 0, 1);  // lfeexpstr
        }
        // chbwcod exists only for channels NOT in coupling.
        if (!cplinu) {
            for (int ch = 0; ch < nfchans; ++ch) {
                if (fresh(ch)) {
                    w.put(static_cast<std::uint32_t>(chbwcod), 6);
                }
            }
        }

        // Exponents, same order.
        if (cplinu && fresh(cpl_stream)) {
            const auto& p = plan[static_cast<std::size_t>(cpl_stream)];
            const auto& run = p.runs[static_cast<std::size_t>(
                p.run_of_block[static_cast<std::size_t>(block)])];
            w.put(run.cpl.cplabsexp, 4);
            for (const auto group : run.cpl.groups) {
                w.put(group, 7);
            }
        }
        for (int ch = 0; ch < nfchans; ++ch) {
            if (!fresh(ch)) {
                continue;
            }
            const auto& p = plan[static_cast<std::size_t>(ch)];
            const auto& run = p.runs[static_cast<std::size_t>(
                p.run_of_block[static_cast<std::size_t>(block)])];
            w.put(run.fbw.absolute, 4);
            for (const auto group : run.fbw.groups) {
                w.put(group, 7);
            }
            w.put(0, 2);  // gainrng
        }
        if (config_.lfe && fresh(nfchans)) {
            const auto& p = plan[static_cast<std::size_t>(nfchans)];
            const auto& run = p.runs[static_cast<std::size_t>(
                p.run_of_block[static_cast<std::size_t>(block)])];
            w.put(run.fbw.absolute, 4);
            for (const auto group : run.fbw.groups) {
                w.put(group, 7);
            }
        }

        w.put(first ? 1 : 0, 1);  // baie
        if (first) {
            w.put(static_cast<std::uint32_t>(codes.sdcycod), 2);
            w.put(static_cast<std::uint32_t>(codes.fdcycod), 2);
            w.put(static_cast<std::uint32_t>(codes.sgaincod), 2);
            w.put(static_cast<std::uint32_t>(codes.dbpbcod), 2);
            w.put(static_cast<std::uint32_t>(codes.floorcod), 3);
        }
        w.put(first ? 1 : 0, 1);  // snroffste
        if (first) {
            w.put(static_cast<std::uint32_t>(csnroffst), 6);
            if (cplinu) {
                w.put(static_cast<std::uint32_t>(fsnroffst), 4);       // cplfsnroffst
                w.put(static_cast<std::uint32_t>(codes.fgaincod), 3);  // cplfgaincod
            }
            for (int ch = 0; ch < nfchans; ++ch) {
                w.put(static_cast<std::uint32_t>(fsnroffst), 4);
                w.put(static_cast<std::uint32_t>(codes.fgaincod), 3);
            }
            if (config_.lfe) {
                w.put(static_cast<std::uint32_t>(fsnroffst), 4);
                w.put(static_cast<std::uint32_t>(codes.fgaincod), 3);
            }
        }
        if (cplinu) {
            w.put(first ? 1 : 0, 1);  // cplleake
            if (first) {
                w.put(static_cast<std::uint32_t>(cplfleak), 3);
                w.put(static_cast<std::uint32_t>(cplsleak), 3);
            }
        }
        // §5.4.3.47-57: this encoder never reuses ('00') a previous block's
        // delta state - it always resends fresh ('01') when a run wants a
        // correction, or says '10' (no delta) otherwise - so deltbaie itself
        // only needs to be 1 when at least one stream has something to say
        // this block.
        const auto stream_delta = [&](int s) -> const DeltaSegments& {
            const auto& p = plan[static_cast<std::size_t>(s)];
            return p.runs[static_cast<std::size_t>(
                              p.run_of_block[static_cast<std::size_t>(block)])]
                .delta;
        };
        bool any_delta = cplinu && stream_delta(cpl_stream).deltnseg > 0;
        for (int ch = 0; ch < nfchans && !any_delta; ++ch) {
            any_delta = stream_delta(ch).deltnseg > 0;
        }
        w.put(any_delta ? 1 : 0, 1);  // deltbaie
        if (any_delta) {
            // §5.4.3.47's own syntax table sends every stream's 2-bit
            // cpldeltbae/deltbae[ch] code FIRST, then every stream's segment
            // data - the two are NOT interleaved per stream.
            if (cplinu) {
                w.put(stream_delta(cpl_stream).deltnseg > 0 ? 1u : 2u, 2);  // cpldeltbae
            }
            for (int ch = 0; ch < nfchans; ++ch) {
                w.put(stream_delta(ch).deltnseg > 0 ? 1u : 2u, 2);  // deltbae[ch]
            }
            const auto emit_segments = [&](const DeltaSegments& segs) {
                if (segs.deltnseg > 0) {
                    w.put(static_cast<std::uint32_t>(segs.deltnseg - 1), 3);
                    for (int seg = 0; seg < segs.deltnseg; ++seg) {
                        const auto i = static_cast<std::size_t>(seg);
                        w.put(static_cast<std::uint32_t>(segs.deltoffst[i]), 5);
                        w.put(static_cast<std::uint32_t>(segs.deltlen[i]), 4);
                        w.put(static_cast<std::uint32_t>(segs.deltba[i]), 3);
                    }
                }
            };
            if (cplinu) {
                emit_segments(stream_delta(cpl_stream));
            }
            for (int ch = 0; ch < nfchans; ++ch) {
                emit_segments(stream_delta(ch));
            }
        }
    };

    // --- 8. Measure the side information -----------------------------------
    const auto measure_side_bits = [&] {
        std::uint32_t bits = 16 + 16 + 2 + 6;  // syncinfo
        std::uint32_t bsi = 25;
        if (has_three_front(config_.acmod)) bsi += 2;  // cmixlev
        if (has_surround(config_.acmod)) bsi += 2;     // surmixlev
        if (config_.acmod == Acmod::k2_0) bsi += 2;    // dsurmod
        if (config_.heavy) bsi += 8;                   // compr (§5.4.2.10)
        if (dual_mono) {
            bsi += 5 + 1 + 1 + 1;  // dialnorm2, compr2e, langcod2e, audprodi2e
            if (config_.heavy2) bsi += 8;  // compr2 - Ch2's OWN heavy flag, not Ch1's
        }
        bits += bsi;
        BitWriter counter;
        for (int block = 0; block < kBlocksPerFrame; ++block) {
            emit_block_side_info(counter, block);
            counter.put(0, 1);  // skiple, always present
        }
        bits += static_cast<std::uint32_t>(counter.bit_count());
        return bits;
    };
    AC3_ZONE_BEGIN(zone_side_bits, "step8_side_bits");
    std::uint32_t side_bits = measure_side_bits();

    // §7.2.2.6: delta bit allocation is a pure quality refinement, never
    // load-bearing - a run's own code saying "no delta" is always legal - so
    // its side-info cost must never be the reason an otherwise-fittable frame
    // is refused. Cleared and re-measured, lazily, only if the budget check
    // below would otherwise fail on it. This is the ONLY gate on delta now
    // (see the per-run computation above, step 5): every eligible stream -
    // every fbw channel and the coupling channel alike, including a frame
    // where coupling is active - gets a chance at a delta correction, and
    // this is what reins in the side-info cost if that chance turns out to
    // be more than the frame can afford.
    if (side_bits + detail::kTailBits > total_bits) {
        bool any_delta = false;
        for (auto& p : plan) {
            for (auto& run : p.runs) {
                if (run.delta.deltnseg > 0) {
                    any_delta = true;
                    run.delta = {};
                }
            }
        }
        if (any_delta) {
            side_bits = measure_side_bits();
        }
    }
    // Ended before the fit check below rather than after `budget`: the
    // check's failure path returns out of encode_frame, and a manual
    // TracyCZone must not be left open across a return.
    AC3_ZONE_END(zone_side_bits);
    if (side_bits + detail::kTailBits > total_bits) {
        // The chosen configuration cannot fit its own headers at this rate.
        return std::unexpected(FrameError::kInvalidBitrate);
    }
    // Mutable: step 9 below may swap this for the no-delta budget if
    // coupling's delta correction turns out to cost more composite SNR
    // offset than it is worth.
    std::uint32_t budget = total_bits - side_bits - detail::kTailBits;

    // --- 9. SNR-offset search ----------------------------------------------
    AC3_ZONE_BEGIN(zone_snr_search, "step9_snr_search");
    std::vector<std::vector<std::vector<std::uint8_t>>> run_bap(
        static_cast<std::size_t>(streams));
    for (int s = 0; s < streams; ++s) {
        run_bap[static_cast<std::size_t>(s)].resize(
            plan[static_cast<std::size_t>(s)].runs.size());
    }
    std::vector<std::span<const std::uint8_t>> bap_views(static_cast<std::size_t>(streams));

    const auto bits_at = [&](int composite) {
        AC3_ZONE_SCOPED_N("bits_at");
        for (int s = 0; s < streams; ++s) {
            auto& p = plan[static_cast<std::size_t>(s)];
            // Every stream shares one fsnroffst here, so the frame-wide
            // §7.2.2.1.1 condition reduces to the composite being zero.
            for (std::size_t run = 0; run < p.runs.size(); ++run) {
                const BitAllocRegion region{.start = stream_start(s),
                                            .coupling = s == cpl_stream,
                                            .cplfleak = cplfleak,
                                            .cplsleak = cplsleak,
                                            .snr_all_zero = composite == 0,
                                            .delta = p.runs[run].delta};
                auto& bap = run_bap[static_cast<std::size_t>(s)][run];
                bap.assign(p.runs[run].decoded.size(), 0);
                compute_bit_allocation(p.runs[run].decoded, config_.sample_rate, codes,
                                       composite >> 4, composite & 15, bap, region);
            }
        }
        std::uint32_t total = 0;
        for (int block = 0; block < kBlocksPerFrame; ++block) {
            for (int s = 0; s < streams; ++s) {
                const auto& p = plan[static_cast<std::size_t>(s)];
                const auto run = static_cast<std::size_t>(
                    p.run_of_block[static_cast<std::size_t>(block)]);
                // Only the stream's own region carries mantissas.
                const auto& bap = run_bap[static_cast<std::size_t>(s)][run];
                bap_views[static_cast<std::size_t>(s)] =
                    std::span{bap}.subspan(static_cast<std::size_t>(stream_start(s)));
            }
            total += static_cast<std::uint32_t>(mantissa_bits_per_block(bap_views));
        }
        return total;
    };

    // Binary-searches `search_budget` for the largest fitting composite
    // offset and hands back that offset's own mantissa cost too, so a caller
    // never has to re-run bits_at() over every stream just to learn what its
    // own search already measured on the winning probe (the same "was the
    // last probe already the answer" trick the single-pass search used to
    // apply inline).
    const auto search = [&](std::uint32_t search_budget) -> SnrSearchResult {
        int last_eval = -1;
        std::uint32_t last_bits = 0;
        const int found =
            internal::search_max_fitting(1023, snr_search_hint_, [&](int composite) {
                last_eval = composite;
                last_bits = bits_at(composite);
                return last_bits <= search_budget;
            });
        return {found, last_eval == found ? last_bits : bits_at(found)};
    };

    auto [lo, mantissa_bits] = search(budget);

    // §7.2.2.6 says delta is a pure refinement, and step 8 above already
    // guarantees it never costs a frame its FIT. It can still cost a frame
    // composite SNR offset - quality - even while comfortably fitting: every
    // delta segment is side-info bits taken out of the same budget that
    // would otherwise buy a higher offset, and a correction that lowers the
    // mask in one band asks for MORE mantissa precision there, not less.
    // Coupling is where this bites hardest, because "coupling must not cost
    // more bits than the channels it replaces" (test_encoder.cpp) is a
    // standing promise this encoder makes about the resulting composite
    // offset, and it broke - at 96 kbit/s stereo, no exotic layout required -
    // the moment delta became eligible during coupling (see step 5's
    // comment). So whenever coupling is active and there is still a delta
    // queued to send (step 8's fit-based fallback may already have cleared
    // every one of them), the search is repeated with delta fully cleared,
    // and whichever pass reaches the higher composite offset wins - a tie
    // keeps delta, since at equal offset it is a strictly free correction.
    bool any_delta = false;
    for (const auto& p : plan) {
        for (const auto& run : p.runs) {
            any_delta = any_delta || run.delta.deltnseg > 0;
        }
    }
    if (cplinu && any_delta) {
        std::vector<std::vector<DeltaSegments>> saved(plan.size());
        for (std::size_t s = 0; s < plan.size(); ++s) {
            saved[s].resize(plan[s].runs.size());
            for (std::size_t r = 0; r < plan[s].runs.size(); ++r) {
                saved[s][r] = plan[s].runs[r].delta;
                plan[s].runs[r].delta = {};
            }
        }
        const std::uint32_t side_bits_without = measure_side_bits();
        // Clearing delta only ever removes bits from the side information,
        // so this cannot be larger than what step 8 already proved fits.
        assert(side_bits_without <= side_bits);
        const std::uint32_t budget_without = total_bits - side_bits_without - detail::kTailBits;
        const auto without = search(budget_without);
        if (without.composite > lo) {
            lo = without.composite;
            budget = budget_without;
            mantissa_bits = without.mantissa_bits;
            // Deltas are already cleared above; leave them that way.
        } else {
            for (std::size_t s = 0; s < plan.size(); ++s) {
                for (std::size_t r = 0; r < plan[s].runs.size(); ++r) {
                    plan[s].runs[r].delta = saved[s][r];
                }
            }
            // run_bap was left holding the no-delta pass's allocation above;
            // restoring plan's deltas invalidates it, so step 10 needs a
            // fresh evaluation at the winning (delta) composite.
            mantissa_bits = bits_at(lo);
        }
    }

    snr_search_hint_ = lo;
    csnroffst = lo >> 4;
    fsnroffst = lo & 15;
    assert(mantissa_bits <= budget);
    AC3_ZONE_END(zone_snr_search);

    // --- 10. Mantissa tokens per block -------------------------------------
    AC3_ZONE_BEGIN(zone_mantissa_tokens, "step10_mantissa_tokens");
    // §5.3.3 ordering: each fbw channel's mantissas, with the coupling
    // channel's inserted right after the FIRST coupled channel, then the LFE.
    std::array<std::vector<MantissaToken>, kBlocksPerFrame> block_tokens;
    // maybe_unused: only the assert below reads this, and NDEBUG removes it.
    [[maybe_unused]] std::size_t token_bits_total = 0;
    for (int block = 0; block < kBlocksPerFrame; ++block) {
        MantissaBlockWriter writer;
        const auto emit_stream = [&](int s) {
            const auto& p = plan[static_cast<std::size_t>(s)];
            const auto run = static_cast<std::size_t>(
                p.run_of_block[static_cast<std::size_t>(block)]);
            const auto& exps = p.runs[run].decoded;
            const auto& bap = run_bap[static_cast<std::size_t>(s)][run];
            const int begin = stream_start(s);
            const int end = stream_end(s);
            for (int bin = begin; bin < end; ++bin) {
                const int exp = exps[static_cast<std::size_t>(bin)];
                const auto mantissa = static_cast<std::int32_t>(
                    static_cast<std::int64_t>(fixed_at(s, block, bin - begin)) << exp);
                writer.add(mantissa, bap[static_cast<std::size_t>(bin)]);
            }
        };
        bool emitted_coupling = false;
        for (int ch = 0; ch < nfchans; ++ch) {
            emit_stream(ch);
            if (cplinu && !emitted_coupling) {
                emit_stream(cpl_stream);
                emitted_coupling = true;
            }
        }
        if (config_.lfe) {
            emit_stream(nfchans);
        }
        writer.finish_block();
        token_bits_total += writer.bit_count();
        block_tokens[static_cast<std::size_t>(block)] = writer.tokens();
    }
    assert(token_bits_total == mantissa_bits);
    AC3_ZONE_END(zone_mantissa_tokens);

    // --- 11. Pack ----------------------------------------------------------
    AC3_ZONE_BEGIN(zone_pack, "step11_pack_bitstream_mux");
    const auto plan_pad = detail::plan_padding(budget - mantissa_bits);

    BitWriter w;
    w.put(kSyncWord, 16);
    w.put(0, 16);  // crc1, patched below
    w.put(static_cast<std::uint32_t>(config_.sample_rate), 2);
    w.put(static_cast<std::uint32_t>(*index) * 2 + (pad ? 1u : 0u), 6);

    w.put(8, 5);  // bsid
    w.put(0, 3);  // bsmod
    w.put(static_cast<std::uint32_t>(config_.acmod), 3);
    if (has_three_front(config_.acmod)) {
        w.put(static_cast<std::uint32_t>(config_.cmixlev), 2);
    }
    if (has_surround(config_.acmod)) {
        w.put(static_cast<std::uint32_t>(config_.surmixlev), 2);
    }
    if (config_.acmod == Acmod::k2_0) {
        w.put(0, 2);  // dsurmod
    }
    w.put(config_.lfe ? 1 : 0, 1);
    w.put(static_cast<std::uint32_t>(config_.dialnorm), 5);
    w.put(config_.heavy ? 1 : 0, 1);  // compre
    if (config_.heavy) {
        w.put(compr, 8);
    }
    w.put(0, 1);  // langcode
    w.put(0, 1);  // audprodie
    if (dual_mono) {
        w.put(static_cast<std::uint32_t>(*config_.dialnorm2), 5);
        // compr2e is Ch2's OWN flag (§5.4.2.11 mirrors §5.4.2.10 for the
        // second programme) - it does not piggyback on Ch1's compre, or a
        // 1+1 stream with only Ch1 heavy-compressed would wrongly claim a
        // compr2 word it never computed.
        w.put(config_.heavy2 ? 1 : 0, 1);  // compr2e
        if (config_.heavy2) {
            w.put(compr2, 8);
        }
        w.put(0, 1);  // langcod2e
        w.put(0, 1);  // audprodi2e
    }
    w.put(0, 1);  // copyrightb
    w.put(1, 1);  // origbs
    w.put(0, 1);  // timecod1e
    w.put(0, 1);  // timecod2e
    w.put(0, 1);  // addbsie

    for (int block = 0; block < kBlocksPerFrame; ++block) {
        emit_block_side_info(w, block);

        const std::uint16_t skip = plan_pad.skip_bytes[static_cast<std::size_t>(block)];
        w.put(skip > 0 ? 1 : 0, 1);  // skiple
        if (skip > 0) {
            w.put(skip, 9);
            for (std::uint16_t i = 0; i < skip; ++i) {
                w.put(0, 8);
            }
        }
        for (const auto& token : block_tokens[static_cast<std::size_t>(block)]) {
            w.put(token.value, token.bits);
        }
    }

    assert(w.bit_count() + plan_pad.aux_bits + detail::kTailBits == total_bits);
    for (std::uint32_t i = 0; i < plan_pad.aux_bits; ++i) {
        w.put(0, 1);
    }
    w.put(0, 1);   // auxdatae
    w.put(0, 1);   // crcrsv
    w.put(0, 16);  // crc2, patched below
    assert(w.bit_count() == total_bits);

    std::vector<std::byte> frame = w.take();
    const std::span<const std::byte> view{frame};
    const std::uint16_t crc1 = solve_leading_crc(view.subspan(4, 2 * words58 - 4));
    frame[2] = static_cast<std::byte>(crc1 >> 8);
    frame[3] = static_cast<std::byte>(crc1 & 0xFF);
    std::uint16_t crc2 = crc16(view.subspan(2, total_bytes - 4));
    if (crc2 == kSyncWord) {
        frame[total_bytes - 3] ^= std::byte{0x01};
        crc2 = crc16(view.subspan(2, total_bytes - 4));
    }
    frame[total_bytes - 2] = static_cast<std::byte>(crc2 >> 8);
    frame[total_bytes - 1] = static_cast<std::byte>(crc2 & 0xFF);
    AC3_ZONE_END(zone_pack);
    return frame;
}

}  // namespace ac3
