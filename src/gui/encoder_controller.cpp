#include "encoder_controller.hpp"

#include <QFileInfo>
#include <QtConcurrent/QtConcurrentRun>

#include <iterator>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/sinks/iec61937.hpp"

namespace {

// AC-3 accepts only these three rates (A/52 Table 5.6).
std::optional<ac3::SampleRate> to_sample_rate(std::uint32_t hz) {
    switch (hz) {
        case 48000: return ac3::SampleRate::k48000;
        case 44100: return ac3::SampleRate::k44100;
        case 32000: return ac3::SampleRate::k32000;
        default: return std::nullopt;
    }
}

QString to_qstring(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

// Live meters redraw no faster than this. A file encodes far quicker than it
// plays, so without a wall-clock throttle a two-minute track would fire tens
// of thousands of property updates the display could never show.
constexpr auto kPublishInterval = std::chrono::milliseconds(33);

}  // namespace

struct EncoderController::Source {
    ac3::io::WavData wav;
    ac3::io::Ac3Layout layout;
};

EncoderController::EncoderController(QObject* parent) : QObject(parent) {
    refreshCaptureDevices();
    refreshOutputDevices();
}

EncoderController::~EncoderController() = default;

void EncoderController::refreshCaptureDevices() {
    QStringList names;
    devices_.clear();
    if (auto found = ac3::capture::enumerate_devices()) {
        devices_ = std::move(*found);
        for (const auto& device : devices_) {
            names.append(QString::fromStdString(device.name) +
                         (device.is_default ? QStringLiteral("  [default]") : QString()));
        }
    }
    if (names != capture_devices_) {
        capture_devices_ = names;
        emit captureDevicesChanged();
    }
}

void EncoderController::setBitrateKbps(int kbps) {
    if (kbps == bitrate_kbps_) {
        return;
    }
    bitrate_kbps_ = kbps;
    emit bitrateChanged();
}

void EncoderController::setStatus(const QString& text) {
    if (text == status_) {
        return;
    }
    status_ = text;
    emit statusChanged();
}

void EncoderController::setBusy(bool busy) {
    if (busy == busy_) {
        return;
    }
    busy_ = busy;
    emit busyChanged();
}

void EncoderController::setProgress(double value) {
    if (qFuzzyCompare(value + 1.0, progress_ + 1.0)) {
        return;
    }
    progress_ = value;
    emit progressChanged();
}

void EncoderController::setMetering(bool metering) {
    if (metering == metering_) {
        return;
    }
    metering_ = metering;
    emit meteringChanged();
}

// ---------------------------------------------------------------------------
// Metering. Every figure the meters draw — including where a level sits on
// the bar — comes from ac3::analysis, so the GUI and ac3cli cannot disagree
// about the same audio.
// ---------------------------------------------------------------------------

void EncoderController::setLayout(ac3::Acmod acmod, bool lfe) {
    acmod_ = acmod;
    lfe_ = lfe;
    channel_names_.clear();
    const int count = ac3::analysis::channel_count(acmod, lfe);
    for (int ch = 0; ch < count; ++ch) {
        channel_names_.append(to_qstring(ac3::analysis::channel_name(acmod, lfe, ch)));
    }
    layout_name_ = to_qstring(ac3::analysis::layout_name(acmod, lfe));
    emit layoutChanged();
    // Start silent: leaving the previous source's levels under the new
    // source's labels would put a number against the wrong channel.
    publishLevels(std::vector<ac3::analysis::ChannelLevel>(static_cast<std::size_t>(count)));
}

void EncoderController::clearLayout() {
    channel_names_.clear();
    layout_name_.clear();
    channel_levels_.clear();
    soundfield_.clear();
    setMetering(false);
    emit layoutChanged();
    emit levelsChanged();
}

void EncoderController::publishLevels(std::span<const ac3::analysis::ChannelLevel> levels) {
    QVariantList entries;
    entries.reserve(static_cast<qsizetype>(levels.size()));
    for (std::size_t ch = 0; ch < levels.size(); ++ch) {
        const auto& level = levels[ch];
        const auto azimuth =
            ac3::analysis::channel_azimuth_deg(acmod_, lfe_, static_cast<int>(ch));
        entries.append(QVariantMap{
            {QStringLiteral("peakDb"), level.peak_db},
            {QStringLiteral("rmsDb"), level.rms_db},
            {QStringLiteral("holdDb"), level.hold_db},
            {QStringLiteral("clipped"), level.clipped},
            // Bar positions are computed here rather than in QML: a front end
            // that mapped decibels its own way would quietly disagree with
            // every other reading of the same signal.
            {QStringLiteral("peak"),
             ac3::analysis::meter_fraction(level.peak_db, kMeterFloorDb)},
            {QStringLiteral("rms"), ac3::analysis::meter_fraction(level.rms_db, kMeterFloorDb)},
            {QStringLiteral("hold"), ac3::analysis::meter_fraction(level.hold_db, kMeterFloorDb)},
            {QStringLiteral("azimuthDeg"), azimuth.value_or(0.0)},
            {QStringLiteral("directional"), azimuth.has_value()},
        });
    }
    channel_levels_ = std::move(entries);

    const auto field = ac3::analysis::energy_vector(levels, acmod_);
    soundfield_ = QVariantMap{
        {QStringLiteral("azimuthDeg"), field.azimuth_deg},
        {QStringLiteral("magnitude"), field.magnitude},
        {QStringLiteral("levelDb"), field.level_db},
        {QStringLiteral("active"), field.magnitude > 0.0},
    };
    emit levelsChanged();
}

void EncoderController::publishSummary(const ac3::analysis::LevelMeter& meter) {
    // The exact whole-run figures, not the ballistic tail: once a run is over
    // there is a right answer, and the display should settle on it.
    std::vector<ac3::analysis::ChannelLevel> levels(
        static_cast<std::size_t>(meter.channel_count()));
    for (std::size_t ch = 0; ch < levels.size(); ++ch) {
        const auto& stats = meter.summary()[ch];
        levels[ch].peak_db = stats.peak_db();
        levels[ch].hold_db = stats.peak_db();
        levels[ch].rms_db = stats.rms_db();
        levels[ch].clipped = stats.clipped_samples > 0;
    }
    publishLevels(levels);
}

void EncoderController::refreshOutputDevices() {
    QStringList names;
    outputs_.clear();
    if (auto found = ac3::sinks::enumerate_render_devices()) {
        outputs_ = std::move(*found);
        for (const auto& device : outputs_) {
            // The capability is part of the label: a user staring at a greyed
            // out device deserves to know which of the two reasons applies.
            const QString capability =
                device.supports_ac3_passthrough
                    ? QStringLiteral("AC-3 ready")
                    : (device.supports_exclusive_pcm ? QStringLiteral("cannot bitstream")
                                                     : QStringLiteral("no exclusive access"));
            names.append(QStringLiteral("%1  —  %2")
                             .arg(QString::fromStdString(device.name), capability));
        }
    }
    if (names != output_devices_) {
        output_devices_ = names;
        emit outputDevicesChanged();
    }
}

void EncoderController::playToReceiver(int deviceIndex) {
    if (playing_ || busy_ || output_path_.isEmpty()) {
        return;
    }
    if (deviceIndex < 0 || static_cast<std::size_t>(deviceIndex) >= outputs_.size()) {
        setStatus(QStringLiteral("Choose an output device first."));
        return;
    }
    const auto device = outputs_[static_cast<std::size_t>(deviceIndex)];
    if (!device.supports_ac3_passthrough) {
        setStatus(QStringLiteral("\"%1\" will not accept AC-3 over IEC 61937. Only S/PDIF and "
                                 "HDMI outputs can bitstream, and Dolby Digital must be enabled "
                                 "for the device in Sound settings.")
                      .arg(QString::fromStdString(device.name)));
        return;
    }

    playing_ = true;
    emit playingChanged();
    setStatus(QStringLiteral("Streaming to %1…").arg(QString::fromStdString(device.name)));

    const QString path = output_path_;
    std::ignore = QtConcurrent::run([this, path, device] {
        std::ifstream in{path.toStdString(), std::ios::binary};
        const std::vector<char> raw{std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>()};
        std::vector<std::byte> stream(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) {
            stream[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
        }

        QString message;
        const auto frames = ac3::split_frames(stream);
        if (!frames || frames->empty()) {
            message = QStringLiteral("That file is not a valid AC-3 stream.");
        } else {
            const auto fscod = std::to_integer<std::uint32_t>((*frames)[0][4]) >> 6;
            const auto rate = sample_rate_hz(static_cast<ac3::SampleRate>(fscod));
            ac3::sinks::PassthroughSink sink;
            const auto started = sink.start(device.id, rate);
            if (!started) {
                const auto why = ac3::sinks::describe(started.error());
                message = QString::fromUtf8(why.data(), static_cast<qsizetype>(why.size()));
            } else {
                for (const auto& frame : *frames) {
                    const auto burst = ac3::iec61937::wrap_frame(frame);
                    if (!burst) {
                        break;
                    }
                    while (!sink.submit(*burst)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(4));
                    }
                }
                while (sink.stats().bursts_rendered < sink.stats().bursts_submitted) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                const auto stats = sink.stats();
                sink.stop();
                message = QStringLiteral("Streamed %1 bursts (%2 underruns).")
                              .arg(stats.bursts_rendered)
                              .arg(stats.underruns);
            }
        }

        QMetaObject::invokeMethod(this, [this, message] {
            playing_ = false;
            emit playingChanged();
            setStatus(message);
        });
    });
}

void EncoderController::setRecording(bool recording) {
    if (recording == recording_) {
        return;
    }
    recording_ = recording;
    emit recordingChanged();
}

void EncoderController::stopRecording() {
    stop_recording_.store(true, std::memory_order_relaxed);
}

void EncoderController::startRecording(int deviceIndex, const QUrl& url) {
    if (busy_ || recording_) {
        return;
    }
    if (deviceIndex < 0 || static_cast<std::size_t>(deviceIndex) >= devices_.size()) {
        setStatus(QStringLiteral("Choose a capture device first."));
        return;
    }
    const auto device = devices_[static_cast<std::size_t>(deviceIndex)];
    const auto rate = to_sample_rate(device.sample_rate);
    if (!rate) {
        setStatus(QStringLiteral("\"%1\" runs at %2 Hz; AC-3 needs 32, 44.1 or 48 kHz. "
                                 "Change the endpoint's shared-mode format in Windows sound "
                                 "settings.")
                      .arg(QString::fromStdString(device.name))
                      .arg(device.sample_rate));
        return;
    }

    capture_ = std::make_unique<ac3::capture::Capture>();
    const auto started = capture_->start(device.id, device.kind);
    if (!started) {
        const auto why = ac3::capture::describe(started.error());
        capture_.reset();
        setStatus(QStringLiteral("Could not open \"%1\": %2")
                      .arg(QString::fromStdString(device.name),
                           QString::fromUtf8(why.data(), static_cast<qsizetype>(why.size()))));
        return;
    }

    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    output_path_ = path;
    emit outputChanged();

    stop_recording_.store(false, std::memory_order_relaxed);
    setRecording(true);
    setBusy(true);
    recorded_seconds_ = 0.0;
    emit recordedSecondsChanged();
    // Capture is folded to a stereo pair before it reaches the encoder, so
    // that — not the endpoint's own channel count — is what the meters show.
    setLayout(ac3::Acmod::k2_0, false);
    setMetering(true);
    setStatus(QStringLiteral("Recording from %1…").arg(QString::fromStdString(device.name)));

    const int bitrate = bitrate_kbps_;
    const auto channels = capture_->channels();
    const auto sample_rate = capture_->sample_rate();

    std::ignore = QtConcurrent::run([this, path, rate = *rate, bitrate, channels,
                                     sample_rate]() {
        ac3::FrameEncoder encoder{
            {.sample_rate = rate, .bitrate_kbps = static_cast<std::uint32_t>(bitrate)}};
        ac3::analysis::LevelMeter meter{ac3::Acmod::k2_0, false, sample_rate};
        std::ofstream out{path.toStdString(), std::ios::binary};
        if (!out) {
            QMetaObject::invokeMethod(this, [this] {
                capture_->stop();
                capture_.reset();
                setRecording(false);
                setBusy(false);
                setMetering(false);
                setStatus(QStringLiteral("Could not open the output file for writing."));
                emit encodeFinished(false, status());
            });
            return;
        }

        std::vector<float> interleaved(static_cast<std::size_t>(ac3::kSamplesPerFrame) *
                                       channels);
        std::vector<std::vector<float>> planar(2,
                                               std::vector<float>(ac3::kSamplesPerFrame, 0.0f));
        std::vector<std::span<const float>> views{planar[0], planar[1]};
        std::size_t frames = 0;

        while (!stop_recording_.load(std::memory_order_relaxed)) {
            std::size_t filled = 0;
            while (filled < interleaved.size() &&
                   !stop_recording_.load(std::memory_order_relaxed)) {
                const auto got = capture_->buffer()->read(
                    std::span{interleaved}.subspan(filled, interleaved.size() - filled));
                filled += got;
                if (got == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
            if (filled < interleaved.size()) {
                break;  // stopped mid-frame; drop the partial frame
            }

            for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                const std::size_t base = static_cast<std::size_t>(i) * channels;
                const float left = interleaved[base];
                planar[0][static_cast<std::size_t>(i)] = left;
                planar[1][static_cast<std::size_t>(i)] =
                    channels > 1 ? interleaved[base + 1] : left;
            }
            meter.process(views);

            const auto frame = encoder.encode_frame(views);
            if (!frame) {
                break;
            }
            out.write(reinterpret_cast<const char*>(frame->data()),
                      static_cast<std::streamsize>(frame->size()));
            ++frames;

            // A frame is 32 ms at 48 kHz, so publishing one snapshot per frame
            // already lands close to 30 Hz without any extra throttling.
            const double seconds = static_cast<double>(frames * ac3::kSamplesPerFrame) /
                                   static_cast<double>(sample_rate);
            std::vector<ac3::analysis::ChannelLevel> snapshot(meter.levels().begin(),
                                                              meter.levels().end());
            QMetaObject::invokeMethod(this, [this, seconds, snapshot = std::move(snapshot)] {
                recorded_seconds_ = seconds;
                emit recordedSecondsChanged();
                publishLevels(snapshot);
            });
        }
        out.close();

        std::vector<ac3::analysis::ChannelLevel> totals(
            static_cast<std::size_t>(meter.channel_count()));
        for (std::size_t ch = 0; ch < totals.size(); ++ch) {
            const auto& stats = meter.summary()[ch];
            totals[ch].peak_db = stats.peak_db();
            totals[ch].hold_db = stats.peak_db();
            totals[ch].rms_db = stats.rms_db();
            totals[ch].clipped = stats.clipped_samples > 0;
        }

        QMetaObject::invokeMethod(this, [this, frames, totals = std::move(totals)] {
            const auto stats = capture_->stats();
            capture_->stop();
            capture_.reset();
            setRecording(false);
            setBusy(false);
            setMetering(false);
            setStatus(QStringLiteral("Recorded %1 frames to %2 (%3 dropped, %4 silence-filled)")
                          .arg(frames)
                          .arg(QFileInfo(output_path_).fileName())
                          .arg(stats.frames_dropped)
                          .arg(stats.frames_silence_filled));
            emit recordedSecondsChanged();
            publishLevels(totals);
            emit encodeFinished(true, status());
        });
    });
}

void EncoderController::loadSourceFile(const QUrl& url) {
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    auto wav = ac3::io::read_wav(path.toStdString());
    if (!wav) {
        source_ready_ = false;
        source_path_ = path;
        source_info_.clear();
        clearLayout();
        emit sourceChanged();
        setStatus(QStringLiteral("Could not read %1: %2")
                      .arg(QFileInfo(path).fileName(),
                           QString::fromUtf8(ac3::io::describe(wav.error()).data(),
                                             static_cast<qsizetype>(
                                                 ac3::io::describe(wav.error()).size()))));
        return;
    }

    const auto channels = wav->channels.size();
    const auto rate = wav->sample_rate;
    const double seconds =
        rate > 0 ? static_cast<double>(wav->frame_count()) / static_cast<double>(rate) : 0.0;
    const auto layout = ac3::io::ac3_layout_for(channels);

    QString problem;
    if (!layout) {
        problem = QStringLiteral("AC-3 has no coding mode for %1 channels (1 to 6 are "
                                 "supported)")
                      .arg(channels);
    } else if (!to_sample_rate(rate)) {
        problem = QStringLiteral("sample rate %1 Hz is not legal for AC-3 (need 32, 44.1 or 48 kHz)")
                      .arg(rate);
    }

    if (!layout) {
        clearLayout();
    } else {
        setLayout(layout->acmod, layout->lfe);
        setMetering(false);
        // What the file actually holds, shown before a single frame is
        // encoded: the meters answer "what is in here?" as well as "what is
        // going out?".
        ac3::analysis::LevelMeter meter{layout->acmod, layout->lfe, rate};
        std::vector<std::span<const float>> views(layout->wav_index.size());
        for (std::size_t ch = 0; ch < views.size(); ++ch) {
            views[ch] = wav->channels[layout->wav_index[ch]];
        }
        meter.process(views);
        publishSummary(meter);
    }

    source_info_ = QStringLiteral("%1 Hz · %2 · %3:%4")
                       .arg(rate)
                       .arg(layout ? layout_name_ : QStringLiteral("%1 channels").arg(channels))
                       .arg(static_cast<int>(seconds) / 60)
                       .arg(static_cast<int>(seconds) % 60, 2, 10, QLatin1Char('0'));
    source_ = std::make_unique<Source>(
        Source{std::move(*wav), layout ? *layout : ac3::io::Ac3Layout{}});
    source_path_ = path;
    source_ready_ = problem.isEmpty();
    emit sourceChanged();

    setStatus(source_ready_ ? QStringLiteral("Ready to encode %1.").arg(QFileInfo(path).fileName())
                            : QStringLiteral("Cannot encode %1: %2")
                                  .arg(QFileInfo(path).fileName(), problem));
}

QString EncoderController::suggestedOutputName() const {
    if (source_path_.isEmpty()) {
        return QStringLiteral("output.ac3");
    }
    return QFileInfo(source_path_).completeBaseName() + QStringLiteral(".ac3");
}

void EncoderController::cancel() {
    cancel_requested_.store(true, std::memory_order_relaxed);
}

void EncoderController::encodeTo(const QUrl& url) {
    if (busy_ || !source_ready_ || !source_) {
        return;
    }
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    output_path_ = path;
    emit outputChanged();

    cancel_requested_.store(false, std::memory_order_relaxed);
    setBusy(true);
    setProgress(0.0);
    setStatus(QStringLiteral("Encoding…"));

    const auto layout = source_->layout;
    const auto sample_rate = source_->wav.sample_rate;
    const auto rate = *to_sample_rate(sample_rate);
    const int bitrate = bitrate_kbps_;
    // The WAV payload is copied into the worker, already permuted into A/52
    // channel order, so the GUI thread stays free to swap the loaded source
    // while an encode runs.
    std::vector<std::vector<float>> planes;
    planes.reserve(layout.wav_index.size());
    for (const auto wav_channel : layout.wav_index) {
        planes.push_back(source_->wav.channels[wav_channel]);
    }

    setLayout(layout.acmod, layout.lfe);
    setMetering(true);

    std::ignore = QtConcurrent::run([this, path, rate, bitrate, layout, sample_rate,
                                     planes = std::move(planes)]() mutable {
        ac3::FrameEncoder encoder{{.sample_rate = rate,
                                   .bitrate_kbps = static_cast<std::uint32_t>(bitrate),
                                   .acmod = layout.acmod,
                                   .lfe = layout.lfe}};
        ac3::analysis::LevelMeter meter{layout.acmod, layout.lfe, sample_rate};
        std::ofstream out{path.toStdString(), std::ios::binary};
        if (!out) {
            QMetaObject::invokeMethod(this, [this] {
                setBusy(false);
                setMetering(false);
                setStatus(QStringLiteral("Could not open the output file for writing."));
                emit encodeFinished(false, status());
            });
            return;
        }

        const std::size_t nchans = planes.size();
        const std::size_t total = planes.front().size();
        std::vector<std::vector<float>> block(nchans,
                                              std::vector<float>(ac3::kSamplesPerFrame));
        std::vector<std::span<const float>> views(nchans);
        std::vector<std::span<const float>> metered(nchans);
        std::size_t frames = 0;
        std::uint64_t bytes = 0;
        bool cancelled = false;
        auto published_at = std::chrono::steady_clock::now() - kPublishInterval;

        for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
            if (cancel_requested_.load(std::memory_order_relaxed)) {
                cancelled = true;
                break;
            }
            // The tail frame is zero-padded to a full 1536 samples; the meter
            // sees only the real ones, so padding cannot pull the RMS down.
            const auto valid = std::min<std::size_t>(ac3::kSamplesPerFrame, total - start);
            for (std::size_t ch = 0; ch < nchans; ++ch) {
                for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                    const std::size_t at = start + static_cast<std::size_t>(i);
                    block[ch][static_cast<std::size_t>(i)] = at < total ? planes[ch][at] : 0.0f;
                }
                views[ch] = block[ch];
                metered[ch] = std::span{block[ch]}.first(valid);
            }
            meter.process(metered);

            const auto frame = encoder.encode_frame(views);
            if (!frame) {
                QMetaObject::invokeMethod(this, [this] {
                    setBusy(false);
                    setMetering(false);
                    setStatus(QStringLiteral("Encoder rejected the settings (illegal bitrate)."));
                    emit encodeFinished(false, status());
                });
                return;
            }
            out.write(reinterpret_cast<const char*>(frame->data()),
                      static_cast<std::streamsize>(frame->size()));
            bytes += frame->size();
            ++frames;

            const double done = static_cast<double>(start + ac3::kSamplesPerFrame) /
                                static_cast<double>(total);
            const auto now = std::chrono::steady_clock::now();
            if (now - published_at >= kPublishInterval) {
                published_at = now;
                std::vector<ac3::analysis::ChannelLevel> snapshot(meter.levels().begin(),
                                                                  meter.levels().end());
                QMetaObject::invokeMethod(
                    this, [this, done, snapshot = std::move(snapshot)] {
                        setProgress(std::min(done, 1.0));
                        publishLevels(snapshot);
                    });
            } else {
                QMetaObject::invokeMethod(this,
                                          [this, done] { setProgress(std::min(done, 1.0)); });
            }
        }
        out.close();

        std::vector<ac3::analysis::ChannelLevel> totals(
            static_cast<std::size_t>(meter.channel_count()));
        for (std::size_t ch = 0; ch < totals.size(); ++ch) {
            const auto& stats = meter.summary()[ch];
            totals[ch].peak_db = stats.peak_db();
            totals[ch].hold_db = stats.peak_db();
            totals[ch].rms_db = stats.rms_db();
            totals[ch].clipped = stats.clipped_samples > 0;
        }

        QMetaObject::invokeMethod(this, [this, frames, bytes, cancelled,
                                         totals = std::move(totals)] {
            setBusy(false);
            setMetering(false);
            setProgress(cancelled ? 0.0 : 1.0);
            publishLevels(totals);
            if (cancelled) {
                setStatus(QStringLiteral("Encode cancelled."));
            } else {
                setStatus(QStringLiteral("Wrote %1 frames (%2 KB) to %3")
                              .arg(frames)
                              .arg(bytes / 1024)
                              .arg(QFileInfo(output_path_).fileName()));
            }
            emit encodeFinished(!cancelled, status());
        });
    });
}
