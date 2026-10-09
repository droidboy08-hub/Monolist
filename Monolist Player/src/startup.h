#pragma once

#include <QObject>
#include <QString>

// Opening Monolist when the user signs in to Windows, as Spotify offers it
// (the owner's, 2026-10-08): off, open, open minimised, or the mini player.
// Settings > Settings chooses; the choice is the app's own value under the
// user's Run key (no administrator: HKEY_CURRENT_USER), which is also where
// Windows lists it in Settings > Apps > Startup. That value is the only
// record, so one Windows' own tools removed reads as off here too.
//
// The value names an executable, and a portable copy moves: a newer download
// unpacked in a folder of its own, the old one deleted. A value naming a
// Monolist that is gone is made this one's at launch (repair), with the same
// choice; one naming another copy that is still there is said in Settings
// (otherCopy), and choosing again makes it this one.
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
    // "off", "open", "minimised" or "mini" (the mini player alone).
    Q_PROPERTY(QString mode READ mode NOTIFY changed)
    // Windows' Startup apps has Monolist turned off: whatever is chosen
    // here, it will not open until that is turned on again.
    Q_PROPERTY(bool systemOff READ systemOff NOTIFY changed)
    // The folder of another copy of Monolist that the value starts rather
    // than this one, still there (an older download kept beside this one);
    // empty when it starts this one, or nothing.
    Q_PROPERTY(QString otherCopy READ otherCopy NOTIFY changed)

public:
    // Windows' own keys under HKEY_CURRENT_USER: read only on a run against
    // a scratch library (MONOLIST_DATA_DIR).
    explicit Startup(QObject *parent = nullptr);
    // Other keys there, the self-test's scratch ones; `readOnly`, never
    // written, as Windows' own are from a scratch library.
    Startup(const QString &runKey, const QString &approvedKey, bool readOnly = false, QObject *parent = nullptr);

    bool supported() const;
    QString mode() const;
    bool systemOff() const;
    QString otherCopy() const;
    bool readOnly() const { return m_readOnly; }
    // A choice, for this executable: the same choice again rewrites a value
    // that starts another copy.
    Q_INVOKABLE void setMode(const QString &mode);
    // Reads both again: Windows' Settings may have changed either while
    // the app was open. Settings calls it as it is shown.
    Q_INVOKABLE void refresh() { Q_EMIT changed(); }
    // At launch (main.cpp): a value naming an executable that is no longer
    // there — moved, or an older download's folder deleted — is made this
    // one's, with the same choice. One naming another copy still there is
    // left to the user (otherCopy): two copies would take it in turns.
    void repair();

    // The command the Run value holds for a mode, for this executable.
    static QString commandFor(const QString &executable, const QString &mode);
    // The executable a Run command starts: the quoted path at its head.
    static QString executableIn(const QString &command);
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
