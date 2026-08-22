#pragma once

#include <cstdint>
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

// Atmos over TrueHD: every source channel becomes a dynamic object carried
// as its own discrete lossless channel (ac3::mlp::AtmosEncoder), with
// 16ch_channel_meaning() announcing the roles and per-frame OAMD positions
// riding EMDF in EXTRA_DATA(). Motion comes from an optional keyframe file
// (the same format and object addressing atmos-path/atmos-encode take);
// without one, objects fan out evenly around the room and stay put.
int run_truehd_atmos(std::string_view in_path, std::string_view out_path,
                     std::uint32_t objects, std::string_view paths_path);

}  // namespace ac3cli::commands
