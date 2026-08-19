#include "ac3/mlp/block.hpp"

#include <algorithm>
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

    // Strip the constant LSBs into the LSB word (DC-offset slot left zero -
    // implemented on the decode side, not yet exploited by this encoder).
    header.b1 = detect_b1(samples, wordlength);
    header.lsb_word = static_cast<std::uint32_t>(samples[0]) & ((1u << header.b1) - 1);

    std::vector<std::int32_t> significant(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        significant[i] = (samples[i] - static_cast<std::int32_t>(header.lsb_word)) >> header.b1;
    }

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

}  // namespace ac3::mlp
