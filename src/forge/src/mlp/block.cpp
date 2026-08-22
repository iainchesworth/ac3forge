#include "ac3/mlp/block.hpp"

#include <algorithm>
#include <array>
#include <cassert>

#include "ac3/mlp/huffman.hpp"

namespace ac3::mlp {

namespace {

// Provisional field layout (see block.hpp's header comment). All widths are
// this project's own self-consistent choices except where the WO states one
// (B1 "typically requiring 5 bits"; the N-bit LSB word):
//
//   coding           2 bits   (BlockCoding)
//   b1               5 bits   (WO)
//   lsb_word         N bits   (WO: leading N-b1 bits are the DC offset)
//   -- kEmpty stops here --
//   n                5 bits   (Table 3's n = 3..19, or PCM's n = 1..30)
//   shift            4 bits
//   order_a          4 bits   (0..8)
//   order_b          4 bits   (0..8)
//   coefficients     10 bits each, signed (covers the WO's stated m/64
//                             ranges, max |192|, with headroom)
//   init             (N - b1) bits each, signed, max(order_a, order_b) of them
//
// Payload follows immediately: L codewords (kSignificant via Tables 2+3,
// kPcm via Table 7), nothing for kEmpty.

constexpr int kCodingBits = 2;
constexpr int kB1Bits = 5;
constexpr int kNBits = 5;
constexpr int kShiftBits = 4;
constexpr int kOrderBits = 4;
constexpr int kCoefficientBits = 10;
constexpr std::int32_t kCoefficientMax = (std::int32_t{1} << (kCoefficientBits - 1)) - 1;

void put_signed(BitWriter& w, std::int32_t value, int bits) {
    assert(bits >= 1 && bits <= 31);
    assert(value >= -(std::int32_t{1} << (bits - 1)) &&
           value < (std::int32_t{1} << (bits - 1)));
    w.put(static_cast<std::uint32_t>(value) & ((1u << bits) - 1), bits);
}

[[nodiscard]] std::int32_t read_signed(BitReader& r, int bits) {
    assert(bits >= 1 && bits <= 31);
    const std::uint32_t raw = r.read(bits);
    const std::uint32_t sign = 1u << (bits - 1);
    return static_cast<std::int32_t>((raw ^ sign)) - static_cast<std::int32_t>(sign);
}

// How many constant low bits every sample shares - the WO's B1 detection:
// "determine for that block how many B1 of the least significant bits have
// identical form throughout the block ... comparing the identity of each
// bit in a word with that of the first word". Capped at N-1 so a constant
// block still leaves one significant bit (all-zero blocks take the kEmpty
// path before this matters).
[[nodiscard]] int detect_b1(std::span<const std::int32_t> samples, int wordlength) {
    std::uint32_t common = ~0u;
    for (const auto sample : samples) {
        common &= ~static_cast<std::uint32_t>(sample ^ samples[0]);
    }
    int b1 = 0;
    while (b1 < wordlength - 1 && ((common >> b1) & 1) != 0) {
        ++b1;
    }
    return b1;
}

// The DC offset carried in the LSB word's leading bits (WO: the N-bit LSB
// word's leading N-B1 bits). Two candidates, priced by the exact payload
// the entropy stage would emit, because neither dominates: the MIDRANGE
// centres the peak level that entropy-table selection keys on (best when
// the residual IS the signal - passthrough noise on a pedestal), while the
// FIRST SAMPLE zeroes the predictor's warm-up view (best when prediction
// removes the DC anyway and the only DC-sensitive residual is the first
// one). A constant block scores zero under both and codes as empty.
//
// A candidate is only legal if the shifted words still fit the signed
// (wordlength - B1)-bit significant domain (`width`) - midrange always
// does, but centring on the first sample can double the span, and the
// single-channel init field packs at exactly that width.
[[nodiscard]] std::int32_t choose_dc(std::span<const std::int32_t> significant,
                                     const PredictorCoefficients& coefficients, int width) {
    const auto [lo_it, hi_it] = std::minmax_element(significant.begin(), significant.end());
    const auto lo = *lo_it;
    const auto hi = *hi_it;
    const auto midrange = lo + (hi - lo) / 2;
    const auto fits = [&](std::int32_t dc) {
        return static_cast<std::int64_t>(lo) - dc >= -(std::int64_t{1} << (width - 1)) &&
               static_cast<std::int64_t>(hi) - dc < (std::int64_t{1} << (width - 1));
    };
    const std::array<std::int32_t, 2> candidates{significant[0], midrange};
    if (candidates[0] == candidates[1] || !fits(candidates[0])) {
        return midrange;
    }
    std::int32_t best = midrange;
    long long best_bits = -1;
    std::vector<std::int32_t> shifted(significant.size());
    std::vector<std::int32_t> residual(significant.size());
    for (const auto dc : candidates) {
        for (std::size_t i = 0; i < significant.size(); ++i) {
            shifted[i] = significant[i] - dc;
        }
        PredictorState state{};
        predict_encode(coefficients, shifted, residual, state);
        const auto bits = choose_coding_cost(residual).payload_bits;
        if (best_bits < 0 || bits < best_bits) {
            best_bits = bits;
            best = dc;
        }
    }
    return best;
}

}  // namespace

void build_block_header(BitWriter& w, const BlockHeader& header, int wordlength) {
    assert(wordlength >= 2 && wordlength <= 24);
    assert(header.b1 >= 0 && header.b1 < wordlength);

    w.put(static_cast<std::uint32_t>(header.coding), kCodingBits);
    w.put(static_cast<std::uint32_t>(header.b1), kB1Bits);
    w.put(header.lsb_word & ((1u << wordlength) - 1), wordlength);

    if (header.coding == BlockCoding::kEmpty) {
        return;
    }

    w.put(static_cast<std::uint32_t>(header.n), kNBits);
    w.put(static_cast<std::uint32_t>(header.coefficients.shift), kShiftBits);
    w.put(static_cast<std::uint32_t>(header.coefficients.a.size()), kOrderBits);
    w.put(static_cast<std::uint32_t>(header.coefficients.b.size()), kOrderBits);
    for (const auto c : header.coefficients.a) {
        put_signed(w, c, kCoefficientBits);
    }
    for (const auto c : header.coefficients.b) {
        put_signed(w, c, kCoefficientBits);
    }
    const auto order = std::max(header.coefficients.a.size(), header.coefficients.b.size());
    assert(header.init.size() == order);
    for (const auto i : header.init) {
        put_signed(w, i, wordlength - header.b1);
    }
}

bool parse_block_header(BitReader& r, int wordlength, BlockHeader& out) {
    const auto coding = r.read(kCodingBits);
    if (coding > static_cast<std::uint32_t>(BlockCoding::kSignificant)) {
        return false;
    }
    out.coding = static_cast<BlockCoding>(coding);

    out.b1 = static_cast<int>(r.read(kB1Bits));
    if (out.b1 >= wordlength) {
        return false;
    }
    out.lsb_word = r.read(wordlength);

    if (out.coding == BlockCoding::kEmpty) {
        out.n = 0;
        out.coefficients = {};
        out.init.clear();
        return !r.overflowed();
    }

    out.n = static_cast<int>(r.read(kNBits));
    if (out.coding == BlockCoding::kSignificant &&
        (out.n < huffman::kMinN || out.n > huffman::kMaxN)) {
        return false;
    }
    if (out.coding == BlockCoding::kPcm && (out.n < 1 || out.n > 30)) {
        return false;
    }

    out.coefficients.shift = static_cast<int>(r.read(kShiftBits));
    const auto order_a = r.read(kOrderBits);
    const auto order_b = r.read(kOrderBits);
    if (order_a > kMaxPredictorOrder || order_b > kMaxPredictorOrder) {
        return false;
    }
    out.coefficients.a.resize(order_a);
    out.coefficients.b.resize(order_b);
    for (auto& c : out.coefficients.a) {
        c = read_signed(r, kCoefficientBits);
    }
    for (auto& c : out.coefficients.b) {
        c = read_signed(r, kCoefficientBits);
    }
    const auto order = std::max<std::size_t>(order_a, order_b);
    out.init.resize(order);
    for (auto& i : out.init) {
        i = read_signed(r, wordlength - out.b1);
    }
    return !r.overflowed();
}

void encode_block(BitWriter& w, std::span<const std::int32_t> samples, int wordlength,
                  const PredictorCoefficients& coefficients) {
    assert(!samples.empty());
    assert(wordlength >= 2 && wordlength <= 24);
    for ([[maybe_unused]] const auto s : samples) {
        // The single most-negative code is excluded: every entropy range in
        // WO Tables 3/7 is asymmetric, -2^n + 1 .. 2^n (see huffman.hpp).
        assert(s > -(std::int32_t{1} << (wordlength - 1)) &&
               s < (std::int32_t{1} << (wordlength - 1)));
    }
    for ([[maybe_unused]] const auto c : coefficients.a) {
        assert(c >= -kCoefficientMax && c <= kCoefficientMax);
    }
    for ([[maybe_unused]] const auto c : coefficients.b) {
        assert(c >= -kCoefficientMax && c <= kCoefficientMax);
    }

    BlockHeader header;

    // Digital black takes the WO's "empty" table: B1/LSB word only.
    if (std::all_of(samples.begin(), samples.end(), [](std::int32_t s) { return s == 0; })) {
        header.coding = BlockCoding::kEmpty;
        build_block_header(w, header, wordlength);
        return;
    }

    // Strip the constant LSBs, then centre what's left: the LSB word packs
    // [DC offset | constant LSB pattern], and removing the midrange DC
    // before prediction keeps a pedestal (or a predictor's warm-up view of
    // one) from widening the whole block's entropy table.
    header.b1 = detect_b1(samples, wordlength);
    const auto lsb_pattern = static_cast<std::uint32_t>(samples[0]) & ((1u << header.b1) - 1);

    std::vector<std::int32_t> significant(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        significant[i] = (samples[i] - static_cast<std::int32_t>(lsb_pattern)) >> header.b1;
    }
    const auto dc = choose_dc(significant, coefficients, wordlength - header.b1);
    for (auto& value : significant) {
        value -= dc;
    }
    const int dc_bits = wordlength - header.b1;
    header.lsb_word =
        ((static_cast<std::uint32_t>(dc) & ((1u << dc_bits) - 1)) << header.b1) | lsb_pattern;

    // Decorrelate. The first `order` significant words double as the
    // header's initialisation data (the WO's state-swap rule).
    header.coefficients = coefficients;
    const auto order = std::max(coefficients.a.size(), coefficients.b.size());
    assert(order <= samples.size());
    header.init.assign(significant.begin(),
                       significant.begin() + static_cast<std::ptrdiff_t>(order));

    std::vector<std::int32_t> residual(significant.size());
    PredictorState state{};
    predict_encode(coefficients, significant, residual, state);

    // Entropy mode from the block's peak residual: the smallest fitting
    // Table 3 range, with the WO's own PCM-is-occasionally-better cost
    // comparison; residuals wider than Table 3's cap force PCM.
    const auto [lo_it, hi_it] = std::minmax_element(residual.begin(), residual.end());
    int n = 1;
    while (n < 30 && (*lo_it < -(std::int32_t{1} << n) + 1 || *hi_it > (std::int32_t{1} << n))) {
        ++n;
    }

    if (n > huffman::kMaxN) {
        header.coding = BlockCoding::kPcm;
        header.n = n;
    } else {
        const int table_n = std::max(n, huffman::kMinN);
        long long significant_bits = 0;
        for (const auto v : residual) {
            significant_bits += huffman::significant_length(v, table_n);
        }
        const long long pcm_bits =
            static_cast<long long>(residual.size()) * (n + 1);
        if (pcm_bits < significant_bits) {
            header.coding = BlockCoding::kPcm;
            header.n = n;
        } else {
            header.coding = BlockCoding::kSignificant;
            header.n = table_n;
        }
    }

    build_block_header(w, header, wordlength);
    for (const auto v : residual) {
        if (header.coding == BlockCoding::kPcm) {
            huffman::encode_pcm(w, v, header.n);
        } else {
            huffman::encode_significant(w, v, header.n);
        }
    }
}

bool decode_block(BitReader& r, int wordlength, std::span<std::int32_t> samples) {
    BlockHeader header;
    if (!parse_block_header(r, wordlength, header)) {
        return false;
    }

    if (header.coding == BlockCoding::kEmpty) {
        std::fill(samples.begin(), samples.end(), 0);
        return !r.overflowed();
    }

    std::vector<std::int32_t> residual(samples.size());
    for (auto& v : residual) {
        v = header.coding == BlockCoding::kPcm ? huffman::decode_pcm(r, header.n)
                                               : huffman::decode_significant(r, header.n);
    }
    if (r.overflowed()) {
        return false;
    }

    // Fig. 18b's reconstruction with the WO's state swap: the first `order`
    // outputs are the header's initialisation data verbatim, the matching
    // residuals seed the feedback history, and normal decode runs from
    // there.
    const auto order = header.init.size();
    if (order > samples.size()) {
        return false;
    }
    std::vector<std::int32_t> significant(samples.size());
    PredictorState state{};
    for (std::size_t i = 0; i < order; ++i) {
        significant[i] = header.init[i];
        // Mirror predict_encode/decode's own history bookkeeping.
        for (std::size_t j = state.input.size() - 1; j > 0; --j) {
            state.input[j] = state.input[j - 1];
            state.output[j] = state.output[j - 1];
        }
        state.input[0] = significant[i];
        state.output[0] = residual[i];
    }
    predict_decode(header.coefficients,
                   std::span<const std::int32_t>{residual}.subspan(order),
                   std::span<std::int32_t>{significant}.subspan(order), state);

    // DC offset (leading wordlength - b1 bits of the LSB word), then
    // reattach the constant LSB pattern.
    const int dc_bits = wordlength - header.b1;
    const auto dc_raw = header.lsb_word >> header.b1;
    const auto dc = static_cast<std::int32_t>((dc_raw ^ (1u << (dc_bits - 1)))) -
                    (std::int32_t{1} << (dc_bits - 1));
    const auto lsb =
        static_cast<std::int32_t>(header.lsb_word & ((1u << header.b1) - 1));
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = ((significant[i] + dc) << header.b1) + lsb;
    }
    return true;
}

// --- multichannel ----------------------------------------------------------

namespace {

// Provisional multichannel layout (block.hpp's header comment):
//
//   channel count - 1     4 bits
//   matrix step count     4 bits
//   per step: target 4, shift 4, then (channels - 1) coefficients at 10
//             bits signed, dense in channel order skipping the target
//   per channel: b1 5, lsb_word N        (strip happens BEFORE the matrix)
//   per channel: coding 2, then for non-empty channels n 5, shift 4,
//                order_a 4, order_b 4, coefficients, init_width 5, init at
//                init_width bits each. Unlike the single-channel layout's
//                fixed (N - b1)-bit init, the width is explicit here: init
//                values are POST-matrix significant words, and the matrix
//                can grow them past the input wordlength (the WO notes the
//                same effect for its quantizer cascades: matrixing "will
//                increase the wordlength of initialisation data").
//   payload: interleaved per sample across non-empty channels

constexpr int kChannelCountBits = 4;
constexpr int kStepCountBits = 4;
constexpr int kStepTargetBits = 4;
constexpr int kStepShiftBits = 4;
constexpr int kInitWidthBits = 5;

// The smallest signed field width holding every value in `values` (at
// least 1 bit).
[[nodiscard]] int signed_width(std::span<const std::int32_t> values) {
    int width = 1;
    for (const auto value : values) {
        while (value < -(std::int32_t{1} << (width - 1)) ||
               value >= (std::int32_t{1} << (width - 1))) {
            ++width;
        }
    }
    return width;
}

struct ChannelPlan {
    BlockCoding coding = BlockCoding::kEmpty;
    int n = 0;
    std::vector<std::int32_t> residual;
    std::vector<std::int32_t> init;
};

// The entropy-mode decision shared with the single-channel path's inline
// logic: smallest fitting range, PCM forced beyond Table 3's cap, and the
// WO's PCM-cost comparison otherwise.
[[nodiscard]] std::pair<BlockCoding, int> choose_coding(std::span<const std::int32_t> residual) {
    const auto choice = choose_coding_cost(residual);
    return {choice.coding, choice.n};
}

[[nodiscard]] ChannelPlan plan_channel(std::span<const std::int32_t> significant,
                                       const PredictorCoefficients& coefficients) {
    ChannelPlan plan;
    if (std::all_of(significant.begin(), significant.end(),
                    [](std::int32_t v) { return v == 0; })) {
        return plan;  // kEmpty
    }
    const auto order = std::max(coefficients.a.size(), coefficients.b.size());
    assert(order <= significant.size());
    plan.init.assign(significant.begin(),
                     significant.begin() + static_cast<std::ptrdiff_t>(order));
    plan.residual.resize(significant.size());
    PredictorState state{};
    predict_encode(coefficients, significant, plan.residual, state);
    const auto [coding, n] = choose_coding(plan.residual);
    plan.coding = coding;
    plan.n = n;
    return plan;
}

// The decoder half of the WO's state-swap rule, shared shape with
// decode_block's inline version: init values ARE the first outputs, the
// matching residuals seed the feedback history.
void reconstruct_channel(const PredictorCoefficients& coefficients,
                         std::span<const std::int32_t> init,
                         std::span<const std::int32_t> residual,
                         std::span<std::int32_t> significant) {
    PredictorState state{};
    for (std::size_t i = 0; i < init.size(); ++i) {
        significant[i] = init[i];
        for (std::size_t j = state.input.size() - 1; j > 0; --j) {
            state.input[j] = state.input[j - 1];
            state.output[j] = state.output[j - 1];
        }
        state.input[0] = significant[i];
        state.output[0] = residual[i];
    }
    predict_decode(coefficients, residual.subspan(init.size()),
                   significant.subspan(init.size()), state);
}

}  // namespace

void encode_block_channels(BitWriter& w, std::span<const std::span<const std::int32_t>> channels,
                           int wordlength, const MultichannelBlockConfig& config) {
    const auto channel_count = channels.size();
    assert(channel_count >= 1 && channel_count <= kMaxBlockChannels);
    assert(config.coefficients.size() == channel_count);
    assert(config.steps.size() <= 15);
    assert(!channels[0].empty());
    const auto length = channels[0].size();
    for ([[maybe_unused]] const auto& channel : channels) {
        assert(channel.size() == length);
        for ([[maybe_unused]] const auto s : channel) {
            assert(s > -(std::int32_t{1} << (wordlength - 1)) &&
                   s < (std::int32_t{1} << (wordlength - 1)));
        }
    }

    // Per-channel strip, BEFORE the matrix (WO Fig. 3's stage order), then
    // per-channel DC centring - the LSB word packs [DC | pattern], and the
    // matrix gets to work on centred channels (the decoder inverts in the
    // opposite order: predict, un-matrix, add DC, unstrip). A constant
    // channel centres to all-zero and rides the free kEmpty path.
    std::vector<int> b1(channel_count);
    std::vector<std::uint32_t> lsb_word(channel_count);
    std::vector<std::vector<std::int32_t>> significant(channel_count);
    for (std::size_t c = 0; c < channel_count; ++c) {
        b1[c] = detect_b1(channels[c], wordlength);
        const auto pattern =
            static_cast<std::uint32_t>(channels[c][0]) & ((1u << b1[c]) - 1);
        significant[c].resize(length);
        for (std::size_t i = 0; i < length; ++i) {
            significant[c][i] =
                (channels[c][i] - static_cast<std::int32_t>(pattern)) >> b1[c];
        }
        // The caller's offset when given (see MultichannelBlockConfig::dc -
        // mandatory for consistency when it also chose the matrix);
        // otherwise priced here against this channel's configured
        // predictor.
        const auto dc =
            c < config.dc.size()
                ? config.dc[c]
                : choose_dc(significant[c], config.coefficients[c], wordlength - b1[c]);
        for (auto& value : significant[c]) {
            value -= dc;
        }
        const int dc_bits = wordlength - b1[c];
        lsb_word[c] =
            ((static_cast<std::uint32_t>(dc) & ((1u << dc_bits) - 1)) << b1[c]) | pattern;
    }

    // The lossless matrix, per sample instant across channels.
    if (!config.steps.empty()) {
        std::vector<std::int64_t> instant(channel_count);
        for (std::size_t i = 0; i < length; ++i) {
            for (std::size_t c = 0; c < channel_count; ++c) {
                instant[c] = significant[c][i];
            }
            matrix::encode_cascade(config.steps, instant);
            for (std::size_t c = 0; c < channel_count; ++c) {
                assert(instant[c] >= INT32_MIN && instant[c] <= INT32_MAX);
                significant[c][i] = static_cast<std::int32_t>(instant[c]);
            }
        }
    }

    std::vector<ChannelPlan> plans(channel_count);
    for (std::size_t c = 0; c < channel_count; ++c) {
        plans[c] = plan_channel(significant[c], config.coefficients[c]);
    }

    // Header.
    w.put(static_cast<std::uint32_t>(channel_count - 1), kChannelCountBits);
    w.put(static_cast<std::uint32_t>(config.steps.size()), kStepCountBits);
    for (const auto& step : config.steps) {
        assert(step.target >= 0 && static_cast<std::size_t>(step.target) < channel_count);
        w.put(static_cast<std::uint32_t>(step.target), kStepTargetBits);
        assert(step.shift >= 0 && step.shift <= 14);
        w.put(static_cast<std::uint32_t>(step.shift), kStepShiftBits);
        // Dense n-1 coefficients in channel order, skipping the target.
        for (std::size_t c = 0; c < channel_count; ++c) {
            if (static_cast<int>(c) == step.target) {
                continue;
            }
            std::int32_t numerator = 0;
            for (const auto& [source, value] : step.terms) {
                assert(source != step.target);
                if (source == static_cast<int>(c)) {
                    numerator = value;
                }
            }
            put_signed(w, numerator, kCoefficientBits);
        }
    }
    for (std::size_t c = 0; c < channel_count; ++c) {
        w.put(static_cast<std::uint32_t>(b1[c]), kB1Bits);
        w.put(lsb_word[c] & ((1u << wordlength) - 1), wordlength);
    }
    for (std::size_t c = 0; c < channel_count; ++c) {
        w.put(static_cast<std::uint32_t>(plans[c].coding), kCodingBits);
        if (plans[c].coding == BlockCoding::kEmpty) {
            continue;
        }
        const auto& coefficients = config.coefficients[c];
        w.put(static_cast<std::uint32_t>(plans[c].n), kNBits);
        w.put(static_cast<std::uint32_t>(coefficients.shift), kShiftBits);
        w.put(static_cast<std::uint32_t>(coefficients.a.size()), kOrderBits);
        w.put(static_cast<std::uint32_t>(coefficients.b.size()), kOrderBits);
        for (const auto value : coefficients.a) {
            put_signed(w, value, kCoefficientBits);
        }
        for (const auto value : coefficients.b) {
            put_signed(w, value, kCoefficientBits);
        }
        const int init_width = signed_width(plans[c].init);
        w.put(static_cast<std::uint32_t>(init_width), kInitWidthBits);
        for (const auto value : plans[c].init) {
            put_signed(w, value, init_width);
        }
    }

    // Payload, interleaved per sample across non-empty channels.
    for (std::size_t i = 0; i < length; ++i) {
        for (std::size_t c = 0; c < channel_count; ++c) {
            if (plans[c].coding == BlockCoding::kEmpty) {
                continue;
            }
            if (plans[c].coding == BlockCoding::kPcm) {
                huffman::encode_pcm(w, plans[c].residual[i], plans[c].n);
            } else {
                huffman::encode_significant(w, plans[c].residual[i], plans[c].n);
            }
        }
    }
}

bool decode_block_channels(BitReader& r, int wordlength,
                           std::span<const std::span<std::int32_t>> channels) {
    const auto channel_count = channels.size();
    if (channel_count == 0 || channels[0].empty()) {
        return false;
    }
    const auto length = channels[0].size();

    if (r.read(kChannelCountBits) + 1 != channel_count) {
        return false;
    }
    const auto step_count = r.read(kStepCountBits);
    std::vector<matrix::Step> steps(step_count);
    for (auto& step : steps) {
        step.target = static_cast<int>(r.read(kStepTargetBits));
        if (static_cast<std::size_t>(step.target) >= channel_count) {
            return false;
        }
        step.shift = static_cast<int>(r.read(kStepShiftBits));
        for (std::size_t c = 0; c < channel_count; ++c) {
            if (static_cast<int>(c) == step.target) {
                continue;
            }
            step.terms.emplace_back(static_cast<int>(c), read_signed(r, kCoefficientBits));
        }
    }

    std::vector<int> b1(channel_count);
    std::vector<std::uint32_t> lsb_word(channel_count);
    for (std::size_t c = 0; c < channel_count; ++c) {
        b1[c] = static_cast<int>(r.read(kB1Bits));
        if (b1[c] >= wordlength) {
            return false;
        }
        lsb_word[c] = r.read(wordlength);
    }

    std::vector<BlockCoding> coding(channel_count);
    std::vector<int> n(channel_count);
    std::vector<PredictorCoefficients> coefficients(channel_count);
    std::vector<std::vector<std::int32_t>> init(channel_count);
    for (std::size_t c = 0; c < channel_count; ++c) {
        const auto raw = r.read(kCodingBits);
        if (raw > static_cast<std::uint32_t>(BlockCoding::kSignificant)) {
            return false;
        }
        coding[c] = static_cast<BlockCoding>(raw);
        if (coding[c] == BlockCoding::kEmpty) {
            continue;
        }
        n[c] = static_cast<int>(r.read(kNBits));
        if (coding[c] == BlockCoding::kSignificant &&
            (n[c] < huffman::kMinN || n[c] > huffman::kMaxN)) {
            return false;
        }
        if (coding[c] == BlockCoding::kPcm && (n[c] < 1 || n[c] > 30)) {
            return false;
        }
        coefficients[c].shift = static_cast<int>(r.read(kShiftBits));
        const auto order_a = r.read(kOrderBits);
        const auto order_b = r.read(kOrderBits);
        if (order_a > kMaxPredictorOrder || order_b > kMaxPredictorOrder) {
            return false;
        }
        coefficients[c].a.resize(order_a);
        coefficients[c].b.resize(order_b);
        for (auto& value : coefficients[c].a) {
            value = read_signed(r, kCoefficientBits);
        }
        for (auto& value : coefficients[c].b) {
            value = read_signed(r, kCoefficientBits);
        }
        const auto order = std::max<std::size_t>(order_a, order_b);
        if (order > length) {
            return false;
        }
        const auto init_width = r.read(kInitWidthBits);
        if (init_width < 1 || init_width > 31) {
            return false;
        }
        init[c].resize(order);
        for (auto& value : init[c]) {
            value = read_signed(r, static_cast<int>(init_width));
        }
    }

    // Payload.
    std::vector<std::vector<std::int32_t>> residual(channel_count);
    for (std::size_t c = 0; c < channel_count; ++c) {
        if (coding[c] != BlockCoding::kEmpty) {
            residual[c].resize(length);
        }
    }
    for (std::size_t i = 0; i < length; ++i) {
        for (std::size_t c = 0; c < channel_count; ++c) {
            if (coding[c] == BlockCoding::kEmpty) {
                continue;
            }
            residual[c][i] = coding[c] == BlockCoding::kPcm
                                 ? huffman::decode_pcm(r, n[c])
                                 : huffman::decode_significant(r, n[c]);
        }
    }
    if (r.overflowed()) {
        return false;
    }

    // Per-channel reconstruction, matrix inversion, then unstrip.
    std::vector<std::vector<std::int32_t>> significant(channel_count);
    for (std::size_t c = 0; c < channel_count; ++c) {
        significant[c].assign(length, 0);
        if (coding[c] != BlockCoding::kEmpty) {
            reconstruct_channel(coefficients[c], init[c], residual[c], significant[c]);
        }
    }

    if (!steps.empty()) {
        std::vector<std::int64_t> instant(channel_count);
        for (std::size_t i = 0; i < length; ++i) {
            for (std::size_t c = 0; c < channel_count; ++c) {
                instant[c] = significant[c][i];
            }
            matrix::decode_cascade(steps, instant);
            for (std::size_t c = 0; c < channel_count; ++c) {
                if (instant[c] < INT32_MIN || instant[c] > INT32_MAX) {
                    return false;
                }
                significant[c][i] = static_cast<std::int32_t>(instant[c]);
            }
        }
    }

    for (std::size_t c = 0; c < channel_count; ++c) {
        const int dc_bits = wordlength - b1[c];
        const auto dc_raw = lsb_word[c] >> b1[c];
        const auto dc = static_cast<std::int32_t>((dc_raw ^ (1u << (dc_bits - 1)))) -
                        (std::int32_t{1} << (dc_bits - 1));
        const auto lsb = static_cast<std::int32_t>(lsb_word[c] & ((1u << b1[c]) - 1));
        for (std::size_t i = 0; i < length; ++i) {
            channels[c][i] = ((significant[c][i] + dc) << b1[c]) + lsb;
        }
    }
    return true;
}

// --- hooks for encoder-side selection --------------------------------------

CodingChoice choose_coding_cost(std::span<const std::int32_t> residual) {
    CodingChoice choice;
    const auto [lo_it, hi_it] = std::minmax_element(residual.begin(), residual.end());
    int n = 1;
    while (n < 30 && (*lo_it < -(std::int32_t{1} << n) + 1 || *hi_it > (std::int32_t{1} << n))) {
        ++n;
    }
    const long long pcm_bits = static_cast<long long>(residual.size()) * (n + 1);
    if (n > huffman::kMaxN) {
        return {BlockCoding::kPcm, n, pcm_bits};
    }
    const int table_n = std::max(n, huffman::kMinN);
    long long significant_bits = 0;
    for (const auto v : residual) {
        significant_bits += huffman::significant_length(v, table_n);
    }
    if (pcm_bits < significant_bits) {
        return {BlockCoding::kPcm, n, pcm_bits};
    }
    return {BlockCoding::kSignificant, table_n, significant_bits};
}

int detect_constant_lsbs(std::span<const std::int32_t> samples, int wordlength) {
    return detect_b1(samples, wordlength);
}

}  // namespace ac3::mlp
