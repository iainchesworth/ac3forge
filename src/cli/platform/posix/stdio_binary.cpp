#include "../stdio_binary.hpp"

// POSIX makes no text/binary distinction for file descriptors or streams -
// stdin/stdout already pass bytes through unmodified, so there is nothing to
// do here. This file exists (rather than folding a no-op into main.cpp)
// purely so the call site never has to know which platform it is running on
// - see src/audio/src/platform/posix/ for the same shape applied to a bigger
// surface.

namespace ac3::cli::platform {

void set_stdio_binary() {}

}  // namespace ac3::cli::platform
