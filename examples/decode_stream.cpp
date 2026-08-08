// Read an elementary stream back: work out what it is, then decode it.
//
// ac3::io::scan does the first half without committing to a generation — it
// finds the access-unit boundaries and reports what the stream renders, which
// is what a muxer needs and what tells you which decoder to reach for.

#include <cstddef>
#include <cstdio>
#include <span>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/io/elementary.hpp"

namespace {

// A short AC-3 stream to decode, so the example needs no input file.
std::vector<std::byte> make_stream() {
    ac3::FrameEncoder encoder{{.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0}};
    std::vector<std::vector<float>> pcm(2, std::vector<float>(ac3::kSamplesPerFrame));
    for (std::size_t ch = 0; ch < pcm.size(); ++ch) {
        for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
            pcm[ch][static_cast<std::size_t>(n)] = 0.25F * static_cast<float>((n % 97) - 48) / 48.0F;
        }
    }
    const std::vector<std::span<const float>> views{pcm[0], pcm[1]};

    std::vector<std::byte> stream;
    for (int frame = 0; frame < 5; ++frame) {
        const auto encoded = encoder.encode_frame(views);
        if (!encoded) {
            return {};
        }
        stream.insert(stream.end(), encoded->begin(), encoded->end());
    }
    return stream;
}

}  // namespace

int main() {
    const std::vector<std::byte> stream = make_stream();

    // Spans in the result point into `stream`, so it has to outlive them.
    const auto scanned = ac3::io::scan(stream);
    if (!scanned) {
        std::printf("scan failed: %.*s\n",
                    static_cast<int>(ac3::io::describe(scanned.error()).size()),
                    ac3::io::describe(scanned.error()).data());
        return 1;
    }
    std::printf("%s, %u Hz, %d channels, %zu access units\n",
                scanned->kind == ac3::io::StreamKind::kAc3 ? "AC-3" : "E-AC-3",
                ac3::sample_rate_hz(scanned->sample_rate), scanned->channels,
                scanned->access_units.size());

    // AC-3: one syncframe per access unit. For E-AC-3 use ac3::Eac3Decoder and
    // decode_access_unit, which applies the §E3.8.2 render across substreams.
    ac3::FrameDecoder decoder;
    std::size_t samples = 0;
    for (const auto unit : scanned->access_units) {
        const auto decoded = decoder.decode_frame(unit);
        if (!decoded) {
            std::printf("decode failed: %.*s\n",
                        static_cast<int>(ac3::describe(decoded.error()).size()),
                        ac3::describe(decoded.error()).data());
            return 1;
        }
        samples += decoded->channels.front().size();
    }
    std::printf("decoded %zu samples per channel\n", samples);
    return 0;
}
