#pragma once

#include <cstdint>
#include <string_view>

#include "../support.hpp"

// The continuous (streaming/live) audio commands: decode-and-play-back a whole file in real
// time ('monitor'), and capture-encode-monitor-and/or-passthrough continuously while also
// writing the encoded result to a file ('live'). Split out of main.cpp as part of the
// repo-structure review's H4 monolith split - the counterpart to commands/audio_io.hpp's
// one-shot record/play/devices/outputs, deliberately kept separate since run_live alone is
// ~800 lines and both commands share the "runs until done, not until one file is written"
// shape rather than audio_io's "single operation, then exit" one.
namespace ac3cli::commands {

// Decode a file back to PCM and play it on an ordinary (shared-mode, not
// bitstreamed) output - a sanity-check/preview path, and the offline half of
// live monitoring ('live's --monitor equivalent works the same way, one
// access unit at a time as it is produced instead of read from a file).
// Object metadata (JOC/OAMD), when present, is reported (object count) but
// not played or exported here: Eac3Decoder reads TS 103 420's object layer
// (DecodedSubstream::object_metadata/object_audio), but this path only plays
// the 5.1 bed - exactly what a legacy decoder hears, which is the thing most
// worth confirming actually sounds right. 'ac3cli decode' with objects_dir
// is where the reconstructed object audio itself comes out.
int run_monitor(std::string_view in_path, int device_index, const ac3cli::Options& meta);

// Live capture -> live encode -> optionally live monitor and/or live IEC
// 61937 passthrough, running continuously and also writing the encoded
// access units to a file (so a live session leaves an artifact the way
// 'record' always has). This is the command 'record' is not: 'record' only
// ever reaches a file.
//
// mode "atmos" additionally moves each object's placement every frame from
// elapsed wall-clock time, using the same orbiting math run_atmos's
// synthetic demo uses - the concrete shape a real per-frame live position
// source (a separate, parallel piece of work) drops into once it lands: swap
// the orbit-angle expression below for a read of wherever that source keeps
// its current position, still evaluated fresh every frame inside this same loop.
int run_live(std::string_view out_path, int capture_device, std::uint32_t seconds,
            std::uint32_t bitrate, int monitor_device, int passthrough_device,
            std::string_view mode, const ac3cli::Options& meta);

}  // namespace ac3cli::commands
