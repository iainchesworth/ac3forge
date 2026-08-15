#include "../stdio_binary.hpp"

#include <cstdio>
#include <fcntl.h>
#include <io.h>

// _setmode()/_fileno() are the documented MSVC CRT way to change a standard
// stream's translation mode after the fact - see the header's own comment
// for what goes wrong on Windows without this. std::cin/std::cout are built
// on top of the same CRT file descriptors _fileno() names here, so flipping
// fd 0/1 to _O_BINARY changes what the C++ streams see too, not just <cstdio>.

namespace ac3::cli::platform {

void set_stdio_binary() {
    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);
}

}  // namespace ac3::cli::platform
