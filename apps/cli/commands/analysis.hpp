#pragma once

#include <optional>
#include <string>
#include <string_view>

// The measurement/transform commands that need no audio hardware: qc, levels, loudness, and
// spdif. spdif is not itself a measurement command, but by this point in the H4 split it sits
// textually adjacent to loudness/levels/qc (the audio-hardware and container commands that used
// to separate them have already moved to their own files) - grouped here by that physical
// adjacency rather than forced into a cleaner-sounding but artificial category, the same rationale
// commands/containers.hpp gives for excluding spdif from itself.
// Split out of main.cpp as part of the repo-structure review's H4 monolith split.
namespace ac3cli::commands {

int run_qc(std::string_view in_path, const std::optional<std::string>& preset_arg);
int run_levels(std::string_view in_path);
int run_loudness(std::string_view in_path);
int run_spdif(std::string_view in_path, std::string_view out_path);

}  // namespace ac3cli::commands
