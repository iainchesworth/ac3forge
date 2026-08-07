#include "encoder_controller.hpp"

#include <QFileInfo>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <fstream>
#include <optional>
#include <span>
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

bool EncoderController::captureSupported() const {
    // Live capture arrives with the WASAPI backend; the UI stays honest about
    // that rather than offering a control that cannot work yet.
    return false;
}

void EncoderController::refreshCaptureDevices() {
    QStringList devices;
    if (captureSupported()) {
        // Populated by the capture backend once it lands.
    }
    if (devices != capture_devices_) {
        capture_devices_ = devices;
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
