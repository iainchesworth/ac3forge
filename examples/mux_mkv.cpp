// Wrap an elementary stream in Matroska.
//
// matroska::matroska links nothing from ac3::forge — it takes frames as opaque
// bytes. Pairing it with ac3::io::scan is what keeps the track header honest:
// the channel count and packet boundaries come off the bitstream rather than
// from the caller.

#include <cstddef>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/io/elementary.hpp"
#include "matroska/matroska.hpp"

int main() {
    // Some AC-3 to wrap.
    ac3::FrameEncoder encoder{{.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0}};
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
        const auto encoded = encoder.encode_frame(views);
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

    // One Matroska frame per access unit. For E-AC-3 an access unit is the
    // independent substream plus its dependents, which is exactly what scan
    // groups — a player must receive them together.
    std::vector<std::vector<std::byte>> frames;
    frames.reserve(scanned->access_units.size());
    for (const auto unit : scanned->access_units) {
        frames.emplace_back(unit.begin(), unit.end());
    }

    const matroska::AudioTrack track{
        .codec_id = std::string{scanned->kind == ac3::io::StreamKind::kAc3
                                    ? matroska::kCodecAc3
                                    : matroska::kCodecEac3},
        .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
        .channels = scanned->channels,
        .samples_per_frame = ac3::kSamplesPerFrame,
    };

    const auto file = matroska::mux(track, frames);
    if (!file) {
        std::printf("mux failed: %.*s\n",
                    static_cast<int>(matroska::describe(file.error()).size()),
                    matroska::describe(file.error()).data());
        return 1;
    }

    std::printf("%zu bytes of Matroska from %zu frames\n", file->size(), frames.size());
    return 0;
}
