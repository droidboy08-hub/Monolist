#pragma once

#include <QObject>
#include <QString>

// Opening Monolist when the user signs in to Windows, as Spotify offers it
// (the owner's, 2026-10-08): off, open, or open minimised. Settings >
// Settings chooses; the choice is the app's own value under the user's Run
// key (no administrator: HKEY_CURRENT_USER), which is also where Windows
// lists it in Settings > Apps > Startup. That value is the only record, so
// one Windows' own tools removed reads as off here too.
//
// Windows' Startup apps switch is the user's and is left alone: when it has
// Monolist turned off, the app says so (systemOff) rather than switching it
// back on behind them. Uninstalling removes the value (monolist.iss).
//
// Elsewhere (macOS has login items of its own, for the macOS session to
// add) `supported` is false and Settings shows nothing.
//
// Exposed to QML as the "Startup" singleton.
class Startup : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool supported READ supported CONSTANT)
    // "off", "open" or "minimised".
    Q_PROPERTY(QString mode READ mode NOTIFY changed)
    // Windows' Startup apps has Monolist turned off: whatever is chosen
    // here, it will not open until that is turned on again.
    Q_PROPERTY(bool systemOff READ systemOff NOTIFY changed)

public:
    // The keys under HKEY_CURRENT_USER: Windows' own unless the self-test
    // gives scratch ones.
    explicit Startup(QObject *parent = nullptr);
    Startup(const QString &runKey, const QString &approvedKey, QObject *parent = nullptr);

    bool supported() const;
    QString mode() const;
    bool systemOff() const;
    Q_INVOKABLE void setMode(const QString &mode);
    // Reads both again: Windows' Settings may have changed either while
    // the app was open. Settings calls it as it is shown.
    Q_INVOKABLE void refresh() { Q_EMIT changed(); }

    // The command the Run value holds for a mode, for this executable.
    static QString commandFor(const QString &executable, const QString &mode);
    // The value's name, under both keys.
    static QString valueName();

Q_SIGNALS:
    void changed();

private:
    QString m_runKey;
    QString m_approvedKey;
    // A run against a scratch library (MONOLIST_DATA_DIR) never writes the
    // real Run key: that belongs to the real library's app.
    bool m_readOnly = false;
};
