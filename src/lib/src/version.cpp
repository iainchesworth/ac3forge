#include "ac3/version.hpp"

#include <string>

namespace ac3 {

// Plain concatenation, not std::format: <format> is unavailable on some
// libc++ this project builds against (NDK r26's bundled libc++ does not
// implement it - see docs/platforms/android.md), and this string is simple
// enough that a formatting library buys nothing a handful of += calls
// don't already give just as clearly.
std::string version_details() {
    std::string out;
    out += "ac3forge ";
    out += version_full;
    out += "\n  release: ";
    out += git_describe;
    out += "\n  commit:  ";
    out += git_commit_full;
    out += "\n  branch:  ";
    out += git_branch;
    out += "\n  target:  ";
    out += build_target;
    if (git_dirty) {
        out += "\n  state:   dirty (uncommitted changes)";
    }
    return out;
}

}  // namespace ac3
