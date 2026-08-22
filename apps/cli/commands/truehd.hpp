#pragma once

#include <string_view>

// The TrueHD (MLP) lossless commands. Unlike every other encode/decode pair
// here, these work in integer sample words end to end - the float-normalized
// WAV path the lossy codecs use would forfeit the bit-exactness the decoder
// verifies. The .mlp output is the raw access-unit sequence of the Dolby
// bitstream description's §5, with no container. Grouped in their own file
// from the start (the codec family shares nothing with the AC-3 command
// groups), following the H4 monolith-split layout.
namespace ac3cli::commands {

int run_truehd_encode(std::string_view in_path, std::string_view out_path);

int run_truehd_decode(std::string_view in_path, std::string_view out_path);

}  // namespace ac3cli::commands
