// Fragment an elementary stream into CMAF and signal it for HLS/DASH.
//
// mp4::fragment() shares its AudioTrack/frame shape with mp4::mux() (see
// examples/mux_mp4.cpp) - the only new step is FragmentOptions and reading
// back FragmentedOutput's init_segment/media_segments. mp4/hls.hpp and
// mp4/dash.hpp then build the manifests that point at those same segments,
// codec-blind the same way mp4::mp4 itself is: nothing here is AC-3/E-AC-3
// specific except HlsOptions::channels_attribute, which - like
// AudioTrack::codec_config - is opaque to mp4:: and supplied by this caller.

#include <cstddef>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/io/dec3.hpp"
#include "ac3/io/elementary.hpp"
#include "mp4/dash.hpp"
#include "mp4/hls.hpp"
#include "mp4/mp4.hpp"

int main() {
    // Some AC-3 to fragment - enough frames to span several fragments below
    // (31 frames at 8/fragment is 4 fragments: 8, 8, 8, 7).
    auto encoder = std::make_unique<ac3::FrameEncoder>(
        ac3::EncoderConfig{.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0});
    std::vector<std::vector<float>> pcm(2, std::vector<float>(ac3::kSamplesPerFrame));
    const std::vector<std::span<const float>> views{pcm[0], pcm[1]};

    std::vector<std::byte> elementary;
    for (int frame = 0; frame < 31; ++frame) {
        for (std::size_t ch = 0; ch < pcm.size(); ++ch) {
            for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
                pcm[ch][static_cast<std::size_t>(n)] =
                    0.2F * static_cast<float>((n % 61) - 30) / 30.0F;
            }
        }
        const auto encoded = encoder->encode_frame(views);
        if (!encoded) {
            return 1;
        }
        elementary.insert(elementary.end(), encoded->begin(), encoded->end());
    }

    const auto scanned = ac3::io::scan(elementary);
    if (!scanned) {
        std::printf("scan failed\n");
        return 1;
    }

    std::vector<std::vector<std::byte>> frames;
    frames.reserve(scanned->access_units.size());
    for (const auto unit : scanned->access_units) {
        frames.emplace_back(unit.begin(), unit.end());
    }

    const mp4::AudioTrack track{
        .codec_id = std::string{scanned->kind == ac3::io::StreamKind::kAc3 ? mp4::kCodecAc3
                                                                           : mp4::kCodecEac3},
        .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
        .channels = scanned->channels,
        .samples_per_frame = ac3::kSamplesPerFrame,
        .codec_config = ac3::io::build_codec_config_box(*scanned),
    };

    const auto fragmented =
        mp4::fragment(track, frames, mp4::FragmentOptions{.frames_per_fragment = 8});
    if (!fragmented) {
        std::printf("fragment failed: %.*s\n",
                    static_cast<int>(mp4::describe(fragmented.error()).size()),
                    mp4::describe(fragmented.error()).data());
        return 1;
    }

    // Dolby Digital Plus with Atmos objects would set channels_attribute to
    // "<N>/JOC" here instead (see mp4/hls.hpp's own citations) - plain AC-3
    // has no such extension, so the default (just the channel count) is
    // correct as-is.
    const auto media_playlist =
        mp4::build_hls_media_playlist(track, fragmented->media_segments, mp4::HlsOptions{});
    const auto master_playlist = mp4::build_hls_master_playlist(track, fragmented->media_segments,
                                                                "audio.m3u8", mp4::HlsOptions{});
    const auto dash_snippet = mp4::build_dash_adaptation_set(track, fragmented->media_segments);

    std::printf("%zu bytes init segment, %zu media segment(s) from %zu frames\n",
                fragmented->init_segment.size(), fragmented->media_segments.size(), frames.size());
    std::printf("HLS media playlist:\n%s\n", media_playlist.c_str());
    std::printf("HLS master playlist:\n%s\n", master_playlist.c_str());
    std::printf("DASH AdaptationSet:\n%s\n", dash_snippet.c_str());
    return 0;
}
