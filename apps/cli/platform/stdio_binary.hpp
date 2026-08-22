#pragma once

// Puts stdin and stdout into pure binary mode, so ac3cli's "-" convention (a
// lone '-' in place of a file argument means stdin for an input path, stdout
// for an output path - see main.cpp's is_stdio_path()) can move raw audio
// bytes through them unmodified.
//
// On Windows, the CRT opens stdin/stdout in TEXT mode by default: output
// silently translates every 0x0A byte to the pair 0x0D 0x0A, and input
// treats a stray 0x1A byte as an in-band end-of-file - either one corrupts
// real (non-text) audio data the moment it crosses the pipe. POSIX systems
// draw no distinction between text and binary streams, so there is nothing
// to set there.
//
// This is the one piece of platform-specific code ac3cli needs - see this
// directory's CMakeLists.txt entry for the WIN32/else split that selects
// between platform/windows/ and platform/posix/, the same shape ac3::audio's
// own platform tree uses (src/audio/CMakeLists.txt) scaled down to one
// function, so main.cpp itself never has to ask which OS it is running on.

namespace ac3::cli::platform {

void set_stdio_binary();

}  // namespace ac3::cli::platform
