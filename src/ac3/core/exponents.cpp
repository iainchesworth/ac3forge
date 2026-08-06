#include "ac3/core/exponents.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdlib>

namespace ac3 {

std::int32_t to_fixed25(double c) {
    const double scaled = std::round(c * 16777216.0);  // 2^24
    if (scaled >= 16777215.0) {
        return 16777215;  // 2^24 - 1
    }
    if (scaled <= -16777216.0) {
        return -16777216;  // -2^24
    }
    return static_cast<std::int32_t>(scaled);
}

int exponent_from_fixed(std::int32_t fixed) {
    const auto magnitude = static_cast<std::uint32_t>(std::abs(static_cast<std::int64_t>(fixed)));
    if (magnitude == 0) {
        return kMaxExponent;
    }
    // Leading zeros of the 24-bit magnitude field: countl_zero on 32 bits
    // minus the 8 bits above it. |c| >= 0.5 (bit 23 set) gives exponent 0.
    const int exponent = std::countl_zero(magnitude) - 8;
    return std::clamp(exponent, 0, kMaxExponent);
}

void extract_exponents(std::span<const std::int32_t> fixed, std::span<std::uint8_t> exponents) {
    assert(fixed.size() == exponents.size());
    for (std::size_t i = 0; i < fixed.size(); ++i) {
        exponents[i] = static_cast<std::uint8_t>(exponent_from_fixed(fixed[i]));
    }
}

EncodedExponents encode_exponents(std::span<const std::uint8_t> raw, ExpStrategy strategy) {
    const int endmant = static_cast<int>(raw.size());
    const int group_size = exponent_group_size(strategy);
    const int group_count = exponent_group_count(strategy, endmant);
    assert(group_size > 0 && endmant >= 1);
    // Every legal AC-3 mantissa count (fbw: 37 + 3*(chbwcod+12); coupled:
    // 37 + 12*cplbegf; LFE: 7) has endmant - 1 divisible by 3, which is what
    // guarantees the §7.1.3 group-count formulas cover every bin.
    assert((endmant - 1) % 3 == 0);

    const int diff_count = group_count * 3;

    // Pre-exponent sequence p[0..diff_count]: p[0] is the absolute exponent
    // (bin 0), p[1+i] covers mantissa bins [1 + i*group_size, ...). Shared
    // pairs/quads take the group's MINIMUM exponent (§8.2.10 / Table 7.3
    // note) so the loudest member stays representable. Positions whose first
    // bin lies at or past endmant are pure padding, handled after slew
    // limiting below.
    const int real_diffs = (endmant - 1 + group_size - 1) / group_size;
    assert(real_diffs <= diff_count);
    std::vector<int> pre(static_cast<std::size_t>(diff_count) + 1);
    pre[0] = std::min<int>(raw[0], kMaxAbsoluteExponent);  // §7.1.2 4-bit cap
    for (int i = 0; i < real_diffs; ++i) {
        const int begin = 1 + i * group_size;
        int value = kMaxExponent;
        for (int bin = begin; bin < begin + group_size && bin < endmant; ++bin) {
            value = std::min<int>(value, raw[static_cast<std::size_t>(bin)]);
        }
        pre[static_cast<std::size_t>(i) + 1] = value;
    }

    // Slew limiting (§8.2.10): differentials must fit +-2; adjust by only
    // ever DECREASING exponents. Forward pass caps rises at +2, backward
    // pass caps falls at -2; both only lower values, so no bin ever gets a
    // larger exponent than its raw one (mantissas gain leading zeros, which
    // is always representable).
    for (int i = 1; i <= real_diffs; ++i) {
        pre[static_cast<std::size_t>(i)] =
            std::min(pre[static_cast<std::size_t>(i)], pre[static_cast<std::size_t>(i) - 1] + 2);
    }
    for (int i = real_diffs; i-- > 0;) {
        pre[static_cast<std::size_t>(i)] =
            std::min(pre[static_cast<std::size_t>(i)], pre[static_cast<std::size_t>(i) + 1] + 2);
    }

    // Canonical padding AFTER slew limiting: zero differentials, so encoding
    // the decoder-mirror set reproduces the bitstream fields exactly.
    for (int i = real_diffs; i < diff_count; ++i) {
        pre[static_cast<std::size_t>(i) + 1] = pre[static_cast<std::size_t>(i)];
    }

    EncodedExponents encoded;
    encoded.absolute = static_cast<std::uint8_t>(pre[0]);
    encoded.groups.reserve(static_cast<std::size_t>(group_count));
    for (int g = 0; g < group_count; ++g) {
        int mapped[3];
        for (int j = 0; j < 3; ++j) {
            const std::size_t i = static_cast<std::size_t>(3 * g + j);
            const int diff = pre[i + 1] - pre[i];
            assert(diff >= -2 && diff <= 2);
            mapped[j] = diff + 2;  // Table 7.1 mapping
        }
        encoded.groups.push_back(
            static_cast<std::uint8_t>(25 * mapped[0] + 5 * mapped[1] + mapped[2]));
    }
    return encoded;
}

void decode_exponents(std::uint8_t absolute, std::span<const std::uint8_t> groups,
                      ExpStrategy strategy, std::span<std::uint8_t> out) {
    const int group_size = exponent_group_size(strategy);
    const int ngrps = static_cast<int>(groups.size());
    assert(group_size > 0);
    assert(out.empty() || (out.size() - 1) % 3 == 0);  // legal endmant contract
    assert(static_cast<int>(out.size()) <= 1 + ngrps * 3 * group_size);

    // §7.1.3 pseudocode: ungroup, unbias, accumulate, expand by grpsize.
    std::vector<int> aexp(static_cast<std::size_t>(ngrps) * 3);
    int prevexp = absolute;
    for (int grp = 0; grp < ngrps; ++grp) {
        const int gexp = groups[static_cast<std::size_t>(grp)];
        const int dexp[3] = {gexp / 25, (gexp % 25) / 5, (gexp % 25) % 5};
        for (int j = 0; j < 3; ++j) {
            const std::size_t i = static_cast<std::size_t>(grp * 3 + j);
            aexp[i] = prevexp + (dexp[j] - 2);
            prevexp = aexp[i];
        }
    }

    if (!out.empty()) {
        out[0] = absolute;
    }
    for (std::size_t i = 0; i < aexp.size(); ++i) {
        for (int j = 0; j < group_size; ++j) {
            const std::size_t bin = i * static_cast<std::size_t>(group_size) +
                                    static_cast<std::size_t>(j) + 1;
            if (bin < out.size()) {
                out[bin] = static_cast<std::uint8_t>(aexp[i]);
            }
        }
    }
}

}  // namespace ac3
