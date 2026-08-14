// Per-kernel micro-benchmarks: the OTHER half of Phase 2 of the performance-
// observability programme, alongside bench_encoder.cpp's whole-frame trend
// numbers and test_performance.cpp's real-time gate. Where those measure a
// whole encode_frame() call, this isolates the individual DSP/bitstream
// kernels Tracy's frame-level zones (docs/platforms/android.md,
// src/lib/src/internal/profiling/) name, so a capture that shows "step2_mdct
// is 60% of the frame" has a companion number for what mdct512_forward alone
// costs per call - together they say both WHERE the time goes and HOW MUCH
// of it is the kernel's own algorithm versus how often it is invoked.
//
// Not a Catch2 binary and not ctest-registered, on purpose - the same
// reasoning as bench_encoder.cpp's own header comment: nothing here asserts
// anything, it is a numbers producer for a human (or a future trend script)
// to read, not a pass/fail gate.
//
// Every kernel is fed inputs derived from a real (if synthetic-tone) signal
// pushed through the real forward pipeline (apply_analysis_window +
// mdct512_forward, exactly as encoder.cpp/eac3_frame.cpp windows a block) -
// never all-zero silence. A bit allocator or quantizer given silence takes
// early-exit paths a real signal never takes (see the codebase's own
// "silence gives false passes" lesson for the correctness-test side of the
// same problem), so a benchmark built on those paths would time the wrong
// thing.

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "ac3/core/bitalloc.hpp"
#include "ac3/core/exponents.hpp"
#include "ac3/core/mantissas.hpp"
#include "ac3/core/mdct.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/encoder/eac3_tools.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/oba/joc_tables.hpp"

namespace {

constexpr double kSampleRateHz = 48000.0;
// Enough consecutive blocks for the AHT kernel (needs 6) and
// ecpl_channel_spectrum (needs 3 consecutive), with headroom.
constexpr int kBenchBlocks = 12;
// compute_bit_allocation's own endmant ceiling (bitalloc.cpp asserts
// end <= 253) - the largest a real fbw/coupling stream's bin count ever
// reaches, so exponent/bap kernels below work on this many bins of a
// block's 256 real MDCT coefficients, not all of them.
constexpr int kBins = 253;

// A few tones at different frequencies/amplitudes rather than one pure note -
// closer to real program material's spread of exponents and bap values than
// a single sine - and deterministic, so the numbers are reproducible run to
// run without needing a fixed random seed.
std::vector<float> tone_signal(int samples) {
    std::vector<float> out(static_cast<std::size_t>(samples));
    for (int n = 0; n < samples; ++n) {
        const double t = static_cast<double>(n) / kSampleRateHz;
        const double s = 0.28 * std::sin(2.0 * std::numbers::pi * 440.0 * t) +
                         0.18 * std::sin(2.0 * std::numbers::pi * 1108.0 * t) +
                         0.09 * std::sin(2.0 * std::numbers::pi * 3729.0 * t);
        out[static_cast<std::size_t>(n)] = static_cast<float>(s);
    }
    return out;
}

// The real forward pipeline's own windowing convention (encoder.cpp/
// eac3_frame.cpp step 1/2): block b's 512-sample window covers
// pcm[b*256-256 .. b*256+256), samples before 0 treated as silence - exactly
// a stream's opening frame, where there is no prior-block history yet.
struct RealFrame {
    std::vector<float> pcm;                       // kBenchBlocks * 256 samples
    std::vector<std::array<double, 256>> coeffs;  // one 512->256 MDCT per block
};

RealFrame build_real_frame() {
    RealFrame frame;
    frame.pcm = tone_signal(kBenchBlocks * ac3::kSamplesPerBlock);
    frame.coeffs.resize(static_cast<std::size_t>(kBenchBlocks));
    for (int block = 0; block < kBenchBlocks; ++block) {
        std::array<double, 512> time{};
        for (int n = 0; n < 512; ++n) {
            const int pos = block * 256 - 256 + n;
            time[static_cast<std::size_t>(n)] =
                pos < 0 ? 0.0 : static_cast<double>(frame.pcm[static_cast<std::size_t>(pos)]);
        }
        std::array<double, 512> windowed{};
        ac3::apply_analysis_window(time, windowed);
        ac3::mdct512_forward(windowed, frame.coeffs[static_cast<std::size_t>(block)]);
    }
    return frame;
}

// §8.2.7/§8.2.10 in miniature: real per-bin exponents extracted from a real
// block's coefficients, then run through the real encode/decode round trip -
// exactly the decoder-mirror rule exponents.hpp documents - so downstream
// kernels see the same decoded exponents compute_bit_allocation would in a
// real frame, not a hand-picked array.
std::array<std::uint8_t, kBins> real_decoded_exponents(const std::array<double, 256>& coeffs) {
    std::array<std::uint8_t, kBins> raw{};
    for (int i = 0; i < kBins; ++i) {
        const auto fixed = ac3::to_fixed25(coeffs[static_cast<std::size_t>(i)]);
        raw[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(ac3::exponent_from_fixed(fixed));
    }
    const auto encoded = ac3::encode_exponents(raw, ac3::ExpStrategy::kD15);
    std::array<std::uint8_t, kBins> decoded{};
    ac3::decode_exponents(encoded.absolute, encoded.groups, ac3::ExpStrategy::kD15, decoded);
    return decoded;
}

struct Result {
    std::string name;
    long long iters = 0;
    double ns_per_call = 0.0;
};

// Runs `body(i)` `iters` times and reports the average wall time per call.
// `body` returns a double folded into a running sum that this function
// prints via the returned Result's caller - so no call inside `body` can be
// proven dead and elided, the usual microbenchmark hazard. (In practice
// every kernel here is a call across the ac3::forge shared-library boundary,
// which already blocks inlining/elision on its own - this is cheap
// insurance on top of that, not the only thing preventing it.)
template <class Fn>
Result time_kernel(std::string name, long long iters, Fn&& body) {
    double sink = 0.0;
    const auto start = std::chrono::steady_clock::now();
    for (long long i = 0; i < iters; ++i) {
        sink += body(i);
    }
    const auto elapsed =
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start);
    if (!std::isfinite(sink)) {
        std::fprintf(stderr, "%s: non-finite accumulator - inputs may be degenerate\n",
                    name.c_str());
        std::exit(1);
    }
    return {.name = std::move(name), .iters = iters,
            .ns_per_call = elapsed.count() / static_cast<double>(iters)};
}

Result bench_mdct512_forward(const RealFrame& frame) {
    std::array<double, 512> time{};
    for (int n = 0; n < 512; ++n) {
        time[static_cast<std::size_t>(n)] = static_cast<double>(frame.pcm[static_cast<std::size_t>(n)]);
    }
    std::array<double, 512> windowed{};
    ac3::apply_analysis_window(time, windowed);
    std::array<double, 256> coeffs{};
    return time_kernel("mdct512_forward", 20000, [&](long long) {
        ac3::mdct512_forward(windowed, coeffs);
        return coeffs[0];
    });
}

Result bench_mdct256_pair(const RealFrame& frame) {
    std::array<double, 512> time{};
    for (int n = 0; n < 512; ++n) {
        time[static_cast<std::size_t>(n)] = static_cast<double>(frame.pcm[static_cast<std::size_t>(n)]);
    }
    std::array<double, 512> windowed{};
    ac3::apply_analysis_window(time, windowed);
    const std::span<const double, 512> full(windowed);
    std::array<double, 128> first{};
    std::array<double, 128> second{};
    return time_kernel("mdct256_pair", 20000, [&](long long) {
        ac3::mdct256_forward_first(full.first<256>(), first);
        ac3::mdct256_forward_second(full.last<256>(), second);
        return first[0] + second[0];
    });
}

Result bench_imdct512_windowed(const RealFrame& frame) {
    const auto& coeffs = frame.coeffs[0];
    std::array<double, 512> x{};
    return time_kernel("imdct512_windowed", 20000, [&](long long) {
        ac3::imdct512_windowed(coeffs, x);
        return x[0];
    });
}

Result bench_compute_bit_allocation(const RealFrame& frame) {
    const auto decoded = real_decoded_exponents(frame.coeffs[0]);
    std::array<std::uint8_t, kBins> bap{};
    const ac3::BitAllocCodes codes{};
    // A mid-range composite (csnroffst 25, fsnroffst 4), not the 0/0 all-zero
    // special case - a real allocator search spends almost none of its time
    // at that degenerate corner.
    return time_kernel("compute_bit_allocation", 10000, [&](long long) {
        ac3::compute_bit_allocation(decoded, ac3::SampleRate::k48000, codes, 25, 4, bap);
        return static_cast<double>(bap[10]);
    });
}

Result bench_encode_exponents(const RealFrame& frame) {
    std::array<std::uint8_t, kBins> raw{};
    for (int i = 0; i < kBins; ++i) {
        const auto fixed = ac3::to_fixed25(frame.coeffs[0][static_cast<std::size_t>(i)]);
        raw[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(ac3::exponent_from_fixed(fixed));
    }
    return time_kernel("encode_exponents", 20000, [&](long long) {
        const auto encoded = ac3::encode_exponents(raw, ac3::ExpStrategy::kD15);
        return static_cast<double>(encoded.absolute);
    });
}

Result bench_mantissa_bits_per_block(const RealFrame& frame) {
    const auto decoded0 = real_decoded_exponents(frame.coeffs[0]);
    const auto decoded1 = real_decoded_exponents(frame.coeffs[1]);
    std::array<std::uint8_t, kBins> bap0{};
    std::array<std::uint8_t, kBins> bap1{};
    const ac3::BitAllocCodes codes{};
    ac3::compute_bit_allocation(decoded0, ac3::SampleRate::k48000, codes, 25, 4, bap0);
    ac3::compute_bit_allocation(decoded1, ac3::SampleRate::k48000, codes, 25, 4, bap1);
    const std::array<std::span<const std::uint8_t>, 2> channel_baps{bap0, bap1};
    return time_kernel("mantissa_bits_per_block", 100000,
                       [&](long long) { return static_cast<double>(ac3::mantissa_bits_per_block(channel_baps)); });
}

Result bench_quantize_mantissa_loop(const RealFrame& frame) {
    const auto decoded = real_decoded_exponents(frame.coeffs[0]);
    std::array<std::uint8_t, kBins> bap{};
    const ac3::BitAllocCodes codes{};
    ac3::compute_bit_allocation(decoded, ac3::SampleRate::k48000, codes, 25, 4, bap);
    std::array<std::int32_t, kBins> mantissas{};
    for (int i = 0; i < kBins; ++i) {
        const int exp = decoded[static_cast<std::size_t>(i)];
        const auto fixed = ac3::to_fixed25(frame.coeffs[0][static_cast<std::size_t>(i)]);
        mantissas[static_cast<std::size_t>(i)] =
            static_cast<std::int32_t>(static_cast<std::int64_t>(fixed) << exp);
    }
    return time_kernel("quantize_mantissa_loop_253bins", 20000, [&](long long) {
        std::uint32_t sink = 0;
        for (int i = 0; i < kBins; ++i) {
            const int b = bap[static_cast<std::size_t>(i)];
            if (b == 0) {
                continue;
            }
            sink += ac3::quantize_mantissa(mantissas[static_cast<std::size_t>(i)], b);
        }
        return static_cast<double>(sink);
    });
}

Result bench_aht_forward_and_vector_quantize(const RealFrame& frame) {
    using ac3::eac3::kBlocksPerFrameSize;
    constexpr int kBin = 60;
    std::array<double, kBlocksPerFrameSize> blocks{};
    for (std::size_t j = 0; j < kBlocksPerFrameSize; ++j) {
        blocks[j] = frame.coeffs[j][static_cast<std::size_t>(kBin)];
    }
    return time_kernel("aht_forward_plus_vector_quantize", 20000, [&](long long) {
        std::array<double, kBlocksPerFrameSize> transformed{};
        ac3::eac3::aht_forward(blocks, transformed);
        // aht_vector_quantize mutates its input in place with the codebook's
        // own reconstruction, so every iteration needs its own scratch copy
        // to keep quantizing the SAME real transform output rather than an
        // already-quantized one.
        auto scratch = transformed;
        const int index = ac3::eac3::aht_vector_quantize(scratch, 4);
        return static_cast<double>(index) + scratch[0];
    });
}

Result bench_ecpl_channel_spectrum(const RealFrame& frame) {
    const std::span<const double, 256> prev(frame.coeffs[0]);
    const std::span<const double, 256> curr(frame.coeffs[1]);
    const std::span<const double, 256> next(frame.coeffs[2]);
    std::array<double, 256> real_out{};
    std::array<double, 256> imag_out{};
    return time_kernel("ecpl_channel_spectrum", 5000, [&](long long) {
        ac3::eac3::ecpl_channel_spectrum(prev, curr, next, real_out, imag_out);
        return real_out[0];
    });
}

Result bench_band_energy(const RealFrame& frame) {
    // Matches AtmosConfig's own default num_bands_idx (see atmos.hpp).
    constexpr std::size_t kNumBandsIdx = 4;
    const auto& mapping = ac3::joc::kSubbandToBand[kNumBandsIdx];
    const int bands = ac3::joc::kNumBands[kNumBandsIdx];
    const std::span<const float> signal(frame.pcm.data(),
                                        static_cast<std::size_t>(ac3::kSamplesPerFrame));
    std::vector<double> out(static_cast<std::size_t>(bands));
    return time_kernel("band_energy", 5000, [&](long long) {
        ac3::oba::band_energy(signal, mapping, out);
        return out[0];
    });
}

// One full bits_at evaluation: the shape encoder.cpp's and eac3_frame.cpp's
// own SNR-search lambda both share (compute_bit_allocation per stream, then
// mantissa_bits_per_block per block), here for a plausible 2-channel stereo
// case over a full 6-block frame of real exponents.
Result bench_bits_at(const RealFrame& frame) {
    constexpr int kStreams = 2;
    std::array<std::array<std::uint8_t, kBins>, kStreams> decoded{};
    std::array<std::array<std::uint8_t, kBins>, kStreams> bap{};
    for (int s = 0; s < kStreams; ++s) {
        decoded[static_cast<std::size_t>(s)] =
            real_decoded_exponents(frame.coeffs[static_cast<std::size_t>(s)]);
    }
    const ac3::BitAllocCodes codes{};
    std::array<std::span<const std::uint8_t>, kStreams> bap_views{};
    return time_kernel("bits_at_2stream_6block", 5000, [&](long long) {
        for (int s = 0; s < kStreams; ++s) {
            ac3::compute_bit_allocation(decoded[static_cast<std::size_t>(s)], ac3::SampleRate::k48000,
                                        codes, 25, 4, bap[static_cast<std::size_t>(s)]);
            bap_views[static_cast<std::size_t>(s)] = bap[static_cast<std::size_t>(s)];
        }
        std::size_t total = 0;
        for (int block = 0; block < ac3::kBlocksPerFrame; ++block) {
            total += ac3::mantissa_bits_per_block(bap_views);
        }
        return static_cast<double>(total);
    });
}

void write_json(const std::vector<Result>& results, const std::string& path) {
    std::ofstream out(path);
    out << "{\n  \"results\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        out << "    {\"name\": \"" << r.name << "\", \"iters\": " << r.iters
            << ", \"ns_per_call\": " << r.ns_per_call << "}" << (i + 1 < results.size() ? "," : "")
            << "\n";
    }
    out << "  ]\n}\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string json_out;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--json-out" && i + 1 < argc) {
            json_out = argv[++i];
        }
    }

    const RealFrame frame = build_real_frame();

    const std::vector<Result> results{
        bench_mdct512_forward(frame),
        bench_mdct256_pair(frame),
        bench_imdct512_windowed(frame),
        bench_compute_bit_allocation(frame),
        bench_encode_exponents(frame),
        bench_mantissa_bits_per_block(frame),
        bench_quantize_mantissa_loop(frame),
        bench_aht_forward_and_vector_quantize(frame),
        bench_ecpl_channel_spectrum(frame),
        bench_band_energy(frame),
        bench_bits_at(frame),
    };

    for (const auto& r : results) {
        std::printf("%-34s %8lld iters  %10.1f ns/call\n", r.name.c_str(), r.iters, r.ns_per_call);
    }

    if (!json_out.empty()) {
        write_json(results, json_out);
        std::printf("wrote %s\n", json_out.c_str());
    }

    return 0;
}
