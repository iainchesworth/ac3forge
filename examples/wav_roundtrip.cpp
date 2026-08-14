// Write a real WAV file, encode it, decode it back, and write the result.
//
// Every other example stays in memory: PCM is synthesized straight into the
// encoder and the decoded samples are only ever counted. A real pipeline
// reads a file a user handed it and writes one back, which means crossing
// WAV's own channel order (WAVE_FORMAT_EXTENSIBLE: FL, FR, FC, LFE, BL, BR)
// against A/52 Table 5.8's (L, C, R, SL, SR, LFE) twice - once on the way in,
// once on the way out.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <numbers>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/io/wav.hpp"

namespace {

constexpr ac3::Acmod kAcmod = ac3::Acmod::k3_2;
constexpr bool kLfe = true;
constexpr int kFrames = 62;  // two seconds
constexpr std::array<double, 6> kTones{1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0};

bool fail(const char* what, std::string_view detail) {
    std::printf("%s: %.*s\n", what, static_cast<int>(detail.size()), detail.data());
    return false;
}

}  // namespace

int main() {
    const auto source_path = (std::filesystem::temp_directory_path() / "ac3forge_source.wav").string();
    const auto result_path = (std::filesystem::temp_directory_path() / "ac3forge_result.wav").string();

    // Synthesize 5.1 in AC-3 order (L, C, R, SL, SR, LFE) and write it out in
    // WAV order - wav_channel_order says where each AC-3 channel belongs in
    // the interleave.
    std::vector<std::vector<float>> ac3_order(6, std::vector<float>(
                                                       static_cast<std::size_t>(kFrames) * ac3::kSamplesPerFrame));
    for (std::size_t ch = 0; ch < ac3_order.size(); ++ch) {
        for (std::size_t n = 0; n < ac3_order[ch].size(); ++n) {
            const double t = static_cast<double>(n) / 48000.0;
            ac3_order[ch][n] =
                static_cast<float>(0.4 * std::sin(2.0 * std::numbers::pi * kTones[ch] * t));
        }
    }
    const auto write_order = ac3::io::wav_channel_order(kAcmod, kLfe);
    if (const auto wrote = ac3::io::write_wav_f32(source_path, ac3_order, 48000, write_order); !wrote) {
        return fail("write_wav_f32 failed", ac3::io::describe(wrote.error()));
    }
    std::printf("wrote %s\n", source_path.c_str());

    // Read it back - read_wav hands the samples back in WAV order, so
    // ac3_layout_for's wav_index permutes them onto AC-3 channel k.
    const auto read = ac3::io::read_wav(source_path);
    if (!read) {
        return fail("read_wav failed", ac3::io::describe(read.error()));
    }
    const auto layout = ac3::io::ac3_layout_for(read->channels.size());
    if (!layout || layout->acmod != kAcmod || layout->lfe != kLfe) {
        std::printf("unexpected WAV channel count: %zu\n", read->channels.size());
        return 1;
    }
    std::vector<std::vector<float>> from_wav(layout->wav_index.size());
    for (std::size_t k = 0; k < layout->wav_index.size(); ++k) {
        from_wav[k] = read->channels[layout->wav_index[k]];
    }

    // Encode, then decode, exactly kSamplesPerFrame at a time.
    // Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history
    // state (PREfast's C6262).
    auto encoder = std::make_unique<ac3::FrameEncoder>(
        ac3::EncoderConfig{.bitrate_kbps = 448, .acmod = kAcmod, .lfe = kLfe});
    ac3::FrameDecoder decoder;
    std::vector<std::vector<float>> decoded_ac3_order(6);
    for (int frame = 0; frame < kFrames; ++frame) {
        std::vector<std::span<const float>> views;
        for (const auto& channel : from_wav) {
            views.push_back(std::span<const float>{channel}.subspan(
                static_cast<std::size_t>(frame) * ac3::kSamplesPerFrame, ac3::kSamplesPerFrame));
        }
        const auto encoded = encoder->encode_frame(views);
        if (!encoded) {
            std::printf("encode failed: %d\n", std::to_underlying(encoded.error()));
            return 1;
        }
        const auto decoded = decoder.decode_frame(*encoded);
        if (!decoded) {
            return fail("decode failed", ac3::describe(decoded.error()));
        }
        for (std::size_t ch = 0; ch < decoded->channels.size(); ++ch) {
            auto& out = decoded_ac3_order[ch];
            out.insert(out.end(), decoded->channels[ch].begin(), decoded->channels[ch].end());
        }
    }

    // Write the round trip back out, permuted into WAV order the same way the
    // source was.
    if (const auto wrote = ac3::io::write_wav_f32(result_path, decoded_ac3_order, 48000, write_order);
        !wrote) {
        return fail("write_wav_f32 failed", ac3::io::describe(wrote.error()));
    }
    std::printf("wrote %s (%zu frames)\n", result_path.c_str(), decoded_ac3_order.front().size());

    std::filesystem::remove(source_path);
    std::filesystem::remove(result_path);
    return 0;
}
