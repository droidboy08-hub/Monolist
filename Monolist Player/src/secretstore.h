#pragma once

#include <QByteArray>
#include <QString>

// Somewhere to keep the few things that must never sit in the database: the
// Last.fm session key now, a YouTube Music cookie jar later.
//
// The settings table is the wrong place for them. Any QML can read it
// (Library.settingValue), `--set` echoes what it writes to the log, and the
// database is a plain SQLite file that backup tools copy anywhere. So a secret
// lives here instead, under a short name ("lastfm.session"), encrypted by the
// operating system for the signed-in user:
//
//  - Windows: DPAPI (CryptProtectData), one file per secret in a "secrets"
//    folder beside the database (secretstore_win.cpp). Not Credential Manager:
//    its blobs are capped at 2,560 bytes, 512 in MinGW's headers, and a cookie
//    jar outgrows either.
//  - macOS: the Keychain, in secretstore_mac.mm, which defines
//    MONOLIST_HAVE_KEYCHAIN from CMake when it is added.
//  - Anywhere else: not available yet. Every call says so rather than falling
//    back to a plain file, so whatever wanted to keep a secret can tell the
//    user it will not be remembered.
//
// Nothing here logs what goes in or comes out, and nothing that calls it may
// either. The names are not secret and are fine to log.
class SecretStore
{
public:
    enum class Status {
        Ok,
        NotFound,     // nothing is stored under that name
        Corrupt,      // something is, but it does not decrypt: altered, cut short,
                      // or written by another user or on another computer
        Unavailable,  // no store on this platform yet
        Failed        // the disk or the system refused; the error text says which
    };

    static bool available();
    // "Windows DPAPI" or "macOS Keychain"; empty where there is none.
    static QString backendName();
    // Why there is none, in a sentence; empty where there is one.
    static QString unavailableReason();
    // The folder a file-backed store keeps its files in; empty for one that
    // keeps them elsewhere (the Keychain) or has none.
    static QString folderPath();

    // Replaces whatever was stored under `name`.
    static Status write(const QString &name, const QByteArray &secret, QString *error = nullptr);
    // `secret` is cleared on anything but Ok, so a caller can never mistake
    // half a value for the value.
    static Status read(const QString &name, QByteArray *secret, QString *error = nullptr);
    // Removing what is not there is Ok: what the caller wanted is true.
    static Status remove(const QString &name, QString *error = nullptr);

    // A name is 1-64 characters of lower-case letters, digits, '.', '-' and
    // '_', starting with a letter or digit, and never a Windows device name
    // ("con", "nul", "com1"...) before its first dot. Every name is a
    // constant in the code, but it becomes a file name, so the rule is kept.
    static bool validName(const QString &name);

    static QString statusText(Status status);

private:
    // One implementation per platform; the public calls above check the name
    // first, so these never see an invalid one.
    static Status platformWrite(const QString &name, const QByteArray &secret, QString *error);
    static Status platformRead(const QString &name, QByteArray *secret, QString *error);
    static Status platformRemove(const QString &name, QString *error);
};
