#include <cstddef>
#include <cstdint>
#include <span>

#include "ac3/io/elementary.hpp"

// ac3::io::scan is the first thing that touches a stream nobody has looked at
// yet: format-sniffing for AC-3 vs E-AC-3 vs garbage, called before any
// decoder commits to a layout (see src/forge/src/io/elementary.cpp). It must
// never crash or hang on arbitrary bytes, only return an error.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(data), size};
    (void)ac3::io::scan(bytes);
    return 0;
}
