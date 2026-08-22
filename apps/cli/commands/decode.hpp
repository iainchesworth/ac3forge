#pragma once

#include <string_view>

#include "../support.hpp"

// The decode commands: decode (AC-3/E-AC-3 auto-detected by bsid) and its E-AC-3-specific
// object-layer/DRC-reporting half. Confirmed still physically contiguous (report_decoded_objects,
// print_drc_summary, run_decode_eac3, run_decode) after 8 prior extraction rounds.
// Split out of main.cpp as part of the repo-structure review's H4 monolith split.
namespace ac3cli::commands {

int run_decode(std::string_view in_path, std::string_view out_path, const ac3cli::Options& meta,
               std::string_view objects_dir);

}  // namespace ac3cli::commands
