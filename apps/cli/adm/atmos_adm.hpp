#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/oba/motion.hpp"
#include "ac3/audio/audio_backend.hpp"

// Roadmap B1 phase 3 of 3 ("ADM BWF reader feeding the JOC encoder", see ROADMAP.md) - the narrow
// seam between main.cpp's 'atmos-adm' command and ac3adm::ac3adm/ac3::admbridge, this project's
// one opt-in, non-default library (AC3FORGE_BUILD_ADM, default OFF - see root CMakeLists.txt's
// own option() for why: libadm's Boost dependency).
//
// main.cpp cannot #include "ac3adm/ac3adm.hpp" or "ac3/admbridge/bridge.hpp" itself, not even
// behind a preprocessor guard: this project's tools/checks/check_platform_macros.ps1 (CI-enforced, see
// .github/workflows/ci.yml's own "Check for preprocessor conditionals in src/" job) refuses ANY
// #if/#ifdef/#ifndef under src/ - deliberately stricter than "no OS macros"; that script's own
// header comment says a feature-flag #ifdef is "just as unwelcome as a platform one". So whether
// ac3adm::ac3adm/ac3::admbridge exist in this particular build has to be a build-time FILE choice,
// the same "exactly one implementation, selected by CMake" shape apps/cli/platform/{windows,posix}/
// stdio_binary.cpp and src/audio's own src/backend/<os>/ directory already use for an OS
// difference - here for a library-linked-or-not difference instead. apps/cli/CMakeLists.txt adds
// exactly one of adm/enabled/atmos_adm.cpp or adm/disabled/atmos_adm.cpp to the ac3cli target;
// main.cpp calls the two functions below completely unconditionally either way.
//
// The functions below are declared entirely in terms of ac3::oba's own types (always available -
// ac3::oba is part of ac3::forge, unconditionally built) and plain strings, never
// ac3adm::AdmDocument/AdmError or ac3::admbridge::BridgeResult/BridgeError - so this header itself
// never needs those two modules' own headers, and main.cpp (which includes this one) never gains a
// hard dependency on them either. adm/enabled/atmos_adm.cpp is the one place both meet.
namespace ac3cli {

// Whether THIS build's ac3cli can run 'atmos-adm' at all - the same "is this available here?"
// question main.cpp's existing Needs::kCapture/kPassthrough/kMonitor already ask of
// ac3::audio::audio_backend() (see that header's own top comment on why a per-platform TU,
// not a conditional, answers it); Needs::kAdm (main.cpp's kCommands table) asks this instead,
// and unmet() refuses the command before run_atmos_adm is ever called when it reports
// unavailable - reusing ac3::audio::Capability's {available, reason} shape rather than
// inventing a second one for what is structurally the identical question.
[[nodiscard]] const ac3::audio::Capability& adm_capability();

// Everything run_atmos_adm (main.cpp) needs from one parsed-and-bridged ADM BWF master, expressed
// purely in ac3::oba terms. `handle` owns whatever `pcm`'s spans actually borrow from (an
// ac3adm::AdmDocument, in the real implementation) - keep an AdmAtmosSource alive for exactly as
// long as its `pcm` spans are read, the same lifetime contract ac3::admbridge::BridgeResult
// itself documents for its own `pcm` field.
struct AdmAtmosSource {
    std::uint32_t sample_rate = 0;
    std::vector<bool> is_bed;                 // parallel to paths/pcm; true = bed speaker feed
    std::vector<ac3::oba::ObjectPath> paths;  // pass directly to ac3::oba::evaluate_placements
    std::vector<std::span<const float>> pcm;  // one mono span per channel; see `handle` above
    std::shared_ptr<void> handle;             // opaque - owns the parsed document, if any

    [[nodiscard]] std::size_t channel_count() const { return paths.size(); }
};

// Parses `path` (ac3adm::parse_bw64) and bridges it onto AtmosEncoder's input shape
// (ac3::admbridge::build), or a single diagnostic string already run through both AdmError's and
// BridgeError's own describe() - so main.cpp never needs either error enum's type, only text to
// print. Empty `programme_id` means "the file's own default (lowest-ID) audioProgramme", the same
// default ac3::admbridge::build itself documents.
[[nodiscard]] std::expected<AdmAtmosSource, std::string> load_adm_atmos_source(
    std::string_view path, std::string_view programme_id);

}  // namespace ac3cli
