#include "encoder_controller.hpp"

#include <QFileInfo>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "ac3/encoder/encoder.hpp"
#include "ac3/io/wav.hpp"

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

}  // namespace

struct EncoderController::Source {
    ac3::io::WavData wav;
};

EncoderController::EncoderController(QObject* parent) : QObject(parent) {
    refreshCaptureDevices();
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
    capture_level_ = 0.0;
    emit recordedSecondsChanged();
    setStatus(QStringLiteral("Recording from %1…").arg(QString::fromStdString(device.name)));

    const int bitrate = bitrate_kbps_;
    const auto channels = capture_->channels();
    const auto sample_rate = capture_->sample_rate();

    std::ignore = QtConcurrent::run([this, path, rate = *rate, bitrate, channels,
                                     sample_rate]() {
        ac3::FrameEncoder encoder{
            {.sample_rate = rate, .bitrate_kbps = static_cast<std::uint32_t>(bitrate)}};
        std::ofstream out{path.toStdString(), std::ios::binary};
        if (!out) {
            QMetaObject::invokeMethod(this, [this] {
                capture_->stop();
                capture_.reset();
                setRecording(false);
                setBusy(false);
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

            float peak = 0.0f;
            for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                const std::size_t base = static_cast<std::size_t>(i) * channels;
                const float left = interleaved[base];
                const float right = channels > 1 ? interleaved[base + 1] : left;
                planar[0][static_cast<std::size_t>(i)] = left;
                planar[1][static_cast<std::size_t>(i)] = right;
                peak = std::max({peak, std::abs(left), std::abs(right)});
            }

            const auto frame = encoder.encode_frame(views);
            if (!frame) {
                break;
            }
            out.write(reinterpret_cast<const char*>(frame->data()),
                      static_cast<std::streamsize>(frame->size()));
            ++frames;

            const double seconds = static_cast<double>(frames * ac3::kSamplesPerFrame) /
                                   static_cast<double>(sample_rate);
            QMetaObject::invokeMethod(this, [this, seconds, peak] {
                recorded_seconds_ = seconds;
                capture_level_ = static_cast<double>(peak);
                emit recordedSecondsChanged();
            });
        }
        out.close();

        QMetaObject::invokeMethod(this, [this, frames] {
            const auto stats = capture_->stats();
            capture_->stop();
            capture_.reset();
            setRecording(false);
            setBusy(false);
            capture_level_ = 0.0;
            setStatus(QStringLiteral("Recorded %1 frames to %2 (%3 dropped, %4 silence-filled)")
                          .arg(frames)
                          .arg(QFileInfo(output_path_).fileName())
                          .arg(stats.frames_dropped)
                          .arg(stats.frames_silence_filled));
            emit recordedSecondsChanged();
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

    QString problem;
    if (channels != 2) {
        problem = QStringLiteral("needs 2 channels (this file has %1)").arg(channels);
    } else if (!to_sample_rate(rate)) {
        problem = QStringLiteral("sample rate %1 Hz is not legal for AC-3 (need 32, 44.1 or 48 kHz)")
                      .arg(rate);
    }

    source_ = std::make_unique<Source>(Source{std::move(*wav)});
    source_path_ = path;
    source_ready_ = problem.isEmpty();
    source_info_ = QStringLiteral("%1 Hz · %2 channel%3 · %4:%5")
                       .arg(rate)
                       .arg(channels)
                       .arg(channels == 1 ? QString() : QStringLiteral("s"))
                       .arg(static_cast<int>(seconds) / 60)
                       .arg(static_cast<int>(seconds) % 60, 2, 10, QLatin1Char('0'));
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

    const auto rate = *to_sample_rate(source_->wav.sample_rate);
    const int bitrate = bitrate_kbps_;
    // The WAV payload is copied into the worker so the GUI thread stays free
    // to swap the loaded source while an encode runs.
    auto left = source_->wav.channels[0];
    auto right = source_->wav.channels[1];

    std::ignore = QtConcurrent::run([this, path, rate, bitrate, left = std::move(left),
                                     right = std::move(right)]() mutable {
        ac3::FrameEncoder encoder{{.sample_rate = rate,
                                   .bitrate_kbps = static_cast<std::uint32_t>(bitrate)}};
        std::ofstream out{path.toStdString(), std::ios::binary};
        if (!out) {
            QMetaObject::invokeMethod(this, [this] {
                setBusy(false);
                setStatus(QStringLiteral("Could not open the output file for writing."));
                emit encodeFinished(false, status());
            });
            return;
        }

        const std::size_t total = left.size();
        std::vector<std::vector<float>> block(2, std::vector<float>(ac3::kSamplesPerFrame));
        std::vector<std::span<const float>> views(2);
        std::size_t frames = 0;
        std::uint64_t bytes = 0;
        bool cancelled = false;

        for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
            if (cancel_requested_.load(std::memory_order_relaxed)) {
                cancelled = true;
                break;
            }
            for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                const std::size_t at = start + static_cast<std::size_t>(i);
                block[0][static_cast<std::size_t>(i)] = at < total ? left[at] : 0.0f;
                block[1][static_cast<std::size_t>(i)] = at < total ? right[at] : 0.0f;
            }
            views[0] = block[0];
            views[1] = block[1];
            const auto frame = encoder.encode_frame(views);
            if (!frame) {
                QMetaObject::invokeMethod(this, [this] {
                    setBusy(false);
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
            QMetaObject::invokeMethod(this,
                                      [this, done] { setProgress(std::min(done, 1.0)); });
        }
        out.close();

        QMetaObject::invokeMethod(this, [this, frames, bytes, cancelled] {
            setBusy(false);
            setProgress(cancelled ? 0.0 : 1.0);
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
