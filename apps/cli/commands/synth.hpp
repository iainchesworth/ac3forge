#pragma once

#include <cstdint>
#include <string_view>

#include "../support.hpp"

// The synthetic-signal generator commands: silence, tone sweeps (sine/eac3-sine), a channel-
// placement demo (orbit), and E-AC-3 silence. No file input - each one builds a stream from
// scratch, which is why they share layout_tones/fill_tones/frame_count (private to synth.cpp)
// rather than any of support.hpp's file-reading helpers. Split out of main.cpp as part of the
// repo-structure review's H4 monolith split. Not a physically contiguous block in main.cpp by
// the time this was extracted (encode/atmos/decode/analysis commands sit between silence+sine
// and orbit/eac3-silence) - grouped by what they actually are instead, the same kind of call
// (deliberately overriding pure file-adjacency) commands/live_audio.hpp's split from
// commands/audio_io.hpp already made for a different reason.
namespace ac3cli::commands {

int run_silence(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate);

int run_sine(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
            std::uint32_t freq_hz, std::uint32_t amplitude_pct, std::string_view layout,
            bool couple_flag, const ac3cli::Options& meta);

// The same tone generator as run_sine, but through the E-AC-3 container.
int run_eac3_sine(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
                  std::uint32_t freq_hz, std::uint32_t amplitude_pct, std::string_view layout,
                  const ac3cli::Options& meta);

int run_orbit(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
             std::uint32_t orbit_seconds, const ac3cli::Options& meta);

int run_eac3_silence(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
                     std::string_view layout, const ac3cli::Options& meta);

}  // namespace ac3cli::commands
