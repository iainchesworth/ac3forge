#pragma once

#include <cstdint>
#include <string_view>

// The container-wrapping commands: mkv/mp4/fmp4/ts each take an already-encoded AC-3 or E-AC-3
// elementary stream and wrap it for a specific downstream consumption method, reading everything
// the container needs to declare (format, access-unit boundaries, sample rate, channel count)
// straight off the bitstream via ac3::io::scan rather than from a caller-supplied layout that
// could disagree with what the file actually contains. Split out of main.cpp as part of the
// repo-structure review's H4 monolith split - see support.hpp's own top comment for the overall
// plan; this is the first commands/ group, the four physically-contiguous container commands
// (spdif is elsewhere in main.cpp and will be grouped with its own file-position neighbors in a
// later step, not here).
namespace ac3cli::commands {

int run_mkv(std::string_view in_path, std::string_view out_path);

// Same shape as run_mkv, wrapping mp4::mux() instead: ac3::io::scan() still supplies everything
// the container needs to declare, and additionally - via ac3::io::build_codec_config_box() - the
// exact dac3/dec3 sample-entry payload straight off the bitstream.
int run_mp4(std::string_view in_path, std::string_view out_path);

// fMP4/CMAF segmenting plus HLS/DASH signaling (ROADMAP.md's A2) - writes a DIRECTORY of files
// (an init segment, one media segment per fragment, an HLS media+master playlist pair, and a
// DASH MPD) rather than one file, so a packager or CDN origin can be pointed at out_dir directly.
int run_fmp4(std::string_view in_path, std::string_view out_dir, std::uint32_t frames_per_fragment);

// Same shape as run_mkv: wraps as an MPEG-2 Transport Stream (DVB profile - ETSI EN 300 468
// Annex D's AC3_descriptor/Enhanced_AC3_descriptor), everything read off the bitstream.
int run_ts(std::string_view in_path, std::string_view out_path);

}  // namespace ac3cli::commands
