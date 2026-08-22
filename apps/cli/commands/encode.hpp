#pragma once

#include <cstdint>
#include <string_view>

#include "../support.hpp"

// The real-material encode commands: encode (AC-3) and eac3-encode (E-AC-3), each with its own
// multi-source (src=/map=) sibling. Confirmed still physically contiguous (all four, plus the
// E-AC-3-specific tools_or_error/vbr_or_error option helpers) after 7 prior extraction rounds.
// Split out of main.cpp as part of the repo-structure review's H4 monolith split.
namespace ac3cli::commands {

// Real program material through the E-AC-3 path, for a possibly multi-source
// run (src=/map= given). dialnorm=auto/dialnorm2=auto measure the RENDERED
// bed/programme content (post map=/routing, in coded-channel order) rather
// than raw per-source channels, since which channel is "L"/"Ls" - or, for
// 1+1, which is Ch1/Ch2 - depends on the assignment rather than file order.
int run_eac3_encode_multi(std::string_view in_path, std::string_view out_path,
                          std::uint32_t bitrate, std::string_view tools,
                          std::string_view layout, std::string_view vbr,
                          const ac3cli::Options& meta);

int run_eac3_encode(std::string_view in_path, std::string_view out_path,
                    std::uint32_t bitrate, std::string_view tools, std::string_view layout,
                    std::string_view vbr, const ac3cli::Options& meta,
                    std::string_view in2_path = {});

// The same encode as run_encode below, but for a possibly multi-source run
// (src=/map= given).
int run_encode_multi(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
                     bool couple, std::string_view layout, const ac3cli::Options& meta);

int run_encode(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
               bool couple, std::string_view layout, const ac3cli::Options& meta,
               std::string_view in2_path = {});

}  // namespace ac3cli::commands
