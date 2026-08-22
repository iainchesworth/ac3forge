#pragma once

#include <QColor>
#include <QObject>
#include <QtQmlIntegration>

// The desktop's own accent colour, for Theme.qml's "system" palette. Qt's
// platform themes fill QPalette::Accent natively - the Windows accent
// colour, macOS's control accent, KDE's own accent - and Qt itself falls
// back to Highlight where a platform exposes none, so reading it here is
// the supported mechanism, never an invented integration, and it never
// yields "no colour". Palette changes land live: the OS Settings app
// flipping the accent (or the colour scheme, which can carry a different
// accent with it) reaches the running window without a restart.
class SystemTheme : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QColor accentColor READ accentColor NOTIFY accentChanged)

public:
    explicit SystemTheme(QObject* parent = nullptr);

    [[nodiscard]] QColor accentColor() const;

signals:
    void accentChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
};
