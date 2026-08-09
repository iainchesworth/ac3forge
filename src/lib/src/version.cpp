#include "ac3/version.hpp"

#include <format>
#include <string>

namespace ac3 {

namespace {

// OS/architecture/compiler this binary was built for. These are fixed by the
// toolchain at compile time, so plain predefined macros are simpler than
// threading CMAKE_SYSTEM_NAME/CMAKE_CXX_COMPILER_ID through the git-restamp
// step that re-runs GenerateVersion.cmake on every build.
std::string build_target() {
#if defined(_WIN32)
    constexpr std::string_view os = "Windows";
#elif defined(__APPLE__)
    constexpr std::string_view os = "macOS";
#elif defined(__linux__)
    constexpr std::string_view os = "Linux";
#else
    constexpr std::string_view os = "unknown-os";
#endif

#if defined(_M_X64) || defined(__x86_64__)
    constexpr std::string_view arch = "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    constexpr std::string_view arch = "arm64";
#elif defined(_M_IX86) || defined(__i386__)
    constexpr std::string_view arch = "x86";
#else
    constexpr std::string_view arch = "unknown-arch";
#endif

#if defined(__clang__)
    return std::format("{} {} (Clang {}.{}.{})", os, arch, __clang_major__, __clang_minor__,
                        __clang_patchlevel__);
#elif defined(_MSC_VER)
    return std::format("{} {} (MSVC {})", os, arch, _MSC_VER);
#elif defined(__GNUC__)
    return std::format("{} {} (GCC {}.{}.{})", os, arch, __GNUC__, __GNUC_MINOR__,
                        __GNUC_PATCHLEVEL__);
#else
    return std::format("{} {} (unknown compiler)", os, arch);
#endif
}

}  // namespace

std::string version_details() {
    return std::format(
        "ac3forge {}\n"
        "  release: {}\n"
        "  commit:  {}\n"
        "  branch:  {}\n"
        "  target:  {}{}",
        version_string,
        git_describe,
        git_commit_full,
        git_branch,
        build_target(),
        git_dirty ? "\n  state:   dirty (uncommitted changes)" : "");
}

}  // namespace ac3
