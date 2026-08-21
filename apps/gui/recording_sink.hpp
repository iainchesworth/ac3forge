#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <span>
#include <string>

#include "ac3/io/wav.hpp"
#include "ac3/sinks/iec61937.hpp"
#include "matroska/matroska.hpp"
#include "mpegts/mpegts.hpp"

// Streams a recording's encoded frames to their destination as they are
// produced, for the containers whose format allows it - which is what turns
// a take's memory footprint from "the whole session" (~3.4 MB per minute at
// 448 kbps, unbounded until Stop) into one frame, and puts the take's bytes
// on disk continuously instead of only at a clean stop, so a crash an hour
// in no longer loses the hour.
//
// The four containers here are exactly the streamable ones: a bare
// elementary stream appends; Matroska streams via matroska::Writer (EBML's
// own unknown-size Segment pattern - the file differs from the one-shot
// mux()'s by exactly that, as that class's comment describes); MPEG-TS via
// mpegts::Writer (whose bytes are contract-identical to mux()'s); the
// IEC 61937 WAV carrier via per-frame wrapping into
// ac3::io::WavPcm16StreamWriter, whose closed file is byte-identical to
// write_wav_pcm16_raw over the same bursts. MP4 and fMP4 are deliberately
// absent: moov/stco need every frame's final offset, and the dec3/dac3
// codec-config box needs a bitstream scan - the accumulate-then-mux shape
// in EncoderController::writeOutput IS their streaming-less design (see
// mp4's own "deliberately no monolithic Writer" record), so the recording
// loop keeps that shape for them.
//
// Qt-free on purpose: everything here is std:: and the container libraries,
// so tests/gui's plain C++ test can drive it against the one-shot writers
// without a QML engine in the room. Errors come back as the user-facing
// strings EncoderController's status line already shows - empty means fine.
class RecordingSink {
   public:
    enum class Container : std::uint8_t {
        kElementary,
        kMatroska,
        kSpdif,
        kMpegts,
    };

    struct Config {
        Container container = Container::kElementary;
        bool eac3 = false;
        std::uint32_t sample_rate = 48000;
        int channels = 2;
    };

    // Empty on success. A failure here happens before any capture is worth
    // starting - the file could not be created, or the track refused to
    // validate - so the caller can surface it immediately rather than at
    // the end of a take.
    [[nodiscard]] std::string open(const std::string& path, const Config& config);

    // Empty on success. On failure the bytes already written stay on disk -
    // a partial take is a take, unlike a failed file encode whose input
    // still exists - and the caller decides whether to keep recording.
    [[nodiscard]] std::string push(std::span<const std::byte> frame);

    // Finalizes the container. With zero frames pushed, removes the file
    // and reports "Nothing was encoded." - the whole-buffer path this
    // replaces never created a file at all in that case.
    [[nodiscard]] std::string close();

    [[nodiscard]] std::size_t frames() const { return frames_; }

   private:
    [[nodiscard]] bool write_file(std::span<const std::byte> bytes);

    Config config_;
    std::string path_;
    bool open_ = false;
    std::size_t frames_ = 0;
    // kElementary / kMatroska / kMpegts write here...
    std::ofstream file_;
    // ...through these; kSpdif goes through wav_ instead.
    std::optional<matroska::Writer> matroska_;
    std::optional<mpegts::Writer> mpegts_;
    ac3::io::WavPcm16StreamWriter wav_;
    ac3::iec61937::Eac3BurstPacker packer_;
};
