#pragma once

#include <string_view>

// What this build can do with the machine's audio hardware.
//
// Capture and passthrough are the only parts of ac3forge that are not pure
// file I/O, and they are the only parts a platform can fail to provide. That
// makes "is this available here?" a question with a per-platform answer, and
// under the no-#ifdef rule a per-platform answer is a file in a platform
// directory - not a conditional at the call site.
//
// Exactly one src/platform/<os>/audio_backend.cpp is compiled, chosen by
// CMake alongside that platform's capture.cpp and passthrough.cpp, so this
// report can never disagree with the implementations it describes. Callers
// read it as data: the CLI marks its usage listing from it and refuses the
// four live-audio commands before running them, without naming an OS.

namespace ac3::platform {

struct Capability {
    bool available = false;
    // Empty when available. Otherwise one line saying what is missing and
    // why, printed verbatim by whoever had to turn a caller away - so the
    // wording lives beside the implementation it is making excuses for.
    std::string_view reason;
};

struct AudioBackend {
    // Enumerating capture endpoints and reading samples from one:
    // ac3::capture, behind the CLI's 'devices' and 'record'.
    Capability capture;
    // Enumerating render endpoints and bitstreaming to one in exclusive
    // mode: ac3::sinks, behind the CLI's 'outputs' and 'play'.
    //
    // Kept separate from capture rather than folded into one "audio" flag
    // because the two are separately hard: capture is an ordinary PCM stream,
    // while passthrough needs exclusive/hog-mode access to a device that will
    // accept a non-PCM format. A platform gaining one and not the other is
    // the expected order, not an edge case.
    Capability passthrough;
};

[[nodiscard]] const AudioBackend& audio_backend();

}  // namespace ac3::platform
