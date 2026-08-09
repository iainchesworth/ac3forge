#include <cstddef>
#include <cstdint>
#include <string>

#include <unistd.h>

#include "ac3/io/wav.hpp"

// read_wav's only interface is a path (src/lib/include/ac3/io/wav.hpp), not a
// byte span, so the one unavoidable step beyond calling it directly is
// round-tripping libFuzzer's buffer through a scratch file - there is no
// in-memory entry point to call instead. /dev/shm keeps that off real disk
// where the container provides it; every POSIX host has /tmp regardless. A
// truncated or malformed WAV is realistic input (a user's own bad file), not
// only an adversarial one - see the task brief this harness answers.
namespace {

const char* scratch_dir() {
    static const char* const dir = (::access("/dev/shm", W_OK) == 0) ? "/dev/shm" : "/tmp";
    return dir;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::string path = std::string(scratch_dir()) + "/ac3forge-fuzz-wav-XXXXXX";
    const int fd = ::mkstemp(path.data());
    if (fd < 0) {
        return 0;
    }
    std::size_t written = 0;
    while (written < size) {
        const auto n = ::write(fd, data + written, size - written);
        if (n <= 0) {
            break;
        }
        written += static_cast<std::size_t>(n);
    }
    ::close(fd);
    (void)ac3::io::read_wav(path);
    ::unlink(path.c_str());
    return 0;
}
