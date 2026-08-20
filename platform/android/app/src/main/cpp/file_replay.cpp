// A diagnostic-only playback path, separate from live_cursor.cpp's synthesized
// encode loop: streams a real, already-encoded AC-3/E-AC-3 file (e.g. an
// audio track pulled straight out of a commercial Dolby Atmos demo trailer's
// MKV, no re-encoding at all) through the exact same PassthroughSink this
// app's own encoder output goes through. Mirrors ac3cli's run_play
// (src/cli/main.cpp) almost exactly - same split_access_units/BurstPacker/
// submit-with-retry shape - just triggered over JNI instead of a CLI arg.
//
// Purpose: isolate "is this app's AudioTrack passthrough configuration
// correct" from "is our own encoder's Atmos/JOC output correct" - a real,
// known-good Dolby-encoded Atmos stream either lights up the receiver's Atmos
// indicator through this exact same code path, or it doesn't, independent of
// anything ac3::forge or the quarantine signer did.

#include <jni.h>

#include <android/log.h>

#include <chrono>
#include <cstddef>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "ac3/decoder/decoder.hpp"
#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/sinks/iec61937.hpp"
#include "ac3/audio/passthrough.hpp"

namespace {

constexpr char kLogTag[] = "ac3forge.shield.file_replay";

// ac3::split_access_units groups syncframes by reading strmtyp from each
// frame's own header - correct for a stream whose independent substream uses
// bsid 16 (every generation this project's own encoder and, apparently, most
// test material produces), but strmtyp only lives at that byte position in a
// genuine Annex E (bsid 11-16) frame. A real commercial disc encountered here
// authors its independent substream at bsid 6 (spec-legal - see below) with
// its Atmos-carrying dependent substream right behind it at bsid 16; for the
// bsid-6 frame, the byte split_access_units reads as strmtyp is really part
// of that frame's crc1 - essentially random per-frame content - so roughly a
// quarter of the time it *happens* to alias to "dependent" and swallows the
// next several access units into one runaway group (confirmed against this
// exact file: 176 of 480 groups corrupted this way, the largest merging 24
// frames into one 122880-byte non-burst). bsid itself is not random - it is
// the frame's own declared identity, in the same fixed position for exactly
// this reason (A/52 Annex E: "bsid sits at bit 40 in both generations") - so
// grouping on "does this frame's bsid fall in the E-AC-3 dependent range"
// instead is deterministic and, empirically, correct throughout this file.
// Kept local to this diagnostic rather than changed in the shared decoder:
// ac3::split_access_units is exercised well beyond this one file/tool, and
// this project's own encoder never emits a non-16 leading bsid, so nothing
// here should risk a broader, less-tested change to that shared code path.
std::expected<std::vector<std::span<const std::byte>>, ac3::DecodeError> group_by_bsid(
    std::span<const std::byte> stream) {
    const auto frames = ac3::split_frames(stream);
    if (!frames) return std::unexpected(frames.error());

    std::vector<std::span<const std::byte>> units;
    std::size_t start = 0;
    std::size_t offset = 0;
    for (const auto& frame : *frames) {
        const auto bsid = std::to_integer<int>(frame[5]) >> 3;
        const bool begins_unit = !(bsid >= ac3::eac3::kMinDecodableBsid && bsid <= ac3::eac3::kBsid);
        if (begins_unit && offset != start) {
            units.push_back(stream.subspan(start, offset - start));
            start = offset;
        }
        offset += frame.size();
    }
    if (offset != start) {
        units.push_back(stream.subspan(start, offset - start));
    }
    return units;
}

std::vector<std::byte> read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const auto size = in.tellg();
    if (size <= 0) return {};
    std::vector<std::byte> data(static_cast<std::size_t>(size));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(data.data()), size);
    // A short read left the untouched tail as zero-initialized garbage that
    // still looks like a full-size buffer to every caller - silently
    // returning it produced a bitstream that was byte-correct up to the
    // short point and corrupt after, which read as a parse failure deep in
    // the file rather than the read failure it actually was.
    if (in.gcount() != size) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "%s: short read (%lld of %lld bytes)", path.c_str(),
                            static_cast<long long>(in.gcount()), static_cast<long long>(size));
        return {};
    }
    return data;
}

bool play_file(const std::string& path) {
    const auto stream = read_all(path);
    if (stream.empty()) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "cannot read %s", path.c_str());
        return false;
    }

    // Always E-AC-3, unlike ac3cli's run_play (which also handles plain
    // .ac3): this diagnostic exists specifically to play real commercial
    // Dolby Atmos/DD+ content (see file header). group_by_bsid, not
    // ac3::split_access_units - see that function's own comment for why.
    const auto split = group_by_bsid(stream);
    if (!split) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s: frame split failed: %s", path.c_str(),
                            std::string(ac3::describe(split.error())).c_str());
        return false;
    }
    if (split->empty()) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s: no access units found", path.c_str());
        return false;
    }
    const auto& units = *split;
    const std::uint32_t content_rate = ac3::sample_rate_hz(
        static_cast<ac3::SampleRate>(std::to_integer<std::uint32_t>(units[0][4]) >> 6));

    ac3::audio::PassthroughSink sink;
    const auto started = sink.start("", content_rate, ac3::audio::BitstreamFormat::kEac3);
    if (!started) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "PassthroughSink::start failed: %s",
                            std::string(ac3::audio::describe(started.error())).c_str());
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "streaming %zu access units from %s (%u Hz, carrier 4x that)",
                        units.size(), path.c_str(), content_rate);

    ac3::iec61937::Eac3BurstPacker eac3_packer;
    for (const auto& unit : units) {
        auto result = eac3_packer.push(unit);
        if (!result) {
            __android_log_print(ANDROID_LOG_ERROR, kLogTag, "burst wrap failed");
            return false;
        }
        if (!*result) continue;  // accumulating; nothing to submit yet
        const auto& burst = **result;
        while (!sink.submit(burst)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    }
    while (sink.stats().bursts_rendered < sink.stats().bursts_submitted) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto stats = sink.stats();
    sink.stop();
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "done: submitted=%llu rendered=%llu underruns=%llu",
                        static_cast<unsigned long long>(stats.bursts_submitted),
                        static_cast<unsigned long long>(stats.bursts_rendered),
                        static_cast<unsigned long long>(stats.underruns));
    return true;
}

}  // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_com_ac3forge_shield_NativeBridge_nativePlayEac3File(JNIEnv* env, jclass /*clazz*/,
                                                          jstring jpath) {
    const char* raw = env->GetStringUTFChars(jpath, nullptr);
    const std::string path(raw != nullptr ? raw : "");
    if (raw != nullptr) env->ReleaseStringUTFChars(jpath, raw);
    return play_file(path) ? JNI_TRUE : JNI_FALSE;
}
