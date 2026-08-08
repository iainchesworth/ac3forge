#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include <algorithm>
#include <limits>
#include <memory>
#include <print>
#include <vector>

#include "encoder_controller.hpp"

// Headless self-checks, the reason the offscreen platform plugin is deployed
// beside the executable. They drive the real controller and the real QML and
// report what the meters did: a clean build proves the app links, and only
// this proves the display is wired to the audio.
//
//   ac3gui --smoke        <in.wav> <out.ac3>              [shot.png]
//   ac3gui --smoke-record <deviceIndex> <seconds> <out.ac3> [shot.png]
//
// Both fail unless every channel's needle actually left the floor, so
// --smoke-record against a silent endpoint fails by design: it is the check
// that would otherwise pass on a meter wired to nothing.

namespace {

// Qt draws the window into an image itself, so this works under the offscreen
// platform and cannot capture whatever happens to be in front of the app.
bool save_window(QQmlApplicationEngine& engine, const QString& path) {
    if (engine.rootObjects().isEmpty()) {
        return false;
    }
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (window == nullptr) {
        return false;
    }
    const QImage shot = window->grabWindow();
    return !shot.isNull() && shot.save(path);
}

// What the meters did while a run was in flight, read from the properties QML
// binds to. The extremes matter more than any single reading: a needle that
// never leaves the floor is not metering, and one that never moves is a still
// frame.
struct MeterTrace {
    std::vector<double> lowest;
    std::vector<double> highest;
    int publishes = 0;
};

// Counts every publish that happens while a run is live. Driven by the signal
// rather than by a sampling timer, so the count is what the display actually
// received rather than what a poll happened to catch. The trace is shared
// rather than referenced: the connection outlives the call that made it.
std::shared_ptr<MeterTrace> watch_meters(EncoderController* controller) {
    auto trace = std::make_shared<MeterTrace>();
    QObject::connect(controller, &EncoderController::levelsChanged, controller,
                     [controller, trace] {
                         if (!controller->metering()) {
                             return;
                         }
                         const auto levels = controller->channelLevels();
                         const auto count = static_cast<std::size_t>(levels.size());
                         if (trace->lowest.size() != count) {
                             trace->lowest.assign(count,
                                                  std::numeric_limits<double>::infinity());
                             trace->highest.assign(count,
                                                   -std::numeric_limits<double>::infinity());
                         }
                         ++trace->publishes;
                         for (std::size_t ch = 0; ch < count; ++ch) {
                             const double peak = levels[static_cast<qsizetype>(ch)]
                                                     .toMap()
                                                     .value(QStringLiteral("peakDb"))
                                                     .toDouble();
                             trace->lowest[ch] = std::min(trace->lowest[ch], peak);
                             trace->highest[ch] = std::max(trace->highest[ch], peak);
                         }
                     });
    return trace;
}

// The live range beside the figures the display settled on. Passing means the
// QML drew one meter per channel, enough publishes arrived to call it live,
// and every channel's needle left the floor at some point.
bool report_meters(EncoderController* controller, const MeterTrace& trace, int drawn,
                   int min_publishes) {
    const auto names = controller->channelNames();
    const auto levels = controller->channelLevels();
    const double floor_db = controller->meterFloorDb();
    std::println("smoke: {} level publishes while live", trace.publishes);
    std::println("smoke: {:<5} {:>10} {:>10} {:>10} {:>10}", "ch", "live min", "live max",
                 "peak", "rms");

    bool every_channel_moved = true;
    for (qsizetype ch = 0; ch < names.size(); ++ch) {
        const auto at = static_cast<std::size_t>(ch);
        const bool traced = at < trace.highest.size();
        const double low = traced ? trace.lowest[at] : floor_db;
        const double high = traced ? trace.highest[at] : floor_db;
        const auto entry = levels.value(ch).toMap();
        std::println("smoke: {:<5} {:>10.2f} {:>10.2f} {:>10.2f} {:>10.2f}",
                     names[ch].toStdString(), low, high,
                     entry.value(QStringLiteral("peakDb")).toDouble(),
                     entry.value(QStringLiteral("rmsDb")).toDouble());
        every_channel_moved = every_channel_moved && high > floor_db;
    }

    const bool passed =
        drawn == names.size() && every_channel_moved && trace.publishes >= min_publishes;
    if (!passed) {
        std::println(stderr,
                     "smoke: FAILED ({} meters for {} channels, {} publishes of at least {}, "
                     "all channels moved {})",
                     drawn, names.size(), trace.publishes, min_publishes,
                     every_channel_moved);
    }
    return passed;
}

// The Repeater that draws the meters. Its count is the QML side of every
// check here: the properties can be perfect and still reach nothing on screen.
int meters_drawn(QQmlApplicationEngine& engine) {
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }
    auto* meters = engine.rootObjects().first()->findChild<QObject*>("channelMeters");
    return meters == nullptr ? -1 : meters->property("count").toInt();
}

EncoderController* smoke_controller(QQmlApplicationEngine& engine) {
    if (engine.rootObjects().isEmpty()) {
        std::println(stderr, "smoke: no root object; the QML failed to load");
        return nullptr;
    }
    auto* controller =
        engine.singletonInstance<EncoderController*>("Ac3Forge", "EncoderController");
    if (controller == nullptr) {
        std::println(stderr, "smoke: EncoderController singleton is not registered");
    }
    return controller;
}

// Encode a file and watch the meters follow it.
int run_smoke(QQmlApplicationEngine& engine, const QString& in_path, const QString& out_path,
              const QString& shot_path) {
    auto* controller = smoke_controller(engine);
    if (controller == nullptr) {
        return 1;
    }

    controller->loadSourceFile(QUrl::fromLocalFile(in_path));
    if (!controller->sourceReady()) {
        std::println(stderr, "smoke: source not usable: {}",
                     controller->status().toStdString());
        return 1;
    }
    std::println("smoke: layout {} ({} channels)", controller->layoutName().toStdString(),
                 controller->channelNames().size());

    const int drawn = meters_drawn(engine);
    if (drawn < 0) {
        std::println(stderr, "smoke: the channelMeters repeater is not in the scene");
        return 1;
    }
    std::println("smoke: QML instantiated {} channel meters", drawn);

    const auto trace = watch_meters(controller);

    QObject::connect(controller, &EncoderController::encodeFinished, controller,
                     [&engine, controller, drawn, shot_path, trace](bool ok,
                                                                    const QString& message) {
                         std::println("smoke: {}", message.toStdString());
                         if (!shot_path.isEmpty()) {
                             std::println("smoke: window grab -> {}",
                                          save_window(engine, shot_path)
                                              ? shot_path.toStdString()
                                              : std::string{"FAILED"});
                         }
                         // A short file encodes inside a single 30 Hz publish
                         // window, so one update is all this mode can demand.
                         // The table prints either way — a failed encode is
                         // easier to diagnose next to what the meters saw.
                         const bool meters_ok = report_meters(controller, *trace, drawn, 1);
                         const bool passed = ok && meters_ok;
                         std::println("smoke: {}", passed ? "OK" : "failed");
                         QCoreApplication::exit(passed ? 0 : 1);
                     });

    controller->encodeTo(QUrl::fromLocalFile(out_path));
    return QGuiApplication::exec();
}

// Record from a capture endpoint and watch the meters follow live audio. This
// is the path a file encode cannot stand in for: a different worker, a layout
// chosen from the device rather than from a header, and levels that arrive in
// real time instead of as fast as the encoder can run.
int run_smoke_record(QQmlApplicationEngine& engine, int device, double seconds,
                     const QString& out_path, const QString& shot_path) {
    auto* controller = smoke_controller(engine);
    if (controller == nullptr) {
        return 1;
    }
    const auto devices = controller->captureDevices();
    if (device < 0 || device >= devices.size()) {
        std::println(stderr, "smoke: capture device {} out of range ({} available)", device,
                     devices.size());
        for (qsizetype i = 0; i < devices.size(); ++i) {
            std::println(stderr, "smoke:   {} {}", i, devices[i].toStdString());
        }
        return 1;
    }
    std::println("smoke: recording {:.1f} s from {}", seconds,
                 devices[device].toStdString());

    const auto trace = watch_meters(controller);

    controller->startRecording(device, QUrl::fromLocalFile(out_path));
    if (!controller->recording()) {
        std::println(stderr, "smoke: recording did not start: {}",
                     controller->status().toStdString());
        return 1;
    }
    std::println("smoke: layout {} ({} channels)", controller->layoutName().toStdString(),
                 controller->channelNames().size());
    const int drawn = meters_drawn(engine);
    if (drawn < 0) {
        std::println(stderr, "smoke: the channelMeters repeater is not in the scene");
        controller->stopRecording();
        return 1;
    }
    std::println("smoke: QML instantiated {} channel meters", drawn);

    const auto millis = static_cast<int>(seconds * 1000.0);
    if (!shot_path.isEmpty()) {
        // Grabbed mid-run, on purpose: a still taken afterwards would show the
        // totals the display settles on, not the live state being checked.
        QTimer::singleShot(millis * 7 / 10, controller, [&engine, shot_path] {
            std::println("smoke: window grab -> {}", save_window(engine, shot_path)
                                                         ? shot_path.toStdString()
                                                         : std::string{"FAILED"});
        });
    }
    QTimer::singleShot(millis, controller, [controller] { controller->stopRecording(); });
    // The capture thread can only stall on something outside this process, so
    // the run must not be able to hang a script forever.
    QTimer::singleShot(millis + 15000, controller, [] {
        std::println(stderr, "smoke: FAILED (recording never finished)");
        QCoreApplication::exit(1);
    });

    QObject::connect(controller, &EncoderController::encodeFinished, controller,
                     [controller, drawn, seconds, trace](bool ok, const QString& message) {
                         std::println("smoke: {}", message.toStdString());
                         // One AC-3 frame is 32 ms and the recorder publishes
                         // per frame, so a run this long owes roughly this
                         // many updates. Half of that is a generous floor for
                         // a machine under load.
                         const auto expected =
                             static_cast<int>(seconds * 1000.0 / 32.0) / 2;
                         const bool meters_ok =
                             report_meters(controller, *trace, drawn, expected);
                         const bool passed = ok && meters_ok;
                         std::println("smoke: {}", passed ? "OK" : "failed");
                         QCoreApplication::exit(passed ? 0 : 1);
                     });

    return QGuiApplication::exec();
}

}  // namespace

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("ac3forge"));
    QGuiApplication::setOrganizationName(QStringLiteral("ac3forge"));

    // Fusion renders identically on every platform, so the layout we design
    // here is the layout everywhere; the native Windows style restyles
    // controls at runtime and would reflow it.
    QQuickStyle::setStyle(QStringLiteral("Fusion"));

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
    engine.loadFromModule("Ac3Forge", "Main");

    const auto args = QGuiApplication::arguments();
    if (args.size() >= 4 && args.size() <= 5 && args[1] == QLatin1String("--smoke")) {
        return run_smoke(engine, args[2], args[3], args.size() == 5 ? args[4] : QString());
    }
    if (args.size() >= 5 && args.size() <= 6 && args[1] == QLatin1String("--smoke-record")) {
        return run_smoke_record(engine, args[2].toInt(), args[3].toDouble(), args[4],
                                args.size() == 6 ? args[5] : QString());
    }
    if (args.size() == 2 && !args[1].startsWith(QLatin1Char('-'))) {
        // Opening the app on a file is the same gesture as dropping one on
        // it, and it is what makes the meters reachable from a script.
        if (auto* controller =
                engine.singletonInstance<EncoderController*>("Ac3Forge", "EncoderController")) {
            controller->loadSourceFile(QUrl::fromLocalFile(args[1]));
        }
    }

    return app.exec();
}
