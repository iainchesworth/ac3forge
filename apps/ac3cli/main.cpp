#include <print>

#include "ac3/core/tables.hpp"

int main() {
    std::println("ac3forge 0.1.0 — clean-room AC-3 (ATSC A/52) encoder, work in progress");
    std::println("Scaffold status: bit I/O, CRC-16, and base syntax tables with tests.");
    std::println("Next milestone: frame skeleton → valid silent 2.0 frame (docs/RESEARCH.md §8).");
    std::println("");
    std::println("Sanity: 448 kbit/s @ 48 kHz frame = {} bytes",
                 ac3::frame_size_bytes(ac3::SampleRate::k48000, 448).value());
    return 0;
}
