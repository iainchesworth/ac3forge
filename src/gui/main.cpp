#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include <algorithm>
#include <iterator>
#include <print>
#include <vector>

#include "encoder_controller.hpp"

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

// Headless self-check, the reason the offscreen platform plugin is deployed
// beside the executable: encode a file through the real controller and the
// real QML, and report what the meters did. A clean build proves the app
// links; only this proves the display is wired to the audio.
//
//   ac3gui --smoke <in.wav> <out.ac3> [shot.png]
//
// Exits non-zero unless the QML instantiated one meter per channel and every
// channel's level left the floor while the encode ran. The optional PNG is
// the window as Qt drew it, meters and all.
int run_smoke(QQmlApplicationEngine& engine, const QString& in_path, const QString& out_path,
              const QString& shot_path) {
    auto* controller =
        engine.singletonInstance<EncoderController*>("Ac3Forge", "EncoderController");
    if (controller == nullptr) {
        std::println(stderr, "smoke: EncoderController singleton is not registered");
        return 1;
    }
    if (engine.rootObjects().isEmpty()) {
        std::println(stderr, "smoke: no root object; the QML failed to load");
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

    // The Repeater that draws the meters. Its count is the QML side of the
    // check: the properties can be perfect and still reach nothing on screen.
    auto* meters = engine.rootObjects().first()->findChild<QObject*>("channelMeters");
    if (meters == nullptr) {
        std::println(stderr, "smoke: the channelMeters repeater is not in the scene");
        return 1;
    }
    const int drawn = meters->property("count").toInt();
    std::println("smoke: QML instantiated {} channel meters", drawn);

    // Sampled from the running encode: the highest level each channel reached
    // while the worker was publishing, not the summary it settles on at the
    // end. A meter that only worked after the fact would pass on the summary
    // alone.
    std::vector<double> highest(static_cast<std::size_t>(controller->channelNames().size()),
                                controller->meterFloorDb());
    auto* sampler = new QTimer(controller);
    sampler->setInterval(10);
    QObject::connect(sampler, &QTimer::timeout, controller, [controller, &highest] {
        if (!controller->metering()) {
            return;
        }
        const auto levels = controller->channelLevels();
        for (qsizetype ch = 0; ch < levels.size() && ch < std::ssize(highest); ++ch) {
            const auto peak = levels[ch].toMap().value(QStringLiteral("peakDb")).toDouble();
            highest[static_cast<std::size_t>(ch)] =
                std::max(highest[static_cast<std::size_t>(ch)], peak);
        }
    });
    sampler->start();

    QObject::connect(
        controller, &EncoderController::encodeFinished, controller,
        [&engine, controller, &highest, drawn, sampler, shot_path](bool ok,
                                                                   const QString& message) {
            sampler->stop();
            std::println("smoke: {}", message.toStdString());
            const auto names = controller->channelNames();
            const auto levels = controller->channelLevels();
            std::println("smoke: {:<5} {:>10} {:>10} {:>10}", "ch", "live peak", "peak", "rms");
            bool every_channel_moved = true;
            for (qsizetype ch = 0; ch < names.size(); ++ch) {
                const auto entry = levels.value(ch).toMap();
                const double live = highest[static_cast<std::size_t>(ch)];
                std::println("smoke: {:<5} {:>10.2f} {:>10.2f} {:>10.2f}",
                             names[ch].toStdString(), live,
                             entry.value(QStringLiteral("peakDb")).toDouble(),
                             entry.value(QStringLiteral("rmsDb")).toDouble());
                every_channel_moved =
                    every_channel_moved && live > controller->meterFloorDb();
            }
            if (!shot_path.isEmpty()) {
                std::println("smoke: window grab -> {}",
                             save_window(engine, shot_path) ? shot_path.toStdString()
                                                            : std::string{"FAILED"});
            }
            const bool passed = ok && drawn == names.size() && every_channel_moved;
            if (!passed) {
                std::println(stderr,
                             "smoke: FAILED (encoded {}, {} meters for {} channels, all "
                             "channels moved {})",
                             ok, drawn, names.size(), every_channel_moved);
            } else {
                std::println("smoke: OK");
            }
            QCoreApplication::exit(passed ? 0 : 1);
        });

    controller->encodeTo(QUrl::fromLocalFile(out_path));
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
