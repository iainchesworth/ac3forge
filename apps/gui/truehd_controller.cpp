#include "truehd_controller.hpp"

#include <QFileInfo>
#include <QLocale>
#include <QMetaObject>
#include <QtConcurrent/QtConcurrentRun>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include "ac3/mlp/atmos.hpp"
#include "ac3/mlp/mlp_tables.hpp"
#include "ac3/mlp/stream.hpp"
#include "ac3/oba/oamd.hpp"

namespace {

[[nodiscard]] std::optional<ac3::mlp::SampleRate> mlp_sample_rate(std::uint32_t hz) {
    switch (hz) {
        case 48000: return ac3::mlp::SampleRate::k48000;
        case 96000: return ac3::mlp::SampleRate::k96000;
        case 192000: return ac3::mlp::SampleRate::k192000;
        case 44100: return ac3::mlp::SampleRate::k44100;
        case 88200: return ac3::mlp::SampleRate::k88200;
        case 176400: return ac3::mlp::SampleRate::k176400;
        default: return std::nullopt;
    }
}

struct EncodeOutcome {
    QString error;
    QString info;
};

// The whole worker-side job: pad, two-pass encode (plain or objects mode),
// write, then decode the written bytes back and diff - the same shape as the
// CLI's truehd-encode/truehd-atmos, minus the console.
[[nodiscard]] EncodeOutcome encode_file(const ac3::io::WavPcmData& wav, const QString& out_path,
                                        bool objects_mode) {
    EncodeOutcome outcome;
    const auto rate = mlp_sample_rate(wav.sample_rate);
    if (!rate) {
        outcome.error = QStringLiteral("%1 Hz is not an MLP sample rate").arg(wav.sample_rate);
        return outcome;
    }
    const auto channel_count = wav.channels.size();
    const auto frame = static_cast<std::size_t>(ac3::mlp::samples_per_access_unit(*rate));
    const std::size_t source_frames = wav.frame_count();
    const std::size_t padded = (source_frames + frame - 1) / frame * frame;
    std::vector<std::vector<std::int32_t>> channels = wav.channels;
    for (auto& channel : channels) {
        channel.resize(padded, 0);
    }

    // Objects mode: the CLI's fan-out default - objects spread evenly
    // around the room at ear height, stationary (the dialog has no motion
    // authoring; the CLI's paths.txt route covers that workflow).
    std::vector<ac3::oba::DynamicObject> objects(objects_mode ? channel_count : 0);
    for (std::size_t i = 0; i < objects.size(); ++i) {
        const double radians =
            2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(channel_count);
        objects[i].position = {.x = 0.5 - 0.5 * std::sin(radians),
                               .y = 0.5 - 0.5 * std::cos(radians),
                               .z = 0.0};
    }

    // The library's two-pass peak_data_rate treatment, over either encoder
    // shape. `emit_all` returns the total plus every finished unit when a
    // sink vector is supplied.
    const auto encode_all = [&](auto& encoder, std::vector<std::vector<std::byte>>* units) {
        std::size_t written = 0;
        for (std::size_t at = 0; at < padded; at += frame) {
            std::vector<std::span<const std::int32_t>> spans;
            spans.reserve(channel_count);
            for (const auto& channel : channels) {
                spans.emplace_back(std::span<const std::int32_t>{channel}.subspan(at, frame));
            }
            const std::span<const std::span<const std::int32_t>> view{spans};
            const bool last = at + frame >= padded;
            const ac3::mlp::EndOfStream end{static_cast<int>(padded - source_frames)};
            std::vector<std::byte> unit;
            if constexpr (std::is_same_v<std::decay_t<decltype(encoder)>,
                                         ac3::mlp::AtmosEncoder>) {
                unit = last ? encoder.encode_access_unit(view, objects, end)
                            : encoder.encode_access_unit(view, objects);
            } else {
                unit = last ? encoder.encode_access_unit(view, end)
                            : encoder.encode_access_unit(view);
            }
            written += unit.size();
            if (units != nullptr) {
                units->push_back(std::move(unit));
            }
        }
        return written;
    };

    std::vector<std::vector<std::byte>> units;
    std::uint32_t peak_16ths = 0;
    if (objects_mode) {
        ac3::mlp::AtmosConfig config;
        config.sample_rate = *rate;
        config.wordlength = wav.bits;
        config.bed = 0;
        config.dynamic_objects = static_cast<int>(channel_count);
        ac3::mlp::AtmosEncoder measuring(config);
        (void)encode_all(measuring, nullptr);
        config.peak_data_rate_16ths = peak_16ths = measuring.measured_peak_data_rate_16ths();
        ac3::mlp::AtmosEncoder encoder(config);
        (void)encode_all(encoder, &units);
    } else {
        ac3::mlp::StreamConfig config;
        config.sample_rate = *rate;
        config.wordlength = wav.bits;
        config.channels = static_cast<int>(channel_count);
        config.automatic = true;
        ac3::mlp::StreamEncoder measuring(config);
        (void)encode_all(measuring, nullptr);
        config.peak_data_rate_16ths = peak_16ths = measuring.measured_peak_data_rate_16ths();
        ac3::mlp::StreamEncoder encoder(config);
        (void)encode_all(encoder, &units);
    }

    std::ofstream out{out_path.toStdString(), std::ios::binary};
    if (!out) {
        outcome.error = QStringLiteral("cannot open %1 for writing").arg(out_path);
        return outcome;
    }
    std::size_t written = 0;
    for (const auto& unit : units) {
        out.write(reinterpret_cast<const char*>(unit.data()),
                  static_cast<std::streamsize>(unit.size()));
        written += unit.size();
    }
    out.close();
    if (!out) {
        outcome.error = QStringLiteral("writing %1 failed").arg(out_path);
        return outcome;
    }

    // The lossless proof: decode what was just encoded and diff it against
    // the source, trailing fill trimmed via the terminator's own count.
    ac3::mlp::StreamDecoder decoder;
    std::vector<std::vector<std::int32_t>> decoded(channel_count);
    for (const auto& unit : units) {
        std::vector<std::vector<std::int32_t>> frame_channels;
        if (!decoder.decode_access_unit(unit, frame_channels) ||
            frame_channels.size() != channel_count) {
            outcome.error = QStringLiteral("verification decode failed");
            return outcome;
        }
        for (std::size_t ch = 0; ch < channel_count; ++ch) {
            decoded[ch].insert(decoded[ch].end(), frame_channels[ch].begin(),
                               frame_channels[ch].end());
        }
    }
    const auto trim = static_cast<std::size_t>(
        decoder.end_of_stream() ? decoder.zero_samples_appended() : 0);
    for (auto& channel : decoded) {
        if (trim <= channel.size()) {
            channel.resize(channel.size() - trim);
        }
    }
    if (decoded != wav.channels) {
        outcome.error = QStringLiteral("verification failed: decode is not bit-exact");
        return outcome;
    }

    const std::size_t pcm_bytes =
        padded * channel_count * static_cast<std::size_t>(wav.bits) / 8;
    const QLocale locale;
    outcome.info =
        QStringLiteral("%1 access units · %2 bytes · %3% of PCM · peak %4 kbit/s · "
                       "verified bit-exact%5")
            .arg(locale.toString(static_cast<qulonglong>(padded / frame)),
                 locale.toString(static_cast<qulonglong>(written)),
                 QString::number(100.0 * static_cast<double>(written) /
                                     static_cast<double>(pcm_bytes),
                                 'f', 1),
                 locale.toString(static_cast<qulonglong>(std::llround(
                     static_cast<double>(peak_16ths) * wav.sample_rate / 16000.0))),
                 objects_mode
                     ? QStringLiteral(" · %1 objects").arg(channel_count)
                     : QString());
    return outcome;
}

}  // namespace

TruehdController::TruehdController(QObject* parent) : QObject(parent) {}

TruehdController::~TruehdController() = default;

QString TruehdController::sourceInfo() const {
    if (!source_) {
        return {};
    }
    const QLocale locale;
    return QStringLiteral("%1 ch · %2 Hz · %3-bit · %4 samples")
        .arg(QString::number(source_->channels.size()),
             locale.toString(source_->sample_rate), QString::number(source_->bits),
             locale.toString(static_cast<qulonglong>(source_->frame_count())));
}

void TruehdController::setError(const QString& message) {
    if (error_ == message) {
        return;
    }
    error_ = message;
    emit errorChanged();
}

void TruehdController::setObjectsMode(bool value) {
    if (objects_mode_ == value || busy_) {
        return;
    }
    objects_mode_ = value;
    emit modeChanged();
}

void TruehdController::setSource(const QUrl& url) {
    if (busy_) {
        return;
    }
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    if (path.isEmpty()) {
        return;
    }
    source_path_ = path;
    source_.reset();
    result_info_.clear();
    setError({});
    busy_ = true;
    emit sourceChanged();
    emit resultChanged();
    emit busyChanged();

    std::ignore = QtConcurrent::run([this, path] {
        auto loaded = ac3::io::read_wav_pcm(path.toStdString());
        QMetaObject::invokeMethod(this, [this, path, loaded = std::move(loaded)]() mutable {
            busy_ = false;
            if (loaded) {
                const auto channels = loaded->channels.size();
                if (channels < 1 || channels > 16 ||
                    !mlp_sample_rate(loaded->sample_rate)) {
                    setError(QStringLiteral(
                        "need 1-16 channels at an MLP rate (48/96/192 or 44.1/88.2/176.4 kHz)"));
                } else {
                    source_ = std::move(*loaded);
                    // Suggest <source>.mlp next to the input.
                    const QFileInfo info(path);
                    output_path_ = info.path() + QStringLiteral("/") +
                                   info.completeBaseName() + QStringLiteral(".mlp");
                    emit outputChanged();
                }
            } else {
                setError(QStringLiteral(
                    "not an integer PCM WAV (need 16- or 24-bit PCM; float sources "
                    "would forfeit bit-exactness)"));
            }
            emit sourceChanged();
            emit busyChanged();
        });
    });
}

void TruehdController::setOutput(const QUrl& url) {
    if (busy_) {
        return;
    }
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    if (path.isEmpty() || path == output_path_) {
        return;
    }
    output_path_ = path;
    emit outputChanged();
}

void TruehdController::encode() {
    if (busy_ || !source_ || output_path_.isEmpty()) {
        return;
    }
    result_info_.clear();
    setError({});
    busy_ = true;
    emit resultChanged();
    emit busyChanged();

    std::ignore = QtConcurrent::run([this, wav = *source_, out = output_path_,
                                     objects = objects_mode_] {
        auto outcome = encode_file(wav, out, objects);
        QMetaObject::invokeMethod(this, [this, outcome = std::move(outcome)]() mutable {
            busy_ = false;
            if (outcome.error.isEmpty()) {
                result_info_ = outcome.info;
            } else {
                setError(outcome.error);
            }
            emit resultChanged();
            emit busyChanged();
        });
    });
}
