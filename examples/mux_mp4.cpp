// Wrap an elementary stream in MP4.
//
// mp4::mp4 links nothing from ac3::forge — it takes frames, and the codec's
// sample-entry configuration box, as opaque bytes. Pairing it with
// ac3::io::scan and ac3::io::build_codec_config_box is what keeps the dec3
// box honest: fscod/bsid/bsmod/acmod/lfeon and the Dolby Atmos extension
// (flag_ec3_extension_type_a/complexity_index_type_a) come straight off the
// bitstream rather than from the caller.

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
#include "mp4/mp4.hpp"

int main() {
    // Some AC-3 to wrap.
    // Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history
    // state (PREfast's C6262).
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

    // Ask the bitstream what it is rather than asserting it.
    const auto scanned = ac3::io::scan(elementary);
    if (!scanned) {
        std::printf("scan failed\n");
        return 1;
    }

    // One MP4 sample per access unit. For E-AC-3 an access unit is the
    // independent substream plus its dependents, which is exactly what scan
    // groups — a player must receive them together.
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
        // The dac3/dec3 sample-entry box, built from the same scan result -
        // see ac3/io/dec3.hpp for why this lives in ac3::io rather than in
        // mp4::mp4 itself.
        .codec_config = ac3::io::build_codec_config_box(*scanned),
    };

    const auto file = mp4::mux(track, frames);
    if (!file) {
        std::printf("mux failed: %.*s\n", static_cast<int>(mp4::describe(file.error()).size()),
                    mp4::describe(file.error()).data());
        return 1;
    }

    std::printf("%zu bytes of MP4 from %zu frames\n", file->size(), frames.size());
    return 0;
}
