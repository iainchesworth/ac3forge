#include <QtQuickTest/quicktest.h>

#include <QCoreApplication>
#include <QObject>
#include <QQuickStyle>
#include <QSettings>
#include <QTemporaryDir>

#include <optional>

// Standard Qt Quick Test entry point: discovers and runs every tst_*.qml
// file under QUICK_TEST_SOURCE_DIR (set in CMakeLists.txt), exercising the
// real EncoderController the qmldir already embedded into this binary
// resolves - see CMakeLists.txt for why that is a second embedding of the
// module rather than a shared library with ac3gui.
//
// The setup object exists for one reason: Main.qml's QML Settings must be
// HERMETIC here. With no organization/application identifiers, Qt 6.8's
// Settings failed to initialise and every window saw in-memory defaults -
// accidental but perfect isolation. Qt 6.10's fallback store PERSISTS
// instead, across windows and across runs, so every freshly created test
// window restored the previous window's saved session on top of the one
// live controller they all share - one duplicated source per test, and
// assignments reappearing from runs that finished days earlier. Real
// identifiers plus a QTemporaryDir settings path make the store behave
// normally and evaporate with the process; session restore itself is
// seeded OFF because restoring is a fresh-process feature no test wants
// firing under a shared controller.
class SettingsIsolation : public QObject {
    Q_OBJECT

public slots:
    void applicationAvailable() {
        QCoreApplication::setOrganizationName(QStringLiteral("ac3forge-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("ac3gui_qmltests"));

        // main.cpp forces Fusion before its engine loads any QML - see that
        // file's own comment: it renders identically on every platform,
        // where the native style would restyle controls at runtime. This
        // binary never did, which matters for more than looks: without it,
        // Qt Quick Controls resolves to the platform's native style, whose
        // native-theme queries are the documented cause of a real,
        // reproducible hang under the offscreen QPA platform (see
        // src/gui/qml/Main.qml's "native Button inside a Repeater fed real
        // data" comment for the first occurrence, on Windows). That
        // occurrence was worked around locally in QML; a second one surfaced
        // here on macOS - not in the Repeater that fix already covers, but
        // in addDeviceBox (Main.qml), a plain native ComboBox populated from
        // EncoderController.captureDevices once a live capture session with
        // a real device selects it. Matching main.cpp's style here removes
        // the native-theme code path this binary was the only place still
        // exercising, rather than chasing each control it happens to affect
        // one at a time - and it is also more correct on its own terms: the
        // QML under test customizes several controls' contentItem (e.g.
        // Main.qml:1584, the exact line the "current style does not support
        // customization" QWARN below names), which only works under a
        // non-native style - Fusion, same as the shipped app - so this
        // binary had been exercising a style ac3gui never actually ships
        // with.
        QQuickStyle::setStyle(QStringLiteral("Fusion"));

        scratch_.emplace();
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, scratch_->path());

        QSettings settings;
        settings.beginGroup(QStringLiteral("workbench"));
        settings.setValue(QStringLiteral("restoreSession"), false);
    }

private:
    std::optional<QTemporaryDir> scratch_;
};

QUICK_TEST_MAIN_WITH_SETUP(ac3gui, SettingsIsolation)

#include "qml_test_main.moc"
