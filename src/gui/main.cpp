#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>

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

    return app.exec();
}
