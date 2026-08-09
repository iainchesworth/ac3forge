#include "ac3/version.hpp"

#include <format>
#include <string>

namespace ac3 {

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
        build_target,
        git_dirty ? "\n  state:   dirty (uncommitted changes)" : "");
}

}  // namespace ac3
