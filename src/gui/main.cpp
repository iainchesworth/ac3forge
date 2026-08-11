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
//   ac3gui --smoke        <in.wav> <out.ac3>                [shot.png] [prop=value ...]
//   ac3gui --smoke-record <deviceIndex> <seconds> <out.ac3> [shot.png] [prop=value ...]
//   ac3gui --smoke-live   <deviceIndex> <seconds> <out.ac3> [shot.png] [prop=value ...]
//   ac3gui --smoke-shot   <shot.png>              [in.wav]  [prop=value ...]
//
// The trailing prop=value tokens are set through Qt's property system, which
// is the same path a QML binding writes through - so a smoke run exercises the
// real controls rather than a parallel API kept alongside them. Anything
// declared Q_PROPERTY works: codecIndex=1, layoutIndex=5, coupling=true,
// drcIndex=2, atmosEnabled=true, containerIndex=1, and (on the window itself)
// tier=expert; preset=7.1.4 invokes applyChannelPreset() instead - see
// apply_properties' own comment.
//
// Both --smoke and --smoke-record fail unless every channel the routing FEEDS
// has its needle leave the floor, so --smoke-record against a silent endpoint
// fails by design: it is the check that would otherwise pass on a meter wired
// to nothing. A channel the source cannot fill is excluded, because reading
// -inf is the correct answer there and demanding otherwise would only reward
// an encoder that invented a signal.
//
// --smoke-shot is different in kind, not just in what it drives: it proves
// nothing about audio reaching anything (there is no encode, no meter trace
// to demand) - it exists purely to put the window into a specific state and
// grab it, the documentation-screenshot use case where a completed run in
// the strip would be noise the other three modes' own callers actually want.

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
// and every channel the routing feeds had its needle leave the floor.
bool report_meters(EncoderController* controller, const MeterTrace& trace, int drawn,
                   int min_publishes) {
    const auto names = controller->channelNames();
    const auto levels = controller->channelLevels();
    const double floor_db = controller->meterFloorDb();
    std::println("smoke: {} level publishes while live", trace.publishes);
    std::println("smoke: {:<10} {:>6} {:>10} {:>10} {:>10} {:>10}", "ch", "fed", "live min",
                 "live max", "peak", "rms");

    bool every_fed_channel_moved = true;
    for (qsizetype ch = 0; ch < names.size(); ++ch) {
        const auto at = static_cast<std::size_t>(ch);
        const bool traced = at < trace.highest.size();
        const double low = traced ? trace.lowest[at] : floor_db;
        const double high = traced ? trace.highest[at] : floor_db;
        const auto entry = levels.value(ch).toMap();
        // A channel the routing puts nothing into reads -inf correctly: the
        // source has nothing that belongs there. Requiring it to move would
        // only be satisfied by inventing a signal.
        const bool fed = entry.value(QStringLiteral("fed"), true).toBool();
        std::println("smoke: {:<10} {:>6} {:>10.2f} {:>10.2f} {:>10.2f} {:>10.2f}",
                     names[ch].toStdString(), fed ? "yes" : "no", low, high,
                     entry.value(QStringLiteral("peakDb")).toDouble(),
                     entry.value(QStringLiteral("rmsDb")).toDouble());
        if (fed) {
            every_fed_channel_moved = every_fed_channel_moved && high > floor_db;
        }
    }

    const bool passed =
        drawn == names.size() && every_fed_channel_moved && trace.publishes >= min_publishes;
    if (!passed) {
        std::println(stderr,
                     "smoke: FAILED ({} meters for {} channels, {} publishes of at least {}, "
                     "all fed channels moved {})",
                     drawn, names.size(), trace.publishes, min_publishes,
                     every_fed_channel_moved);
    }
    return passed;
}

// Applies the trailing prop=value tokens through Qt's property system - the
// same path a QML binding writes through, so this drives the real controls
// rather than a parallel API kept beside them. A name that is not a property,
// or a value the property will not take, is a failure rather than a shrug:
// silently ignoring it would have the run report on settings it never used.
//
// Tried against `controller` (an EncoderController Q_PROPERTY - codecIndex,
// atmosEnabled, bedIndex, vbrEnabled, ...) first, then against `root` (the
// QML window itself) for the handful of properties that deliberately live
// there instead - tier (Guided/Advanced/Expert) chief among them, kept off
// EncoderController because nothing outside Main.qml reads it (see its own
// comment there). One dispatch loop rather than two prop=value vocabularies
// a caller would have to know apart.
//
// Two names are not properties at all: "preset" invokes applyChannelPreset()
// (Q_INVOKABLE, not a Q_PROPERTY - it sets bed, LFE and every extra
// together, which no single property does) and "src2" invokes
// addSourceFile() (loading a second source is what actually populates the
// multi-source assignment table - there is no property that does it either)
// - the two method calls this otherwise property-only mechanism carries a
// special case for.
bool apply_properties(EncoderController* controller, QObject* root, const QStringList& tokens) {
    for (const auto& token : tokens) {
        const auto eq = token.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            std::println(stderr, "smoke: '{}' is not prop=value", token.toStdString());
            return false;
        }
        const QString name = token.left(eq);
        const QString text = token.mid(eq + 1);
        if (name == QLatin1String("preset")) {
            controller->applyChannelPreset(text);
            std::println("smoke: preset = {} -> {}", text.toStdString(),
                         controller->channelShapeName().toStdString());
            continue;
        }
        if (name == QLatin1String("src2")) {
            controller->addSourceFile(QUrl::fromLocalFile(text));
            std::println("smoke: src2 = {} -> {} sources loaded", text.toStdString(),
                         controller->sourceModel().size());
            continue;
        }
        auto* target = static_cast<QObject*>(controller);
        auto index = target->metaObject()->indexOfProperty(name.toUtf8().constData());
        if (index < 0 && root != nullptr) {
            target = root;
            index = target->metaObject()->indexOfProperty(name.toUtf8().constData());
        }
        if (index < 0) {
            std::println(stderr, "smoke: no property named '{}'", name.toStdString());
            return false;
        }
        const auto property = target->metaObject()->property(index);
        QVariant value{text};
        if (!value.convert(property.metaType()) ||
            !property.write(target, value)) {
            std::println(stderr, "smoke: could not set {} to '{}'", name.toStdString(),
                         text.toStdString());
            return false;
        }
        std::println("smoke: {} = {}", name.toStdString(),
                     property.read(target).toString().toStdString());
    }
    return true;
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
              const QString& shot_path, const QStringList& properties) {
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
    // After the load, not before: opening a file settles the layout on the one
    // that matches it, which would overwrite anything set here.
    if (!apply_properties(controller, engine.rootObjects().first(), properties)) {
        return 1;
    }
    std::println("smoke: {}", controller->routingSummary().toStdString());
    std::println("smoke: writing .{}", controller->outputSuffix().toStdString());

    const auto trace = watch_meters(controller);

    QObject::connect(controller, &EncoderController::encodeFinished, controller,
                     [&engine, controller, shot_path, trace](bool ok, const QString& message) {
                         std::println("smoke: {}", message.toStdString());
                         if (!shot_path.isEmpty()) {
                             std::println("smoke: window grab -> {}",
                                          save_window(engine, shot_path)
                                              ? shot_path.toStdString()
                                              : std::string{"FAILED"});
                         }
                         // Counted here rather than before the encode: the
                         // meters follow the CODED channels, and which those
                         // are is only settled once the plan has been applied.
                         const int drawn = meters_drawn(engine);
                         std::println("smoke: layout {} · QML instantiated {} channel meters",
                                      controller->layoutName().toStdString(), drawn);
                         // A short file encodes inside a single 30 Hz publish
                         // window, so one update is all this mode can demand.
                         // The table prints either way — a failed encode is
                         // easier to diagnose next to what the meters saw.
                         const bool meters_ok = report_meters(controller, *trace, drawn, 1);
                         const bool passed = ok && meters_ok;
                         std::println("smoke: {}", passed ? "OK" : "failed");
                         QCoreApplication::exit(passed ? 0 : 1);
                     });

    // Posted rather than called straight away: encodeTo() can fail
    // synchronously (no source above the loudness gate, a routing the source
    // can't fill) and emit encodeFinished before exec() ever starts.
    // QCoreApplication::exit() called that early has nothing to exit - no
    // event loop is on the stack yet - so it only sets the "quit now" flag,
    // and exec() unconditionally clears that flag the moment it starts. The
    // process would then run the loop forever with nothing left to end it.
    // Starting the encode from a zero-delay timer instead guarantees exec()
    // is already live by the time encodeTo() - and any exit() it triggers -
    // runs, whether that happens synchronously or on the worker thread.
    QTimer::singleShot(0, controller, [controller, out_path] {
        controller->encodeTo(QUrl::fromLocalFile(out_path));
    });
    return QGuiApplication::exec();
}

// Record from a capture endpoint and watch the meters follow live audio. This
// is the path a file encode cannot stand in for: a different worker, a layout
// chosen from the device rather than from a header, and levels that arrive in
// real time instead of as fast as the encoder can run.
int run_smoke_record(QQmlApplicationEngine& engine, int device, double seconds,
                     const QString& out_path, const QString& shot_path,
                     const QStringList& properties) {
    auto* controller = smoke_controller(engine);
    if (controller == nullptr) {
        return 1;
    }
    if (!apply_properties(controller, engine.rootObjects().first(), properties)) {
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

// A live session, watched the same way run_smoke_record watches a plain
// recording, plus the one thing a recording never has: frames reaching a
// sink (here, the file writer alone - monitor and passthrough both need a
// real render endpoint this harness cannot assume exists) while the capture
// is still running rather than only once it stops.
int run_smoke_live(QQmlApplicationEngine& engine, int device, double seconds,
                   const QString& out_path, const QString& shot_path,
                   const QStringList& properties) {
    auto* controller = smoke_controller(engine);
    if (controller == nullptr) {
        return 1;
    }
    if (!apply_properties(controller, engine.rootObjects().first(), properties)) {
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
    std::println("smoke: live session {:.1f} s from {}", seconds, devices[device].toStdString());

    const auto trace = watch_meters(controller);

    controller->startLiveSession(device, false, -1, true, QUrl::fromLocalFile(out_path));
    if (!controller->liveActive()) {
        std::println(stderr, "smoke: live session did not start: {}",
                     controller->status().toStdString());
        return 1;
    }
    std::println("smoke: layout {} ({} channels)", controller->layoutName().toStdString(),
                 controller->channelNames().size());
    const int drawn = meters_drawn(engine);
    if (drawn < 0) {
        std::println(stderr, "smoke: the channelMeters repeater is not in the scene");
        controller->stopLiveSession();
        return 1;
    }
    std::println("smoke: QML instantiated {} channel meters", drawn);

    const auto millis = static_cast<int>(seconds * 1000.0);
    if (!shot_path.isEmpty()) {
        QTimer::singleShot(millis * 7 / 10, controller, [&engine, shot_path] {
            std::println("smoke: window grab -> {}", save_window(engine, shot_path)
                                                         ? shot_path.toStdString()
                                                         : std::string{"FAILED"});
        });
    }
    QTimer::singleShot(millis, controller, [controller] { controller->stopLiveSession(); });
    QTimer::singleShot(millis + 15000, controller, [] {
        std::println(stderr, "smoke: FAILED (live session never finished)");
        QCoreApplication::exit(1);
    });

    QObject::connect(
        controller, &EncoderController::encodeFinished, controller,
        [controller, drawn, seconds, trace](bool ok, const QString& message) {
            std::println("smoke: {}", message.toStdString());
            const auto expected = static_cast<int>(seconds * 1000.0 / 32.0) / 2;
            const bool meters_ok = report_meters(controller, *trace, drawn, expected);
            // The one thing a plain recording cannot check: that frames were
            // actually counted as they were produced, not just at the end.
            const bool frames_ok = controller->liveFramesEncoded() > 0;
            std::println("smoke: {} frames encoded live, {} dropped", controller->liveFramesEncoded(),
                         controller->liveFramesDropped());
            const bool passed = ok && meters_ok && frames_ok;
            std::println("smoke: {}", passed ? "OK" : "failed");
            QCoreApplication::exit(passed ? 0 : 1);
        });

    return QGuiApplication::exec();
}

// Loads a source (if given) and applies properties, then grabs a single
// window screenshot without encoding anything - see this file's own top
// comment on why that is a separate mode rather than an option on --smoke.
int run_smoke_shot(QQmlApplicationEngine& engine, const QString& shot_path,
                   const QString& in_path, const QStringList& properties) {
    auto* controller = smoke_controller(engine);
    if (controller == nullptr) {
        return 1;
    }
    if (!in_path.isEmpty()) {
        controller->loadSourceFile(QUrl::fromLocalFile(in_path));
        if (!controller->sourceReady()) {
            std::println(stderr, "smoke: source not usable: {}",
                         controller->status().toStdString());
            return 1;
        }
    }
    if (!apply_properties(controller, engine.rootObjects().first(), properties)) {
        return 1;
    }
    // One event-loop lap so the layout genuinely settles (window resize, tab
    // reflow) before the grab - properties above are applied synchronously,
    // but Qt Quick Layouts can still owe a polish pass, the same reason
    // tst_guided_wizard.qml's own waitForWizardLayout() waits after creating
    // a fresh window rather than grabbing on the same tick.
    QTimer::singleShot(50, controller, [&engine, shot_path] {
        const bool ok = save_window(engine, shot_path);
        std::println("smoke: window grab -> {}",
                     ok ? shot_path.toStdString() : std::string{"FAILED"});
        QCoreApplication::exit(ok ? 0 : 1);
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
    // Everything after the fixed arguments is prop=value; a screenshot path is
    // the one optional positional, told apart by not containing an '='.
    const auto trailing = [&args](qsizetype from, QString& shot) {
        QStringList properties;
        for (qsizetype i = from; i < args.size(); ++i) {
            if (properties.isEmpty() && shot.isEmpty() && !args[i].contains(QLatin1Char('='))) {
                shot = args[i];
            } else {
                properties.append(args[i]);
            }
        }
        return properties;
    };

    if (args.size() >= 4 && args[1] == QLatin1String("--smoke")) {
        QString shot;
        const auto properties = trailing(4, shot);
        return run_smoke(engine, args[2], args[3], shot, properties);
    }
    if (args.size() >= 5 && args[1] == QLatin1String("--smoke-record")) {
        QString shot;
        const auto properties = trailing(5, shot);
        return run_smoke_record(engine, args[2].toInt(), args[3].toDouble(), args[4], shot,
                                properties);
    }
    if (args.size() >= 5 && args[1] == QLatin1String("--smoke-live")) {
        QString shot;
        const auto properties = trailing(5, shot);
        return run_smoke_live(engine, args[2].toInt(), args[3].toDouble(), args[4], shot,
                              properties);
    }
    if (args.size() >= 3 && args[1] == QLatin1String("--smoke-shot")) {
        // shot.png is the one fixed positional here (unlike the other three
        // modes, where it is itself the trailing optional one) - trailing()
        // still tells an input WAV apart from prop=value the same way, just
        // starting one argument later.
        QString in_path;
        const auto properties = trailing(3, in_path);
        return run_smoke_shot(engine, args[2], in_path, properties);
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
