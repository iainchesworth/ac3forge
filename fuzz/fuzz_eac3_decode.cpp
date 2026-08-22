#include <cstddef>
#include <cstdint>
#include <span>

#include "ac3/decoder/decoder.hpp"

// Mirrors ac3cli's 'decode' path for E-AC-3 (apps/cli/main.cpp:
// run_decode_eac3): split the raw stream into access units, then render each
// one with a single Eac3Decoder. decode_access_unit calls split_frames and
// decode_substream internally, so this one harness exercises the whole Annex
// E entry surface split_access_units -> decode_substream -> decode_access_unit
// exactly as a real caller drives it.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(data), size};
    const auto units = ac3::split_access_units(bytes);
    if (!units) {
        return 0;
    }
    ac3::Eac3Decoder decoder;
    for (const auto& unit : *units) {
        (void)decoder.decode_access_unit(unit);
    }
    return 0;
}
