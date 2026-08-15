#include "ac3/core/mdct.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <utility>

#include "ac3/core/window.hpp"

namespace ac3 {

namespace {

constexpr int kN = kTransformLength;  // 512
constexpr double kPi = std::numbers::pi;

// §7.9.4.1 step 2: xcos1[k] = -cos(2pi(8k+1)/8N), xsin1[k] = -sin(2pi(8k+1)/8N).
struct Twiddles {
    std::array<double, kN / 4> cos1;
    std::array<double, kN / 4> sin1;
    Twiddles() {
        for (int k = 0; k < kN / 4; ++k) {
            const double angle = 2.0 * kPi * (8.0 * k + 1.0) / (8.0 * kN);
            cos1[static_cast<std::size_t>(k)] = -std::cos(angle);
            sin1[static_cast<std::size_t>(k)] = -std::sin(angle);
        }
    }
};

const Twiddles& twiddles() {
    static const Twiddles t;
    return t;
}

// §7.9.4.2 step 2: xcos2[k] = -cos(2pi(8k+1)/4N), xsin2[k] = -sin(2pi(8k+1)/4N)
// (N = 512 throughout this section, per the spec's own note — these are NOT
// the 256-sample transform's own N).
struct Twiddles2 {
    std::array<double, kN / 8> cos2;
    std::array<double, kN / 8> sin2;
    Twiddles2() {
        for (int k = 0; k < kN / 8; ++k) {
            const double angle = 2.0 * kPi * (8.0 * k + 1.0) / (4.0 * kN);
            cos2[static_cast<std::size_t>(k)] = -std::cos(angle);
            sin2[static_cast<std::size_t>(k)] = -std::sin(angle);
        }
    }
};

const Twiddles2& twiddles2() {
    static const Twiddles2 t;
    return t;
}

// §7.9.4.1 step 3: the N/4-point complex "IFFT" direct-form sum needs
// cos(8*pi*k*n/N) and sin(8*pi*k*n/N) for every (k, n) pair in the
// kQuarter x kQuarter grid below. This depends only on (k, n), never on the
// coefficients being transformed, so - like Twiddles/Twiddles2 above - it's
// the same fixed matrix on every call and can be computed once instead of
// (kQuarter^2 =) 16,384 fresh cos+sin pairs per transform.
//
// This is deliberately a full matrix, not a period-128 table indexed by
// (k*n) % 128 (the trick fft.cpp's dft512 uses for its own twiddles):
// std::cos(8*pi*k*n/N) reaches an UN-reduced angle of ~792 radians at
// k=n=127, and empirically std::cos of that large angle differs from
// std::cos of the small angle it's congruent to mod 2*pi by ~1.3e-13 -
// nowhere near bit-identical, just close enough to hide under this file's
// existing 1e-10 golden tolerances. Storing the exact expression this loop
// already evaluated, just once instead of every call, has no such gap.
struct InnerSumTable {
    static constexpr int kDim = kN / 4;  // 128
    std::array<std::array<double, kDim>, kDim> cos{};
    std::array<std::array<double, kDim>, kDim> sin{};
    InnerSumTable() {
        for (int n = 0; n < kDim; ++n) {
            for (int k = 0; k < kDim; ++k) {
                const double angle = 8.0 * kPi * k * n / kN;
                cos[static_cast<std::size_t>(n)][static_cast<std::size_t>(k)] = std::cos(angle);
                sin[static_cast<std::size_t>(n)][static_cast<std::size_t>(k)] = std::sin(angle);
            }
        }
    }
};

const InnerSumTable& inner_sum_table() {
    static const InnerSumTable t;
    return t;
}

// §7.9.4.2 step 3: the same idea, for the two independent N/8-point "IFFT"
// sums (angle = 16*pi*k*n/N over the kEighth x kEighth grid).
struct InnerSumPairTable {
    static constexpr int kDim = kN / 8;  // 64
    std::array<std::array<double, kDim>, kDim> cos{};
    std::array<std::array<double, kDim>, kDim> sin{};
    InnerSumPairTable() {
        for (int n = 0; n < kDim; ++n) {
            for (int k = 0; k < kDim; ++k) {
                const double angle = 16.0 * kPi * k * n / kN;
                cos[static_cast<std::size_t>(n)][static_cast<std::size_t>(k)] = std::cos(angle);
                sin[static_cast<std::size_t>(n)][static_cast<std::size_t>(k)] = std::sin(angle);
            }
        }
    }
};

const InnerSumPairTable& inner_sum_pair_table() {
    static const InnerSumPairTable t;
    return t;
}

// §8.2.3.2 direct form, generalized over the transform length and alpha:
// alpha = 0/N=512 is the long transform; alpha = -1/+1 at N=256 are the two
// halves of a block-switched block.
//
// cos(phase) depends only on (k, n, alpha), never on the windowed signal
// itself, and alpha only ever takes the three values above - so it is the
// same fixed N_len x (N_len/2) matrix on every call. This used to compute
// std::cos(phase) fresh inside the loop below, exactly like the inverse
// transform's own step 3 does NOT (imdct512_windowed/imdct256_pair_windowed
// precompute their twiddle factors via Twiddles/Twiddles2 above and reuse
// them). Measured with Tracy (docs/platforms/android.md's performance
// investigation): that recomputation was ~79% of the ENTIRE encoder's
// per-frame cost - 131,072 std::cos() calls per 512-point transform, 36
// transforms a frame (6 channels x 6 blocks). Precomputing the matrix once,
// the same way the inverse transform already does, produces bit-identical
// coefficients (same phase formula, same std::cos(), same accumulation
// order - only WHEN it runs changes) while removing that cost from the hot
// path entirely.
template <int NLen>
struct ForwardCosTable {
    static constexpr int kHalf = NLen / 2;
    std::array<std::array<double, static_cast<std::size_t>(NLen)>, static_cast<std::size_t>(kHalf)>
        value{};
    explicit ForwardCosTable(double alpha) {
        for (int k = 0; k < kHalf; ++k) {
            const double factor = 2.0 * k + 1.0;
            for (int n = 0; n < NLen; ++n) {
                const double phase = (2.0 * kPi / (4.0 * NLen)) * (2.0 * n + 1.0) * factor +
                                      (kPi / 4.0) * factor * (1.0 + alpha);
                value[static_cast<std::size_t>(k)][static_cast<std::size_t>(n)] = std::cos(phase);
            }
        }
    }
};

const ForwardCosTable<512>& forward_cos_table_long() {
    static const ForwardCosTable<512> t(0.0);
    return t;
}

const ForwardCosTable<256>& forward_cos_table_first() {
    static const ForwardCosTable<256> t(-1.0);
    return t;
}

const ForwardCosTable<256>& forward_cos_table_second() {
    static const ForwardCosTable<256> t(1.0);
    return t;
}

template <int NLen>
void mdct_forward_core(std::span<const double> windowed, const ForwardCosTable<NLen>& table,
                       std::span<double> coeffs) {
    for (int k = 0; k < NLen / 2; ++k) {
        double sum = 0.0;
        for (int n = 0; n < NLen; ++n) {
            sum += windowed[static_cast<std::size_t>(n)] *
                   table.value[static_cast<std::size_t>(k)][static_cast<std::size_t>(n)];
        }
        coeffs[static_cast<std::size_t>(k)] = (-2.0 / NLen) * sum;
    }
}

// --- §7.9.4 fast N/4-FFT structure (the encoder-config default; see
// mdct512_forward's own doc comment and EncoderConfig::fast_mdct /
// eac3::FrameConfig::fast_mdct) ---------------------------------------------
//
// Long transform (alpha = 0) ONLY. The direct-form phase is
// theta_k(n) + phi_k(alpha), phi_k(alpha) = (pi/4)(2k+1)(1+alpha) - a shift
// that depends on k but never n - and phi_k(0) = (pi/4)(2k+1) is exactly the
// "+ N/4" term folded into the standard MDCT formula this fold computes
// (X[k] = (-2/N) sum x[n] cos(2pi/N (n+1/2+N/4)(k+1/2))). That term is NOT
// zero, so it does not vanish for alpha = 0 - a fact worth stating plainly
// because an earlier version of this comment claimed alpha = -1 (phi_k = 0,
// the BARE cosine sum with no N/4 shift at all) was "the same formula" as
// alpha = 0 "just at a different NLen". It is not: phi_k(0) != phi_k(-1), so
// X_0 and X_{-1} are two different transforms of the same data, and a
// standalone numerical check (comparing this exact fold against a hand
// reference implementing each phase separately) confirmed the fold below
// reproduces X_0 to ~1e-15 but is off by 100%+ against X_{-1}. Both
// mdct256_forward_first (alpha = -1) and mdct256_forward_second (alpha = +1)
// therefore have NO accelerated path today - each needs its own fold,
// independently derived and verified against ITS OWN direct-form table,
// which is future work. Their `fast` parameters exist for interface
// symmetry with mdct512_forward's but currently always take the direct
// (already table-cached, already bit-exact) path.
//
// The fold below computes DCT-IV(u) - u the length-M "folded" input built
// from the four quarters of the windowed NLen-sample block - via one
// P = M/2-point complex FFT, the standard trick for a real-input DCT-IV.
// Verified 2026-08-14 against ForwardCosTable (this file's own direct-form
// ground truth) to max relative error ~3e-12 on both random data and real
// audio; see tests/test_mdct_fast.cpp, which asserts a 1e-10 bound.

// Everything angle-dependent in the fold below, computed once per NLen -
// the same treatment Twiddles/InnerSumTable/ForwardCosTable give every
// other transform in this file, applied to the fast path itself (phase-5
// target 1 of the performance programme: this kernel runs 36x per frame in
// every encode path, plus 6x per object per frame inside band_energy, and
// its per-call cost was dominated by the 512 std::cos/std::sin libm calls
// below being made fresh on every transform). Two exactness classes:
//
// - pre/post twiddles: the EXACT expressions the fold used to evaluate per
//   call (std::cos/std::sin of -pi*m/M and -pi*(4k+1)/(4M)), stored instead
//   of re-evaluated - bit-identical values, InnerSumTable's own reasoning.
// - FFT stage twiddles + bit-reversal permutation: the previous in-place
//   FFT generated each butterfly group's j-th twiddle by ITERATED complex
//   multiply (w *= wlen), so it carried j-1 accumulated rounding steps.
//   The table stores std::cos/std::sin of each exact angle -2*pi*j/len
//   instead - a (tiny) numerical change in the direction of MORE precision,
//   re-verified against the direct form's ground truth by
//   tests/test_mdct_fast.cpp's unchanged 1e-10 bound.
template <int NLen>
struct FastMdctTables {
    static constexpr std::size_t kM = static_cast<std::size_t>(NLen) / 2;
    static constexpr std::size_t kP = kM / 2;
    // z[m] pre-twiddle exp(-i*pi*m/M), split re/im.
    std::array<double, kP> pre_re{};
    std::array<double, kP> pre_im{};
    // w[k] post-twiddle exp(-i*pi*(4k+1)/(4M)), split re/im.
    std::array<double, kP> post_re{};
    std::array<double, kP> post_im{};
    // Bit-reversal permutation of 0..P-1 for the decimation-in-time FFT.
    std::array<std::uint16_t, kP> bitrev{};
    // FFT stage twiddles exp(-2*pi*i*j/len) for len = 2, 4, ..., P and
    // j < len/2, flattened at offset len/2 - 1: stage `len` holds len/2
    // entries, so the stages pack exactly into P - 1 slots.
    std::array<double, kP - 1> stage_re{};
    std::array<double, kP - 1> stage_im{};
    FastMdctTables() {
        for (std::size_t m = 0; m < kP; ++m) {
            const double ang = -kPi * static_cast<double>(m) / static_cast<double>(kM);
            pre_re[m] = std::cos(ang);
            pre_im[m] = std::sin(ang);
            const double ang2 =
                -kPi * (4.0 * static_cast<double>(m) + 1.0) / (4.0 * static_cast<double>(kM));
            post_re[m] = std::cos(ang2);
            post_im[m] = std::sin(ang2);
        }
        for (std::size_t i = 1; i < kP; ++i) {
            bitrev[i] = static_cast<std::uint16_t>(
                (bitrev[i >> 1] >> 1) | ((i & 1) != 0 ? kP / 2 : 0));
        }
        for (std::size_t len = 2; len <= kP; len <<= 1) {
            const std::size_t half = len / 2;
            for (std::size_t j = 0; j < half; ++j) {
                const double ang =
                    -2.0 * kPi * static_cast<double>(j) / static_cast<double>(len);
                stage_re[half - 1 + j] = std::cos(ang);
                stage_im[half - 1 + j] = std::sin(ang);
            }
        }
    }
};

template <int NLen>
const FastMdctTables<NLen>& fast_mdct_tables() {
    static const FastMdctTables<NLen> t;
    return t;
}

// Iterative radix-2 decimation-in-time FFT, in place over separate re/im
// arrays: on return (re, im) hold A[k] = sum_m a[m] * exp(-2*pi*i*m*k/P) for
// k = 0..P-1 (unnormalized forward transform). Split arrays rather than
// std::complex so the butterfly's four independent multiply-add chains stay
// visible to the auto-vectorizer; twiddles and the bit-reversal permutation
// come from FastMdctTables above instead of being regenerated per call.
template <int NLen>
void fft_forward_pow2(const FastMdctTables<NLen>& t,
                      std::array<double, FastMdctTables<NLen>::kP>& re,
                      std::array<double, FastMdctTables<NLen>::kP>& im) {
    constexpr std::size_t P = FastMdctTables<NLen>::kP;
    for (std::size_t i = 1; i < P; ++i) {
        const std::size_t j = t.bitrev[i];
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (std::size_t len = 2; len <= P; len <<= 1) {
        const std::size_t half = len / 2;
        for (std::size_t i = 0; i < P; i += len) {
            for (std::size_t j = 0; j < half; ++j) {
                const double wr = t.stage_re[half - 1 + j];
                const double wi = t.stage_im[half - 1 + j];
                const double xr = re[i + j + half];
                const double xi = im[i + j + half];
                const double vr = xr * wr - xi * wi;
                const double vi = xr * wi + xi * wr;
                const double ur = re[i + j];
                const double ui = im[i + j];
                re[i + j] = ur + vr;
                im[i + j] = ui + vi;
                re[i + j + half] = ur - vr;
                im[i + j + half] = ui - vi;
            }
        }
    }
}

// DCT-IV-via-FFT fast path for alpha in {0, -1} (see the block comment
// above): NLen is the transform's own length (512 long, 256 first-short),
// M = NLen/2, Q = NLen/4, P = M/2. Quarters of `windowed`: a = [0,Q),
// b = [Q,2Q), c = [2Q,3Q), d = [3Q,4Q). u = concat(-c_R - d, a - b_R),
// R = reversed, length M. z[m] = (u[2m] + i*u[M-1-2m]) * exp(-i*pi*m/M);
// Z = FFT_P(z); w[k] = Z[k] * exp(-i*pi*(4k+1)/(4M)); X[2k] = Re(w[k]),
// X[M-1-2k] = -Im(w[k]); coeffs = (-2/NLen) * X.
template <int NLen>
void mdct_forward_fast_core(std::span<const double> windowed, std::span<double> coeffs) {
    constexpr std::size_t Q = static_cast<std::size_t>(NLen) / 4;
    constexpr std::size_t M = FastMdctTables<NLen>::kM;
    constexpr std::size_t P = FastMdctTables<NLen>::kP;
    const auto& t = fast_mdct_tables<NLen>();

    std::array<double, M> u{};
    for (std::size_t i = 0; i < Q; ++i) {
        // -c_R[i] - d[i] = -windowed[3Q-1-i] - windowed[3Q+i]
        u[i] = -windowed[3 * Q - 1 - i] - windowed[3 * Q + i];
    }
    for (std::size_t j = 0; j < Q; ++j) {
        // a[j] - b_R[j] = windowed[j] - windowed[2Q-1-j]
        u[Q + j] = windowed[j] - windowed[2 * Q - 1 - j];
    }

    std::array<double, P> z_re{};
    std::array<double, P> z_im{};
    for (std::size_t m = 0; m < P; ++m) {
        const double a = u[2 * m];
        const double b = u[M - 1 - 2 * m];
        z_re[m] = a * t.pre_re[m] - b * t.pre_im[m];
        z_im[m] = a * t.pre_im[m] + b * t.pre_re[m];
    }
    fft_forward_pow2<NLen>(t, z_re, z_im);

    for (std::size_t k = 0; k < P; ++k) {
        const double wr = z_re[k] * t.post_re[k] - z_im[k] * t.post_im[k];
        const double wi = z_re[k] * t.post_im[k] + z_im[k] * t.post_re[k];
        coeffs[2 * k] = (-2.0 / NLen) * wr;
        coeffs[M - 1 - 2 * k] = (-2.0 / NLen) * (-wi);
    }
}

}  // namespace

void apply_analysis_window(std::span<const double, 512> x, std::span<double, 512> windowed) {
    for (int n = 0; n < kN; ++n) {
        windowed[static_cast<std::size_t>(n)] =
            x[static_cast<std::size_t>(n)] * kAnalysisWindow[static_cast<std::size_t>(n)];
    }
}

void mdct512_forward(std::span<const double, 512> windowed, std::span<double, 256> coeffs,
                     bool fast) {
    if (fast) {
        mdct_forward_fast_core<512>(windowed, coeffs);
    } else {
        mdct_forward_core<512>(windowed, forward_cos_table_long(), coeffs);
    }
}

void mdct256_forward_first(std::span<const double, 256> windowed, std::span<double, 128> coeffs,
                           bool fast) {
    // alpha = -1 is the BARE cosine sum (phi_k = 0, no "+N/4" phase shift at
    // all) - a genuinely different transform from alpha = 0's, which
    // mdct_forward_fast_core computes (see its own comment for why an
    // earlier version of this file wrongly treated the two as identical).
    // Its fast fold is independently-derived future work; `fast` is accepted
    // for interface symmetry with mdct512_forward's but always takes the
    // direct, already-cached, already-bit-exact path today.
    (void)fast;
    mdct_forward_core<256>(windowed, forward_cos_table_first(), coeffs);
}

void mdct256_forward_second(std::span<const double, 256> windowed, std::span<double, 128> coeffs,
                            bool fast) {
    // alpha = +1's phase shift is a genuine sine-kernel (DST-IV-shaped) sum,
    // not a phase-shifted copy of the cosine one mdct_forward_fast_core
    // computes - see mdct_forward_fast_core's own comment. Its fast fold is
    // future work, independently derived and verified the same way the
    // long transform's was; `fast` is accepted here for interface symmetry
    // with its two siblings (all three take the SAME parameter, so a caller
    // does not need to know which transform actually accelerates) but
    // always takes the direct, already-cached, already-bit-exact path today.
    (void)fast;
    mdct_forward_core<256>(windowed, forward_cos_table_second(), coeffs);
}

void imdct512_windowed(std::span<const double, 256> coeffs, std::span<double, 512> x) {
    const auto& tw = twiddles();
    constexpr int kQuarter = kN / 4;  // 128
    constexpr int kEighth = kN / 8;   // 64

    // Step 2: pre-transform complex multiply.
    // Z[k] = (X[N/2-2k-1] + j*X[2k]) * (xcos1[k] + j*xsin1[k])
    std::array<double, kQuarter> z_re{};
    std::array<double, kQuarter> z_im{};
    for (int k = 0; k < kQuarter; ++k) {
        const double a = coeffs[static_cast<std::size_t>(kN / 2 - 2 * k - 1)];
        const double b = coeffs[static_cast<std::size_t>(2 * k)];
        const double c = tw.cos1[static_cast<std::size_t>(k)];
        const double s = tw.sin1[static_cast<std::size_t>(k)];
        z_re[static_cast<std::size_t>(k)] = a * c - b * s;
        z_im[static_cast<std::size_t>(k)] = b * c + a * s;
    }

    // Step 3: N/4-point complex "IFFT" exactly as the pseudocode sums it:
    // z[n] = sum_k Z[k] * (cos(8*pi*k*n/N) + j*sin(8*pi*k*n/N)), no scaling.
    const auto& s3 = inner_sum_table();
    std::array<double, kQuarter> t_re{};
    std::array<double, kQuarter> t_im{};
    for (int n = 0; n < kQuarter; ++n) {
        double re = 0.0;
        double im = 0.0;
        const auto& row_c = s3.cos[static_cast<std::size_t>(n)];
        const auto& row_s = s3.sin[static_cast<std::size_t>(n)];
        for (int k = 0; k < kQuarter; ++k) {
            const double c = row_c[static_cast<std::size_t>(k)];
            const double s = row_s[static_cast<std::size_t>(k)];
            re += z_re[static_cast<std::size_t>(k)] * c - z_im[static_cast<std::size_t>(k)] * s;
            im += z_re[static_cast<std::size_t>(k)] * s + z_im[static_cast<std::size_t>(k)] * c;
        }
        t_re[static_cast<std::size_t>(n)] = re;
        t_im[static_cast<std::size_t>(n)] = im;
    }

    // Step 4: post-transform complex multiply. y[n] = z[n] * (xcos1[n] + j*xsin1[n])
    std::array<double, kQuarter> y_re{};
    std::array<double, kQuarter> y_im{};
    for (int n = 0; n < kQuarter; ++n) {
        const double c = tw.cos1[static_cast<std::size_t>(n)];
        const double s = tw.sin1[static_cast<std::size_t>(n)];
        y_re[static_cast<std::size_t>(n)] =
            t_re[static_cast<std::size_t>(n)] * c - t_im[static_cast<std::size_t>(n)] * s;
        y_im[static_cast<std::size_t>(n)] =
            t_im[static_cast<std::size_t>(n)] * c + t_re[static_cast<std::size_t>(n)] * s;
    }

    // Step 5: windowing and de-interleaving, transcribed field-for-field.
    const auto& w = kAnalysisWindow;
    const auto yr = [&](int i) { return y_re[static_cast<std::size_t>(i)]; };
    const auto yi = [&](int i) { return y_im[static_cast<std::size_t>(i)]; };
    for (int n = 0; n < kEighth; ++n) {
        x[static_cast<std::size_t>(2 * n)] = -yi(kEighth + n) * w[static_cast<std::size_t>(2 * n)];
        x[static_cast<std::size_t>(2 * n + 1)] =
            yr(kEighth - n - 1) * w[static_cast<std::size_t>(2 * n + 1)];
        x[static_cast<std::size_t>(kQuarter + 2 * n)] =
            -yr(n) * w[static_cast<std::size_t>(kQuarter + 2 * n)];
        x[static_cast<std::size_t>(kQuarter + 2 * n + 1)] =
            yi(kQuarter - n - 1) * w[static_cast<std::size_t>(kQuarter + 2 * n + 1)];
        x[static_cast<std::size_t>(kN / 2 + 2 * n)] =
            -yr(kEighth + n) * w[static_cast<std::size_t>(kN / 2 - 2 * n - 1)];
        x[static_cast<std::size_t>(kN / 2 + 2 * n + 1)] =
            yi(kEighth - n - 1) * w[static_cast<std::size_t>(kN / 2 - 2 * n - 2)];
        x[static_cast<std::size_t>(3 * kN / 4 + 2 * n)] =
            yi(n) * w[static_cast<std::size_t>(kQuarter - 2 * n - 1)];
        x[static_cast<std::size_t>(3 * kN / 4 + 2 * n + 1)] =
            -yr(kQuarter - n - 1) * w[static_cast<std::size_t>(kQuarter - 2 * n - 2)];
    }
}

void imdct256_pair_windowed(std::span<const double, 256> coeffs, std::span<double, 512> x) {
    const auto& tw = twiddles2();
    constexpr int kQuarter = kN / 4;  // 128
    constexpr int kEighth = kN / 8;   // 64

    // Step 1: de-interleave the 256 coefficients into the two half-block sets.
    std::array<double, kQuarter> x1{};
    std::array<double, kQuarter> x2{};
    for (int k = 0; k < kQuarter; ++k) {
        x1[static_cast<std::size_t>(k)] = coeffs[static_cast<std::size_t>(2 * k)];
        x2[static_cast<std::size_t>(k)] = coeffs[static_cast<std::size_t>(2 * k + 1)];
    }

    // Step 2: pre-IFFT complex multiply.
    // Z1[k] = (X1[N/4-2k-1] + j*X1[2k]) * (xcos2[k] + j*xsin2[k]), likewise Z2.
    std::array<double, kEighth> z1_re{};
    std::array<double, kEighth> z1_im{};
    std::array<double, kEighth> z2_re{};
    std::array<double, kEighth> z2_im{};
    for (int k = 0; k < kEighth; ++k) {
        const double c = tw.cos2[static_cast<std::size_t>(k)];
        const double s = tw.sin2[static_cast<std::size_t>(k)];
        const double a1 = x1[static_cast<std::size_t>(kQuarter - 2 * k - 1)];
        const double b1 = x1[static_cast<std::size_t>(2 * k)];
        z1_re[static_cast<std::size_t>(k)] = a1 * c - b1 * s;
        z1_im[static_cast<std::size_t>(k)] = b1 * c + a1 * s;
        const double a2 = x2[static_cast<std::size_t>(kQuarter - 2 * k - 1)];
        const double b2 = x2[static_cast<std::size_t>(2 * k)];
        z2_re[static_cast<std::size_t>(k)] = a2 * c - b2 * s;
        z2_im[static_cast<std::size_t>(k)] = b2 * c + a2 * s;
    }

    // Step 3: two independent N/8-point complex "IFFT" sums, unscaled.
    const auto& s3 = inner_sum_pair_table();
    std::array<double, kEighth> t1_re{};
    std::array<double, kEighth> t1_im{};
    std::array<double, kEighth> t2_re{};
    std::array<double, kEighth> t2_im{};
    for (int n = 0; n < kEighth; ++n) {
        double re1 = 0.0;
        double im1 = 0.0;
        double re2 = 0.0;
        double im2 = 0.0;
        const auto& row_c = s3.cos[static_cast<std::size_t>(n)];
        const auto& row_s = s3.sin[static_cast<std::size_t>(n)];
        for (int k = 0; k < kEighth; ++k) {
            const double c = row_c[static_cast<std::size_t>(k)];
            const double s = row_s[static_cast<std::size_t>(k)];
            re1 += z1_re[static_cast<std::size_t>(k)] * c - z1_im[static_cast<std::size_t>(k)] * s;
            im1 += z1_re[static_cast<std::size_t>(k)] * s + z1_im[static_cast<std::size_t>(k)] * c;
            re2 += z2_re[static_cast<std::size_t>(k)] * c - z2_im[static_cast<std::size_t>(k)] * s;
            im2 += z2_re[static_cast<std::size_t>(k)] * s + z2_im[static_cast<std::size_t>(k)] * c;
        }
        t1_re[static_cast<std::size_t>(n)] = re1;
        t1_im[static_cast<std::size_t>(n)] = im1;
        t2_re[static_cast<std::size_t>(n)] = re2;
        t2_im[static_cast<std::size_t>(n)] = im2;
    }

    // Step 4: post-IFFT complex multiply. y1[n] = z1[n] * (xcos2[n] + j*xsin2[n]).
    std::array<double, kEighth> y1_re{};
    std::array<double, kEighth> y1_im{};
    std::array<double, kEighth> y2_re{};
    std::array<double, kEighth> y2_im{};
    for (int n = 0; n < kEighth; ++n) {
        const double c = tw.cos2[static_cast<std::size_t>(n)];
        const double s = tw.sin2[static_cast<std::size_t>(n)];
        y1_re[static_cast<std::size_t>(n)] =
            t1_re[static_cast<std::size_t>(n)] * c - t1_im[static_cast<std::size_t>(n)] * s;
        y1_im[static_cast<std::size_t>(n)] =
            t1_im[static_cast<std::size_t>(n)] * c + t1_re[static_cast<std::size_t>(n)] * s;
        y2_re[static_cast<std::size_t>(n)] =
            t2_re[static_cast<std::size_t>(n)] * c - t2_im[static_cast<std::size_t>(n)] * s;
        y2_im[static_cast<std::size_t>(n)] =
            t2_im[static_cast<std::size_t>(n)] * c + t2_re[static_cast<std::size_t>(n)] * s;
    }

    // Step 5: windowing and de-interleaving, transcribed field-for-field.
    // N is 512 throughout (the spec's own note), so this reaches the same
    // full x[0..511] the long path's step 5 does.
    const auto& w = kAnalysisWindow;
    const auto y1r = [&](int i) { return y1_re[static_cast<std::size_t>(i)]; };
    const auto y1i = [&](int i) { return y1_im[static_cast<std::size_t>(i)]; };
    const auto y2r = [&](int i) { return y2_re[static_cast<std::size_t>(i)]; };
    const auto y2i = [&](int i) { return y2_im[static_cast<std::size_t>(i)]; };
    for (int n = 0; n < kEighth; ++n) {
        x[static_cast<std::size_t>(2 * n)] = -y1i(n) * w[static_cast<std::size_t>(2 * n)];
        x[static_cast<std::size_t>(2 * n + 1)] =
            y1r(kEighth - n - 1) * w[static_cast<std::size_t>(2 * n + 1)];
        x[static_cast<std::size_t>(kQuarter + 2 * n)] =
            -y1r(n) * w[static_cast<std::size_t>(kQuarter + 2 * n)];
        x[static_cast<std::size_t>(kQuarter + 2 * n + 1)] =
            y1i(kEighth - n - 1) * w[static_cast<std::size_t>(kQuarter + 2 * n + 1)];
        x[static_cast<std::size_t>(kN / 2 + 2 * n)] =
            -y2r(n) * w[static_cast<std::size_t>(kN / 2 - 2 * n - 1)];
        x[static_cast<std::size_t>(kN / 2 + 2 * n + 1)] =
            y2i(kEighth - n - 1) * w[static_cast<std::size_t>(kN / 2 - 2 * n - 2)];
        x[static_cast<std::size_t>(3 * kN / 4 + 2 * n)] =
            y2i(n) * w[static_cast<std::size_t>(kQuarter - 2 * n - 1)];
        x[static_cast<std::size_t>(3 * kN / 4 + 2 * n + 1)] =
            -y2r(kEighth - n - 1) * w[static_cast<std::size_t>(kQuarter - 2 * n - 2)];
    }
}

}  // namespace ac3
