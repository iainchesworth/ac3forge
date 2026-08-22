#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <exception>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <print>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ac3/analysis/levels.hpp"
#include "ac3/audio/capture.hpp"
#include "ac3/audio/resampler.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/assignment.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/encoder/silent_frame.hpp"
#include "ac3/io/dec3.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/loudness.hpp"
#include "ac3/meta/mixing.hpp"
#include "ac3/meta/qc.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/oba/motion.hpp"
#include "ac3/audio/audio_backend.hpp"
#include "ac3/sinks/iec61937.hpp"
#include "ac3/audio/monitor.hpp"
#include "ac3/audio/passthrough.hpp"
#include "ac3/spatial/spatial.hpp"
#include "ac3/version.hpp"
#include "ac3/signing/emdf_atmos_signer.hpp"
#include "ac3/signing/signing_key.hpp"
#include "matroska/matroska.hpp"
#include "mp4/dash.hpp"
#include "mp4/hls.hpp"
#include "mp4/mp4.hpp"
#include "mpegts/mpegts.hpp"
#include "platform/stdio_binary.hpp"
#include "adm/atmos_adm.hpp"
#include "commands/analysis.hpp"
#include "commands/atmos.hpp"
#include "commands/audio_io.hpp"
#include "commands/containers.hpp"
#include "commands/decode.hpp"
#include "commands/encode.hpp"
#include "commands/live_audio.hpp"
#include "commands/synth.hpp"
#include "multi_source.hpp"
#include "support.hpp"

namespace {

namespace plan = ac3::plan;

using namespace ac3cli;
using namespace ac3cli::commands;

void print_usage();


// ---------------------------------------------------------------------------
// The command table. Every command is one row: its name, how many positional
// arguments it needs, the argument spec the usage text prints, and the code
// that runs it.
//
// It replaces a chain of `if (command == ...)` comparisons, each of which
// repeated `args.size() > N ? parse(args[N]) : default` for every parameter.
// That repetition is where this file kept going wrong: consolidating six
// parallel branches turned up SIX argv faults of exactly one shape - an entry
// counting from the wrong base, or reading a slot it had not checked. Two
// would have written output to a file named after the duration. None was
// visible in a build or a unit test, because the indices are only wrong
// relative to a convention nothing states in one place.
//
// Here the convention is stated once: args[0] is the command, so args[1] is
// the first parameter, and min_args is checked before any handler runs.
// print_usage() is generated from the same rows, so the help cannot drift
// from what dispatch accepts - it already had, with eac3-silence and
// eac3-sine missing from it entirely.
// ---------------------------------------------------------------------------

struct Args {
    std::span<char* const> a;
    const Options& meta;
    bool couple;

    [[nodiscard]] std::string_view str(std::size_t i, std::string_view fallback = {}) const {
        return i < a.size() ? std::string_view{a[i]} : fallback;
    }
    [[nodiscard]] std::uint32_t u32(std::size_t i, std::uint32_t fallback) const {
        return i < a.size() ? parse_u32_or(a[i], fallback) : fallback;
    }
    // Signed, unlike u32: routing a negative token through parse_u32_or (which
    // parses unsigned) always fails and silently returns 0 rather than
    // `fallback` - the wrong answer for the sentinel values several commands
    // read as "unset" or "default" (e.g. play's device index). from_chars
    // for a signed int accepts the leading '-' directly, so this parses
    // the token itself instead of bouncing through the unsigned path.
    [[nodiscard]] int i32(std::size_t i, int fallback) const {
        if (i >= a.size()) {
            return fallback;
        }
        const std::string_view text{a[i]};
        int value = 0;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        return ec == std::errc{} && ptr == text.data() + text.size() ? value : fallback;
    }
};

// What a command needs beyond plain file I/O to run at all in THIS build. Most commands need
// nothing. Several need the machine's audio hardware; one (atmos-adm) needs a library that is not
// part of every build - either way, unmet() below answers with the same {available, reason} shape
// (ac3::audio::Capability), so dispatch and the usage listing treat both kinds of "not here"
// identically.
//
// This is a column in the table rather than a check inside each handler for
// the same reason min_args is: stated once, beside the command it describes,
// and read by both dispatch and the usage text so the two cannot disagree
// about which commands exist here.
//
// 'live' needs kCapture, not a new combined category: capture is the one
// hard requirement (no capture endpoint, no session at all), while its
// monitor/passthrough legs are soft - unavailable or refused there degrades
// to a warning and a file-only session, exactly like plugging into a
// receiver that says no. Only 'monitor' (which does nothing BUT play back)
// needs kMonitor as a hard gate, the same way 'play'/'outputs' need
// kPassthrough.
//
// kAdm ('atmos-adm', roadmap B1 phase 3): unlike the three audio ones, this is not a hardware
// question - it is whether ac3adm::ac3adm/ac3::admbridge were linked into this build at all
// (AC3FORGE_BUILD_ADM, default OFF - see the root CMakeLists.txt's own option()). Answered the
// same way regardless: adm/atmos_adm.hpp's ac3cli::adm_capability(), backed by exactly one of
// adm/enabled/atmos_adm.cpp or adm/disabled/atmos_adm.cpp (see run_atmos_adm's own comment for
// why a CMake-selected file, not a preprocessor conditional, decides this).
enum class Needs : std::uint8_t { kNothing, kCapture, kPassthrough, kMonitor, kAdm };

// The unmet requirement, or nullptr when this build/platform can satisfy it.
//
// Note what kCapture/kPassthrough/kMonitor are not: an OS test. main.cpp never asks whether it is
// on Windows - it asks the one translation unit CMake compiled from
// src/audio/src/backend/<os>/ what that backend can do, and prints the answer
// that unit supplied. The day a Unix capture backend lands, capture flips to
// available in that file alone and 'devices' and 'record' start working here
// with no change to this file. kAdm asks the analogous question of
// adm/{enabled,disabled}/atmos_adm.cpp instead - a library-linked-or-not fact rather than an
// OS one, answered by the identical "ask the compiled-in file" shape.
const ac3::audio::Capability* unmet(Needs needs) {
    const auto& backend = ac3::audio::audio_backend();
    switch (needs) {
        case Needs::kNothing: return nullptr;
        case Needs::kCapture: return backend.capture.available ? nullptr : &backend.capture;
        case Needs::kPassthrough:
            return backend.passthrough.available ? nullptr : &backend.passthrough;
        case Needs::kMonitor: return backend.monitor.available ? nullptr : &backend.monitor;
        case Needs::kAdm: {
            const auto& adm = ac3cli::adm_capability();
            return adm.available ? nullptr : &adm;
        }
    }
    return nullptr;
}

struct Command {
    std::string_view name;
    std::size_t min_args;  // positional count INCLUDING the command itself
    std::string_view spec;
    std::string_view note;
    Needs needs;
    int (*run)(const Args&);
};

// 26 commands, always - including atmos-adm, whether or not AC3FORGE_BUILD_ADM linked
// ac3adm::ac3adm/ac3::admbridge into this particular build (see Needs::kAdm/unmet() above and
// run_atmos_adm's own comment): a command this build cannot run is listed with Needs gating it,
// never sized out of the table entirely - the identical "listed, not hidden" treatment
// kCapture/kPassthrough/kMonitor commands already get (see print_usage()'s own comment below on
// why hiding would be a lie about a command that exists and would work elsewhere).
constexpr std::array<Command, 26> kCommands{{
    {"silence", 2, "<out.ac3> [seconds] [bitrate_kbps]", "", Needs::kNothing,
     [](const Args& x) { return run_silence(x.str(1), x.u32(2, 5), x.u32(3, 192)); }},
    {"sine", 2, "<out.ac3> [seconds] [bitrate_kbps] [freq_hz] [amp_pct] [layout]", "",
     Needs::kNothing,
     [](const Args& x) {
         return run_sine(x.str(1), x.u32(2, 5), x.u32(3, 192), x.u32(4, 1000), x.u32(5, 50),
                         x.str(6, "stereo"), x.couple, x.meta);
     }},
    {"orbit", 2, "<out.ac3> [seconds] [bitrate_kbps] [orbit_seconds]", "", Needs::kNothing,
     [](const Args& x) {
         return run_orbit(x.str(1), x.u32(2, 8), x.u32(3, 448), x.u32(4, 4), x.meta);
     }},
    {"atmos", 2, "<out.ec3> [seconds] [bitrate_kbps] [objects] [orbit_seconds] [mode]", "",
     Needs::kNothing,
     [](const Args& x) {
         return run_atmos(x.str(1), x.u32(2, 8), x.u32(3, 448), x.u32(4, 4), x.u32(5, 6),
                          x.str(6, "objects"), x.meta);
     }},
    {"atmos-path", 3, "<out.ec3> <paths.txt> [seconds] [bitrate_kbps] [objects]",
     "objects driven by an authored keyframe file instead of the built-in orbit",
     Needs::kNothing,
     [](const Args& x) {
         return run_atmos_path(x.str(1), x.str(2), x.u32(3, 8), x.u32(4, 448), x.u32(5, 0),
                               x.meta);
     }},
    {"atmos-encode", 3, "<in.wav> <out.ec3> [bitrate_kbps] [objects] [paths.txt]",
     "every source channel as an object; optional: authored per-object motion from a keyframe "
     "file (same format as atmos-path), objects it doesn't mention keep their default placement",
     Needs::kNothing,
     [](const Args& x) {
         return run_atmos_encode(x.str(1), x.str(2), x.u32(3, 448), x.u32(4, 0), x.meta,
                                 x.str(5));
     }},
    {"atmos-adm", 3, "<in.adm.wav> <out.ec3> [bitrate_kbps] [programme_id]",
     "a real ADM BWF master (BS.2076-2 ADM XML + BW64/RF64, roadmap B1) straight to DD+ JOC "
     "E-AC-3; every bed/object channel the resolved audioProgramme names becomes an AtmosEncoder "
     "object, driven by the file's own authored automation - no keyframe file needed. Only in "
     "builds with -DAC3FORGE_BUILD_ADM=ON",
     Needs::kAdm,
     [](const Args& x) {
         return run_atmos_adm(x.str(1), x.str(2), x.u32(3, 448), x.meta, x.str(4));
     }},
    {"record", 2, "<out.ac3> [seconds] [bitrate_kbps] [device_index]", "", Needs::kCapture,
     [](const Args& x) {
         return run_record(x.str(1), x.u32(2, 5), x.u32(3, 192), x.i32(4, 0), x.meta);
     }},
    {"live", 3,
     "<out.ac3|out.ec3> <capture_device> [seconds] [bitrate_kbps] [monitor_device] "
     "[passthrough_device] [mode]",
     "capture -> encode -> live monitor and/or passthrough", Needs::kCapture,
     [](const Args& x) {
         return run_live(x.str(1), x.i32(2, 0), x.u32(3, 10), x.u32(4, 192), x.i32(5, -2),
                         x.i32(6, -2), x.str(7, "channels"), x.meta);
     }},
    {"encode", 3, "<in.wav> <out.ac3> [bitrate_kbps] [layout] [in2.wav]",
     "in2.wav: layout 1+1's Ch2, when Ch1 is a separate mono file; or use src=/map= "
     "for more than one source",
     Needs::kNothing,
     [](const Args& x) {
         return run_encode(x.str(1), x.str(2), x.u32(3, 192), x.couple, x.str(4), x.meta,
                           x.str(5));
     }},
    {"eac3-silence", 2, "<out.ec3> [seconds] [bitrate_kbps] [layout]", "", Needs::kNothing,
     [](const Args& x) {
         return run_eac3_silence(x.str(1), x.u32(2, 5), x.u32(3, 192), x.str(4, "stereo"),
                                 x.meta);
     }},
    {"eac3-sine", 2,
     "<out.ec3> [seconds] [bitrate_kbps] [freq_hz] [amp_pct] [layout]", "", Needs::kNothing,
     [](const Args& x) {
         return run_eac3_sine(x.str(1), x.u32(2, 5), x.u32(3, 192), x.u32(4, 1000),
                              x.u32(5, 50), x.str(6, "stereo"), x.meta);
     }},
    {"eac3-encode", 3,
     "<in.wav> <out.ec3> [bitrate_kbps] [tools] [layout] [vbr] [in2.wav]",
     "in2.wav: layout 1+1's Ch2, when Ch1 is a separate mono file; or use src=/map= "
     "for more than one source",
     Needs::kNothing,
     [](const Args& x) {
         return run_eac3_encode(x.str(1), x.str(2), x.u32(3, 192), x.str(4, "none"), x.str(5),
                                x.str(6, "off"), x.meta, x.str(7));
     }},
    {"decode", 3, "<in.ac3|in.ec3> <out.wav> [objects_dir]",
     "AC-3 or E-AC-3; bsid decides. objects_dir (E-AC-3 Atmos only): export each "
     "JOC-reconstructed object as its own object_NN.wav there",
     Needs::kNothing,
     [](const Args& x) { return run_decode(x.str(1), x.str(2), x.meta, x.str(3)); }},
    {"levels", 2, "<in.wav|in.ac3|in.ec3>", "per-channel peak/RMS report", Needs::kNothing,
     [](const Args& x) { return run_levels(x.str(1)); }},
    {"loudness", 2, "<in.wav>", "BS.1770-4 loudness -> dialnorm", Needs::kNothing,
     [](const Args& x) { return run_loudness(x.str(1)); }},
    {"qc", 2, "<in.ac3|in.ec3> [preset=<name>|all]",
     "bitstream-aware loudness QC: measured loudness vs. embedded dialnorm/compr, optional "
     "preset gate",
     Needs::kNothing, [](const Args& x) { return run_qc(x.str(1), x.meta.qc_preset); }},
    {"spdif", 3, "<in.ac3> <out.wav>", "IEC 61937 wrap as playable PCM16 WAV", Needs::kNothing,
     [](const Args& x) { return run_spdif(x.str(1), x.str(2)); }},
    {"mkv", 3, "<in.ac3|in.ec3> <out.mkv>", "wrap as a playable Matroska file", Needs::kNothing,
     [](const Args& x) { return run_mkv(x.str(1), x.str(2)); }},
    {"mp4", 3, "<in.ac3|in.ec3> <out.mp4>",
     "wrap as a playable MP4 with a spec-correct dac3/dec3 box", Needs::kNothing,
     [](const Args& x) { return run_mp4(x.str(1), x.str(2)); }},
    {"fmp4", 3, "<in.ac3|in.ec3> <out_dir> [frames_per_fragment]",
     "fragmented MP4/CMAF + HLS/DASH manifests, ready for a packager", Needs::kNothing,
     [](const Args& x) { return run_fmp4(x.str(1), x.str(2), x.u32(3, 48)); }},
    {"ts", 3, "<in.ac3|in.ec3> <out.ts>", "wrap as an MPEG-2 Transport Stream (DVB profile)",
     Needs::kNothing, [](const Args& x) { return run_ts(x.str(1), x.str(2)); }},
    {"devices", 1, "", "input and loopback capture endpoints", Needs::kCapture,
     [](const Args&) { return run_devices(); }},
    {"outputs", 1, "", "render endpoints + AC-3/E-AC-3 passthrough support", Needs::kPassthrough,
     [](const Args&) { return run_outputs(); }},
    {"play", 2, "<in.ac3|in.ec3> [device_index]",
     "exclusive-mode IEC 61937 passthrough; bsid decides AC-3 vs E-AC-3", Needs::kPassthrough,
     // -1, not 0: run_play reads a negative index as "the default endpoint",
     // where 0 names the first one 'outputs' lists and demands passthrough of it.
     [](const Args& x) { return run_play(x.str(1), x.i32(2, -1)); }},
    {"monitor", 2, "<in.ac3|in.ec3> [device_index]",
     "decode and play on an ordinary (non-bitstreamed) output", Needs::kMonitor,
     [](const Args& x) { return run_monitor(x.str(1), x.i32(2, -1), x.meta); }},
}};

void print_usage() {
    std::println("ac3forge — clean-room AC-3 / E-AC-3 (ATSC A/52) encoder/decoder");
    std::println("");
    std::println("Usage:");
    std::println("  ac3cli --version    print version and git provenance, then exit");
    for (const auto& c : kCommands) {
        std::string line = std::format("  ac3cli {:<13}{}", c.name, c.spec);
        // A command the platform cannot run is listed, not hidden: hiding it
        // makes 'ac3cli play' answer "unknown command", which is a lie about
        // a command that exists and would work elsewhere. The note slot says
        // so instead, and the reasons follow once below rather than being
        // repeated on every affected row.
        const std::string_view note = unmet(c.needs) != nullptr ? "UNAVAILABLE HERE" : c.note;
        if (!note.empty()) {
            // The note column starts at 62, but 'record' has a spec longer
            // than that and no padding is applied to a line already past the
            // stop - so guarantee the separating space by hand rather than
            // letting the note run into the last argument.
            if (line.size() >= 62) {
                line += ' ';
            }
            line = std::format("{:<62}({})", line, note);
        }
        std::println("{}", line);
    }
    const auto& backend = ac3::audio::audio_backend();
    if (!backend.capture.available || !backend.passthrough.available ||
        !backend.monitor.available) {
        std::println("");
        if (!backend.capture.available) {
            std::println("UNAVAILABLE HERE — {}.", backend.capture.reason);
        }
        if (!backend.passthrough.available) {
            std::println("UNAVAILABLE HERE — {}.", backend.passthrough.reason);
        }
        if (!backend.monitor.available) {
            std::println("UNAVAILABLE HERE — {}.", backend.monitor.reason);
        }
        std::println("Everything else is file I/O and behaves identically on every platform;");
        std::println("'spdif' in particular reaches a receiver without any audio backend at all.");
    }
    std::println("");
    std::println("'-' in place of <in.wav>, <out.ac3>, <out.ec3>, <in.ac3|in.ec3> or <out.wav>");
    std::println("       means stdin (an input path) or stdout (an output path) - encode,");
    std::println("       eac3-encode, atmos-encode and decode only. e.g.:");
    std::println("       ac3cli encode - - 448 couple < in.wav > out.ac3");
    std::println("");
    std::println("live monitor_device/passthrough_device: -2 (default) leaves that leg off,");
    std::println("       -1 is the default render endpoint, N picks one from 'outputs'.");
    std::println("       Either or both may run alongside the file this always writes.");
    std::println("live mode: 'channels' (default) carries stereo straight through; 'atmos'");
    std::println("       pans every captured channel into a 5.1 bed as its own object, moving");
    std::println("       it every frame the same way 'atmos' orbits its synthetic ones — the");
    std::println("       hook a real live position source drops into once one exists.");
    std::println("live capture2=<index>: the capture_device positional stays the session's");
    std::println("       clock master, paced exactly as it always has been; capture2= adds a");
    std::println("       second, independently-clocked device whose stream is resampled to");
    std::println("       track the master, with the measured drift printed at session end.");
    std::println("record/live container=mkv: write straight to Matroska (a single command)");
    std::println("       instead of the bare elementary stream both write by default; 'mkv'");
    std::println("       remains the way to wrap an ALREADY-encoded file after the fact.");
    std::println("monitor/live --monitor play the 5.1 BED of an Atmos-mode stream: the decoder");
    std::println("       reads TS 103 420's object layer (OAMD/JOC) and reports an object count,");
    std::println("       but this path does not render or export objects, so this is what a");
    std::println("       legacy decoder hears, not unmixed objects.");
    std::println("decode objects_dir (E-AC-3 Atmos only): exports each JOC-reconstructed object");
    std::println("       as its own object_NN.wav, alongside the usual 5.1 bed WAV.");
    std::println("");
    std::println("tools:  Annex E coding tools, '+'-joined — {}", plan::kToolsSyntax);
    std::println("        cpl:N / spx:N pin that tool's band edge (e.g. cpl:4+spx:5);");
    std::println("        aht:N pins the GAQ mode — aht:0 is AHT with GAQ switched off;");
    std::println("        atten:N pins the SPX notch depth, noatten removes it");
    std::println("");
    std::println("vbr (eac3-encode only): {}", plan::kVbrSyntax);
    std::println("        quality is encoder-relative, not a fixed target — bit cost rises");
    std::println("        steeply above roughly half the range, so a high quality with no");
    std::println("        max bound will often refuse real programme material outright;");
    std::println("        bitrate_kbps still matters in vbr mode — it feeds the same");
    std::println("        coupling/spx frequency defaults it always has, not a target rate");
    std::println("atmos: objects orbit the room at different heights and rates,");
    std::println("       encoded as a 5.1 E-AC-3 bed with JOC + OAMD side data");
    std::println("       (TS 103 420). FFmpeg reports \"Dolby Digital Plus + Dolby Atmos\".");
    std::println("atmos mode: objects (default) writes the JOC+OAMD container; bed51 omits");
    std::println("       it so the 5.1 bed still plays on a decoder that refuses an object");
    std::println("       container it cannot validate instead of falling back to the bed.");
    std::println("");
    std::println("layout: {}", plan::layout_names(plan::Codec::kEac3));
    std::println("        AC-3 carries only {} — everything wider needs the dependent",
                 plan::layout_names(plan::Codec::kAc3));
    std::println("        substreams that only E-AC-3 has.");
    for (const auto& info : plan::kLayouts) {
        if (info.transmitted == info.rendered) {
            continue;
        }
        // Where the two differ, say so: a dependent that REPLACES a bed
        // channel spends coded channels a listener never counts.
        std::println("        {} renders {} speakers from {} coded channels", info.name,
                     info.rendered, info.transmitted);
    }
    std::println("        For 'sine' and 'eac3-sine' each speaker gets its own tone; append");
    std::println("        'c' to a 'sine' layout (stereoc, 51c) to enable channel coupling.");
    std::println("        For 'encode' and 'eac3-encode' it names the OUTPUT layout: a");
    std::println("        source narrower than it leaves the channels it lacks silent, and");
    std::println("        a wider one folds down per §7.8 using cmixlev/surmixlev.");
    std::println("");
    std::println("        [layout] also takes a comma-separated Table E2.5 location list");
    std::println("        instead of one of the names above, for anything Annex E allows");
    std::println("        that has no preset: e.g. L,C,R,LFE,Vhl,Vhr or L,C,R,LFE,LFE2,Vhc.");
    std::println("        AC-3 accepts one too, as long as it needs no dependent substream");
    std::println("        (e.g. L,R,Cs or L,C,R,Cs - Table 5.8 modes no preset names).");
    std::println("        Locations: L C R Ls Rs Lc Rc Lrs Rrs Cs Ts Lsd Rsd Lw Rw Vhl Vhr");
    std::println("        Vhc Lts Rts LFE2 LFE - a paired location (Lc/Rc, Lrs/Rrs, Lsd/Rsd,");
    std::println("        Lw/Rw, Vhl/Vhr, Lts/Rts) must be given both halves.");
    std::println("");
    std::println("atmos: objects orbit the room at different heights and rates;");
    std::println("       atmos-encode makes each channel of a real file an object instead.");
    std::println("       Both emit a 5.1 E-AC-3 bed with JOC + OAMD side data (TS 103 420).");
    std::println("       FFmpeg reports \"Dolby Digital Plus + Dolby Atmos\".");
    std::println("       atmos-encode's [paths.txt] takes authored per-object motion the same");
    std::println("       way atmos-path does, keyed by WAV channel index; an object it doesn't");
    std::println("       mention keeps atmos-encode's own default (fanned-out) placement.");
    std::println("");
    std::println("mkv wraps an AC-3 or E-AC-3 elementary stream in Matroska, taking the");
    std::println("format, packet boundaries, sample rate and channel count from the bitstream");
    std::println("itself — so it cannot be told the wrong ones. E-AC-3 dependent substreams");
    std::println("are grouped into their access unit and counted as the channels they render.");
    std::println("");
    std::println("fmp4 writes a fragmented MP4/CMAF init segment plus one media segment per");
    std::println("fragment (frames_per_fragment access units each, default 48 - about 1.5s at");
    std::println("48 kHz), alongside an HLS media+master playlist pair and a DASH MPD, all");
    std::println("pointing at the same segments (CMAF's whole point) — ready for a real HLS/");
    std::println("DASH origin or packager. Dolby Atmos content signals CHANNELS=\"<N>/JOC\" in");
    std::println("the HLS playlists automatically, per Apple's HLS Authoring Specification.");
    std::println("");
    std::println("ts wraps the same elementary stream as an MPEG-2 Transport Stream (PAT + PMT");
    std::println("+ one PES-wrapped audio PID), identified per the DVB profile — stream_type");
    std::println("0x06 plus the AC3_descriptor or Enhanced_AC3_descriptor ETSI EN 300 468 Annex D");
    std::println("defines, not ATSC's — with PCR stamped on the audio PID every access unit.");
    std::println("");
    std::println("Without a layout, encode and eac3-encode both follow the source: 1 -> mono,");
    std::println("2 -> stereo, 3 to 6 -> 5.1; eac3-encode alone extends that to 8 -> 7.1,");
    std::println("10 -> 5.1.4, 12 -> 7.1.4 (encode refuses anything wider than 3/2 + LFE).");
    std::println("Commands that carry PCM report per-channel levels when they finish; 'record'");
    std::println("meters live. 'couple' turns on channel coupling wherever a command encodes.");
    print_meta_usage();
    std::println("");
    std::println("For decode, drc=<scale> applies §7.7.1 partial compression (0 = ignore,");
    std::println("1 = as encoded) and 'heavy' prefers compr where the stream carries it.");
    std::println("");
    std::println("qc measures a stream's real BS.1770-4/EBU Tech 3342 loudness and compares it");
    std::println("       against the dialnorm/compr it embeds - preset=<name> also gates that");
    std::println("       measurement against a named delivery spec ({}),", ac3::meta::kQcPresetNames);
    std::println("       or preset=all checks every one; omitting preset= just measures and");
    std::println("       reports, with no pass/fail verdict. Exit code is 0 only when every");
    std::println("       requested gate passes (or none was requested and decode succeeded).");
}

}  // namespace

int run_main(int argc, char** argv) {
    const std::span<char*> raw{argv, static_cast<std::size_t>(argc)};
    if (raw.size() > 1 &&
        (std::string_view{raw[1]} == "--version" || std::string_view{raw[1]} == "-v")) {
        std::println("{}", ac3::version_details());
        return 0;
    }
    // Split the command line into positional arguments and metadata options. An
    // option is a key=value token or one of the bare flags, so the positional
    // arguments keep their places whether options are present or not, and
    // options may appear in any order.
    std::vector<char*> args{};      // args[0] is the command
    std::vector<char*> options{};
    bool couple_flag = false;
    for (std::size_t i = 1; i < raw.size(); ++i) {
        const std::string_view token{raw[i]};
        const bool is_option = token.find('=') != std::string_view::npos ||
                               token == "couple" || token == "heavy" || token == "heavy2" ||
                               token == "mixmeta" || token == "sign-objects" ||
                               token == "verify-objects" || token == "keep-partial" ||
                               token == "fast-mdct" || token == "fast-imdct";
        if (token == "couple") {
            couple_flag = true;
        }
        (is_option ? options : args).push_back(raw[i]);
    }
    Options meta;
    if (!parse_options(options, meta)) {
        return 1;
    }
    if (args.empty()) {
        print_usage();
        return 0;
    }

    const std::string_view command{args[0]};
    for (const auto& c : kCommands) {
        if (c.name != command) {
            continue;
        }
        if (args.size() < c.min_args) {
            std::println(stderr, "error: {} needs {}", c.name, c.spec);
            return 1;
        }
        // Refuse before the handler runs, so a command that cannot work here
        // says why once, in the platform's own words, instead of failing
        // partway through with whatever error code the no-backend stub
        // happened to return. Nothing silently does nothing.
        if (const auto* missing = unmet(c.needs)) {
            std::println(stderr, "error: '{}' is unavailable on this platform: {}", c.name,
                         missing->reason);
            if (c.needs == Needs::kPassthrough) {
                // The one live-audio capability with a portable substitute:
                // same bursts, written to a file instead of an endpoint.
                std::println(stderr,
                             "  'ac3cli spdif <in.ac3> <out.wav>' wraps the same IEC 61937 "
                             "bursts into a WAV that any player will pass through untouched.");
            }
            return 1;
        }
        return c.run(Args{args, meta, couple_flag});
    }
    std::println(stderr, "error: unknown command '{}'", command);
    print_usage();
    return 1;
}

// run_main is std::expected-clean throughout; the one realistic exception
// source left is std::format/std::println itself (std::format_error), which
// nothing here catches internally. Left uncaught, that unwinds out of main
// and terminates - a crash with no exit code a script could act on rather
// than the ordinary "error: ..." this CLI otherwise always prints on
// failure. This is the one place that catches it. clang-tidy still flags
// main() itself: it cannot see past this try/catch to know the escape is
// caught, and reports the one path it cannot fully close by construction -
// the catch block's own std::println, whose fixed one-argument format string
// has no realistic way to throw. NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv) {
    try {
        return run_main(argc, argv);
    } catch (const std::exception& e) {
        std::println(stderr, "error: unhandled exception: {}", e.what());
        return 1;
    } catch (...) {
        std::println(stderr, "error: unhandled exception of unknown type");
        return 1;
    }
}
