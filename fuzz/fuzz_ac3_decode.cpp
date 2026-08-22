#include <cstddef>
#include <cstdint>
#include <span>

#include "ac3/decoder/decoder.hpp"

// Mirrors ac3cli's own 'decode' path (apps/cli/main.cpp: run_decode): split the
// raw stream into syncframes, then decode each one with a single FrameDecoder
// so overlap-add state carries across frames exactly as it does for a real
// caller. A malformed differential exponent chain walking the reconstruction
// outside 0..24 (the bug fixed in 8386c8f) is exactly the class of input this
// is meant to keep catching.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(data), size};
    const auto frames = ac3::split_frames(bytes);
    if (!frames) {
        return 0;
    }
    ac3::FrameDecoder decoder;
    for (const auto& frame : *frames) {
        (void)decoder.decode_frame(frame);
    }
    return 0;
}
