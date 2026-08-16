// Wrap an elementary stream in an MPEG-2 Transport Stream.
//
// mpegts::mpegts links nothing from ac3::forge beyond the AC-3/E-AC-3 choice
// it is told — it takes access units as opaque bytes. Pairing it with
// ac3::io::scan is what keeps the PMT descriptor honest: which codec, and
// where each access unit begins, come off the bitstream rather than from the
// caller.

#include <cstddef>
#include <cstdio>
#include <memory>
#include <span>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/io/elementary.hpp"
#include "mpegts/mpegts.hpp"

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

    // One PES-wrapped access unit per TS access unit. For E-AC-3 an access
    // unit is the independent substream plus its dependents, which is
    // exactly what scan groups — a player must receive them together.
    std::vector<std::vector<std::byte>> frames;
    frames.reserve(scanned->access_units.size());
    for (const auto unit : scanned->access_units) {
        frames.emplace_back(unit.begin(), unit.end());
    }

    const mpegts::AudioTrack track{
        .codec = scanned->kind == ac3::io::StreamKind::kAc3 ? mpegts::AudioCodec::kAc3
                                                             : mpegts::AudioCodec::kEac3,
        .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
        .channels = scanned->channels,
        .samples_per_frame = ac3::kSamplesPerFrame,
    };

    const auto file = mpegts::mux(track, frames);
    if (!file) {
        std::printf("mux failed: %.*s\n", static_cast<int>(mpegts::describe(file.error()).size()),
                    mpegts::describe(file.error()).data());
        return 1;
    }

    std::printf("%zu bytes of MPEG-TS from %zu frames\n", file->size(), frames.size());
    return 0;
}
