#pragma once

#include <cstdint>
#include <string_view>

#include "../support.hpp"

// The Atmos/object-layer commands: two synthetic generators (a built-in orbit, and one driven by
// a hand-authored keyframe file), one real-material encoder (every source channel becomes an
// object), and one ADM BWF reader (roadmap B1 phase 3, the ac3adm/admbridge integration). Split
// out of main.cpp as part of the repo-structure review's H4 monolith split.
namespace ac3cli::commands {

// Objects moving in three dimensions, out as one 5.1 E-AC-3 stream carrying
// JOC and OAMD. Each object orbits at its own rate and sits at its own height,
// so no two of them share a direction for long - which is the condition under
// which JOC can actually pull them apart again. Heights are what makes this
// worth doing at all: a 5.1 bed cannot carry them, and the object metadata can.
int run_atmos(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
             std::uint32_t objects, std::uint32_t orbit_seconds, std::string_view mode,
             const ac3cli::Options& meta);

// Objects driven by a hand-authored keyframe file rather than the built-in
// orbit above - the CLI-side proof that ac3::oba's path primitive works end
// to end from genuinely authored motion, not just a closed-form generator.
// An object index the file never mentions holds still at room centre, the
// same fallback the GUI uses for an object with no authored path.
int run_atmos_path(std::string_view out_path, std::string_view paths_path, std::uint32_t seconds,
                   std::uint32_t bitrate, std::uint32_t objects_arg, const ac3cli::Options& meta);

// Every channel of a real file as its own object, over a 5.1 bed with JOC and
// OAMD beside it. The synthetic 'atmos' above shows what the object layer can
// express; this is the one that answers what it does to material somebody
// actually recorded - and it is what the GUI's object mode runs, so the two
// front ends can be compared on the same file.
int run_atmos_encode(std::string_view in_path, std::string_view out_path,
                     std::uint32_t bitrate, std::uint32_t objects,
                     const ac3cli::Options& meta, std::string_view paths_path = {});

// Roadmap B1 phase 3 of 3 - a real ADM BWF master (BS.2076-2 ADM XML embedded in a BS.2088-1
// BW64/RF64 container) straight to DD+ JOC E-AC-3, no WAV plus a hand-authored keyframe file the
// way atmos-encode above needs, because the master already carries every bed speaker feed's and
// dynamic object's own position/gain automation (§10.3). See adm/atmos_adm.hpp's own header
// comment for why this function is unconditional (ac3adm::ac3adm/ac3::admbridge
// linked-or-not is a build-time FILE choice, never a preprocessor conditional).
int run_atmos_adm(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
                  const ac3cli::Options& meta, std::string_view programme_id);

}  // namespace ac3cli::commands
