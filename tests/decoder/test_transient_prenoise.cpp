#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numeric>
#include <vector>

#include "ac3/decoder/transient_prenoise.hpp"

namespace {

// A buffer long enough to hold a worst-case correction with room to spare:
// synth_len can reach 2*TC1 + pnlen, and pnlen itself can be almost a full
// block, so a few thousand samples covers every case these tests build.
constexpr int kBufLen = 4096;

std::vector<float> ramp(int len, float start = 0.0f, float step = 1.0f) {
    std::vector<float> v(static_cast<std::size_t>(len));
    for (int i = 0; i < len; ++i) {
        v[static_cast<std::size_t>(i)] = start + step * static_cast<float>(i);
    }
    return v;
}

}  // namespace

TEST_CASE("apply_transient_prenoise overwrites the middle region exactly with the synthesis copy",
          "[transient_prenoise]") {
    // A buffer of distinct, easily-traced values (index itself), so the
    // straight-overwrite region [TC1, tot_corr_len - TC2) can be checked
    // bin-for-bin against the exact samples the synthesis buffer should
    // have copied forward from - §3.7.2's synth_buf[samp] =
    // pcm_out[transloc - (2*TC1 + 2*pnlen) + samp].
    auto pcm = ramp(kBufLen, 0.0f, 1.0f);
    const int transloc = 3000;
    const int translen = 100;

    const int block_start = (transloc / 256) * 256;
    const int aud_blk_samp_loc = block_start - 256;
    const int pnlen = transloc - aud_blk_samp_loc;
    const int tot_corr_len = pnlen + translen + ac3::kTransientPrenoiseTC1;
    const int start_samp = transloc - tot_corr_len;
    const int synth_start = transloc - 2 * ac3::kTransientPrenoiseTC1 - 2 * pnlen;

    ac3::apply_transient_prenoise(pcm, transloc, translen);

    for (int samp = ac3::kTransientPrenoiseTC1;
        samp < tot_corr_len - ac3::kTransientPrenoiseTC2; ++samp) {
        CAPTURE(samp);
        const float expected = static_cast<float>(synth_start + samp);
        CHECK(pcm[static_cast<std::size_t>(start_samp + samp)] == expected);
    }
}

TEST_CASE("apply_transient_prenoise's first sample is a pure crossfade-out of the original",
          "[transient_prenoise]") {
    // At samp = 0 in region 1, win_fade_out1[0] should be ~1 (unity) and
    // win_fade_in1[0] should be ~0 - the correction starts by barely
    // touching the original decoded sample, not replacing it outright.
    auto pcm = ramp(kBufLen, 1000.0f, 0.0f);  // constant 1000 everywhere
    const int transloc = 3000;
    const int translen = 100;
    const int block_start = (transloc / 256) * 256;
    const int aud_blk_samp_loc = block_start - 256;
    const int pnlen = transloc - aud_blk_samp_loc;
    const int tot_corr_len = pnlen + translen + ac3::kTransientPrenoiseTC1;
    const int start_samp = transloc - tot_corr_len;

    // Make the synthesis-source region distinctly different (500) so the
    // blend at samp=0 is visibly close to the ORIGINAL (1000), not synth.
    const int synth_start = transloc - 2 * ac3::kTransientPrenoiseTC1 - 2 * pnlen;
    for (int i = 0; i < 2 * ac3::kTransientPrenoiseTC1 + pnlen; ++i) {
        pcm[static_cast<std::size_t>(synth_start + i)] = 500.0f;
    }
    // Restore the original value at exactly start_samp (it was inside the
    // synth-source range above, and region 1 needs to read the TRUE
    // original there for this check to mean anything) - use a value the
    // synth copy step wouldn't have touched: outside [synth_start,
    // synth_start + synth_len).
    pcm[static_cast<std::size_t>(start_samp)] = 1000.0f;

    ac3::apply_transient_prenoise(pcm, transloc, translen);
    // Should have moved only a little way from 1000 toward 500.
    CHECK(pcm[static_cast<std::size_t>(start_samp)] > 900.0f);
}

TEST_CASE("apply_transient_prenoise's last sample is a pure crossfade-in of the original",
          "[transient_prenoise]") {
    // At the final sample (tot_corr_len - 1), win_fade_in2 should be ~1 and
    // win_fade_out2 ~0 - the correction ends back at the original decoded
    // audio, not the synthesized copy.
    auto pcm = ramp(kBufLen, 1000.0f, 0.0f);
    const int transloc = 3000;
    const int translen = 100;
    const int block_start = (transloc / 256) * 256;
    const int aud_blk_samp_loc = block_start - 256;
    const int pnlen = transloc - aud_blk_samp_loc;
    const int tot_corr_len = pnlen + translen + ac3::kTransientPrenoiseTC1;
    const int start_samp = transloc - tot_corr_len;
    const int last = start_samp + tot_corr_len - 1;
    const float original_last = pcm[static_cast<std::size_t>(last)];

    const int synth_start = transloc - 2 * ac3::kTransientPrenoiseTC1 - 2 * pnlen;
    for (int i = 0; i < 2 * ac3::kTransientPrenoiseTC1 + pnlen; ++i) {
        pcm[static_cast<std::size_t>(synth_start + i)] = 500.0f;
    }
    pcm[static_cast<std::size_t>(last)] = original_last;  // keep it distinguishable

    ac3::apply_transient_prenoise(pcm, transloc, translen);
    CHECK(pcm[static_cast<std::size_t>(last)] > 900.0f);
}

TEST_CASE("apply_transient_prenoise reduces energy where a transient's pre-noise sat",
          "[transient_prenoise]") {
    // The end-to-end claim this whole tool exists for: quantization noise
    // sitting just before a sharp transient gets measurably quieter once
    // corrected, because it is overwritten with a copy of the clean audio
    // further back. A synthetic "pre-noise" is a burst of higher-amplitude
    // noise-like content in the region the correction targets; a real clean
    // signal (near-silence) sits further back for the synthesis copy to
    // pull forward.
    std::vector<float> pcm(static_cast<std::size_t>(kBufLen), 0.0f);
    const int transloc = 3000;
    const int translen = 100;
    const int block_start = (transloc / 256) * 256;
    const int aud_blk_samp_loc = block_start - 256;
    const int pnlen = transloc - aud_blk_samp_loc;
    const int tot_corr_len = pnlen + translen + ac3::kTransientPrenoiseTC1;
    const int start_samp = transloc - tot_corr_len;

    // "Noise" (large alternating values) exactly in the region the
    // straight-overwrite pass will replace.
    for (int samp = ac3::kTransientPrenoiseTC1; samp < tot_corr_len - ac3::kTransientPrenoiseTC2;
        ++samp) {
        pcm[static_cast<std::size_t>(start_samp + samp)] = (samp % 2 == 0) ? 1.0f : -1.0f;
    }
    double noisy_energy = 0.0;
    for (int samp = ac3::kTransientPrenoiseTC1; samp < tot_corr_len - ac3::kTransientPrenoiseTC2;
        ++samp) {
        const double v = static_cast<double>(pcm[static_cast<std::size_t>(start_samp + samp)]);
        noisy_energy += v * v;
    }

    ac3::apply_transient_prenoise(pcm, transloc, translen);

    double corrected_energy = 0.0;
    for (int samp = ac3::kTransientPrenoiseTC1; samp < tot_corr_len - ac3::kTransientPrenoiseTC2;
        ++samp) {
        const double v = static_cast<double>(pcm[static_cast<std::size_t>(start_samp + samp)]);
        corrected_energy += v * v;
    }
    CHECK(corrected_energy < noisy_energy * 0.01);
}
