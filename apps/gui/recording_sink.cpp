#include "recording_sink.hpp"

#include <filesystem>
#include <system_error>
#include <utility>

#include "ac3/core/tables.hpp"

namespace {

// The user-facing strings match EncoderController::writeOutput's for the
// same failures, so a streamed take and a whole-buffer one report a broken
// disk in the same words.
constexpr const char* kCannotOpen = "Could not open the output file for writing.";
constexpr const char* kCannotWrap = "Could not wrap the stream into IEC 61937 bursts.";
constexpr const char* kNothingEncoded = "Nothing was encoded.";

const char* write_failed_for(RecordingSink::Container container) {
    switch (container) {
        case RecordingSink::Container::kMatroska:
            return "Writing the Matroska file failed.";
        case RecordingSink::Container::kMpegts:
            return "Writing the MPEG-TS file failed.";
        case RecordingSink::Container::kSpdif:
            return "Writing the WAV carrier failed.";
        case RecordingSink::Container::kElementary:
            break;
    }
    return "Writing the stream failed.";
}

}  // namespace

std::string RecordingSink::open(const std::string& path, const Config& config) {
    config_ = config;
    path_ = path;
    frames_ = 0;

    if (config.container == Container::kSpdif) {
        // The carrier runs at 4x the content rate for E-AC-3 - see
        // ac3cli's own run_spdif (apps/cli/main.cpp) for the citation.
        const auto carrier_rate =
            config.eac3 ? config.sample_rate * 4 : config.sample_rate;
        if (!wav_.open(path, carrier_rate, 2)) {
            return kCannotOpen;
        }
        packer_ = {};
        open_ = true;
        return {};
    }

    if (config.container == Container::kMatroska) {
        auto writer = matroska::Writer::create(matroska::AudioTrack{
            .codec_id = std::string{config.eac3 ? matroska::kCodecEac3 : matroska::kCodecAc3},
            .sample_rate = config.sample_rate,
            .channels = config.channels,
            .samples_per_frame = ac3::kSamplesPerFrame});
        if (!writer) {
            return std::string{matroska::describe(writer.error())};
        }
        matroska_.emplace(std::move(*writer));
    } else if (config.container == Container::kMpegts) {
        auto writer = mpegts::Writer::create(mpegts::AudioTrack{
            .codec = config.eac3 ? mpegts::AudioCodec::kEac3 : mpegts::AudioCodec::kAc3,
            .sample_rate = config.sample_rate,
            .channels = config.channels,
            .samples_per_frame = ac3::kSamplesPerFrame});
        if (!writer) {
            return std::string{mpegts::describe(writer.error())};
        }
        mpegts_.emplace(std::move(*writer));
    }

    file_.open(path, std::ios::binary);
    if (!file_) {
        return kCannotOpen;
    }
    if (matroska_ && !write_file(matroska_->header())) {
        return write_failed_for(config_.container);
    }
    open_ = true;
    return {};
}

std::string RecordingSink::push(std::span<const std::byte> frame) {
    switch (config_.container) {
        case Container::kElementary:
            if (!write_file(frame)) {
                return write_failed_for(config_.container);
            }
            break;
        case Container::kMatroska: {
            auto closed = matroska_->push(frame);
            if (!closed) {
                return std::string{matroska::describe(closed.error())};
            }
            if (!closed->empty() && !write_file(*closed)) {
                return write_failed_for(config_.container);
            }
            break;
        }
        case Container::kMpegts: {
            auto packets = mpegts_->push(frame);
            if (!packets) {
                return std::string{mpegts::describe(packets.error())};
            }
            if (!write_file(*packets)) {
                return write_failed_for(config_.container);
            }
            break;
        }
        case Container::kSpdif: {
            if (config_.eac3) {
                auto burst = packer_.push(frame);
                if (!burst) {
                    return kCannotWrap;
                }
                if (*burst && !wav_.write(**burst)) {
                    return write_failed_for(config_.container);
                }
            } else {
                auto burst = ac3::iec61937::wrap_frame(frame);
                if (!burst) {
                    return kCannotWrap;
                }
                if (!wav_.write(*burst)) {
                    return write_failed_for(config_.container);
                }
            }
            // One frame is 32 ms, so this patches the carrier's header
            // about once a second - WavPcm16StreamWriter::flush_header's
            // own crash-worst-case rationale.
            if (frames_ % 32 == 31) {
                wav_.flush_header();
            }
            break;
        }
    }
    ++frames_;
    return {};
}

std::string RecordingSink::close() {
    if (!open_) {
        return {};
    }
    open_ = false;
    if (frames_ == 0) {
        // The whole-buffer path never created a file for an empty take;
        // matching that means removing the one open() already created.
        wav_.close();
        file_.close();
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path{path_}, ec);
        return kNothingEncoded;
    }
    if (config_.container == Container::kSpdif) {
        // An E-AC-3 tail that never completed a burst is dropped, exactly
        // as the one-shot wrap_stream drops it.
        wav_.close();
        return {};
    }
    if (matroska_ && !write_file(matroska_->finalize())) {
        return write_failed_for(config_.container);
    }
    if (mpegts_) {
        // Always empty by contract; called so the two writers age uniformly.
        std::ignore = mpegts_->finalize();
    }
    file_.close();
    if (file_.fail()) {
        return write_failed_for(config_.container);
    }
    return {};
}

bool RecordingSink::write_file(std::span<const std::byte> bytes) {
    if (bytes.empty()) {
        return static_cast<bool>(file_);
    }
    file_.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file_);
}
