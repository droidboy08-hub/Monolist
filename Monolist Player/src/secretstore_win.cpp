#include "secretstore.h"

#include <QtGlobal>

// The Windows store: DPAPI, one file per secret. The whole file is Windows-only;
// on any other platform it compiles to nothing and secretstore.cpp (or the
// Keychain store) answers instead.
#ifdef Q_OS_WIN

#include "appdatabase.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <windows.h>
#include <wincrypt.h>
#include <dpapi.h>

namespace {

// What every file starts with, so that something else in the folder, or a
// file cut off before its blob, is recognised as not a secret before DPAPI is
// asked to make sense of it. The last byte is the format's version.
const QByteArray kMagic = QByteArrayLiteral("MLSS\x01");

// Far above anything kept here (a cookie jar is tens of kilobytes); a file
// larger than this is not one of ours, and is not read into memory whole.
constexpr qint64 kMaxBytes = 4 * 1024 * 1024;

// DPAPI's "optional entropy": a second key the caller must present to decrypt.
// Only this user on this computer can open the blob at all; the entropy adds
// that only Monolist, asking for this name, can. It also means a file copied
// over another's name does not open as that other secret.
QByteArray entropyFor(const QString &name)
{
    return QByteArrayLiteral("Monolist SecretStore 1\n") + name.toUtf8();
}

QString filePath(const QString &name)
{
    return QDir(SecretStore::folderPath()).filePath(name + QStringLiteral(".dpapi"));
}

QString windowsError(DWORD code)
{
    wchar_t *text = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                            | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, code, 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    const QString message = length > 0 ? QString::fromWCharArray(text, int(length)).trimmed() : QString();
    if (text)
        LocalFree(text);
    return QStringLiteral("%1 (Windows error 0x%2)")
        .arg(message.isEmpty() ? QStringLiteral("unknown error") : message)
        .arg(qulonglong(code), 8, 16, QLatin1Char('0'));
}

void setError(QString *error, const QString &text)
{
    if (error)
        *error = text;
}

}

bool SecretStore::available()
{
    return true;
}

QString SecretStore::backendName()
{
    return QStringLiteral("Windows DPAPI");
}

QString SecretStore::unavailableReason()
{
    return QString();
}

// Beside the database, so MONOLIST_DATA_DIR moves it too and a self-test
// never touches the secrets of the copy in daily use.
QString SecretStore::folderPath()
{
    return QFileInfo(AppDatabase::databaseFilePath()).absoluteDir().filePath(QStringLiteral("secrets"));
}

SecretStore::Status SecretStore::platformWrite(const QString &name, const QByteArray &secret, QString *error)
{
    if (secret.size() > kMaxBytes - kMagic.size() - 1024) {
        setError(error, QStringLiteral("too large to keep (%1 bytes)").arg(secret.size()));
        return Status::Failed;
    }
    if (!QDir().mkpath(folderPath())) {
        setError(error, QStringLiteral("cannot create %1").arg(QDir::toNativeSeparators(folderPath())));
        return Status::Failed;
    }

    QByteArray entropy = entropyFor(name);
    DATA_BLOB in{ DWORD(secret.size()), reinterpret_cast<BYTE *>(const_cast<char *>(secret.constData())) };
    DATA_BLOB salt{ DWORD(entropy.size()), reinterpret_cast<BYTE *>(entropy.data()) };
    DATA_BLOB out{ 0, nullptr };
    // UI_FORBIDDEN: DPAPI can be set by policy to prompt; a background save
    // must fail instead of putting up a dialog nobody asked for.
    if (!CryptProtectData(&in, nullptr, &salt, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        setError(error, QStringLiteral("could not encrypt: %1").arg(windowsError(GetLastError())));
        return Status::Failed;
    }
    QByteArray bytes = kMagic;
    bytes.append(reinterpret_cast<const char *>(out.pbData), qsizetype(out.cbData));
    LocalFree(out.pbData);

    // All or nothing: a crash half-way through leaves the old secret in
    // place, not a truncated file that no longer decrypts.
    QSaveFile file(filePath(name));
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        setError(error, QStringLiteral("could not save: %1").arg(file.errorString()));
        return Status::Failed;
    }
    return Status::Ok;
}

SecretStore::Status SecretStore::platformRead(const QString &name, QByteArray *secret, QString *error)
{
    QFile file(filePath(name));
    if (!file.exists())
        return Status::NotFound;
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("could not open: %1").arg(file.errorString()));
        return Status::Failed;
    }
    if (file.size() > kMaxBytes) {
        setError(error, QStringLiteral("not a secret file (%1 bytes)").arg(file.size()));
        return Status::Corrupt;
    }
    const QByteArray bytes = file.readAll();
    if (bytes.size() <= kMagic.size() || !bytes.startsWith(kMagic)) {
        setError(error, QStringLiteral("not a secret file, or cut short"));
        return Status::Corrupt;
    }

    QByteArray entropy = entropyFor(name);
    DATA_BLOB in{ DWORD(bytes.size() - kMagic.size()),
                  reinterpret_cast<BYTE *>(const_cast<char *>(bytes.constData() + kMagic.size())) };
    DATA_BLOB salt{ DWORD(entropy.size()), reinterpret_cast<BYTE *>(entropy.data()) };
    DATA_BLOB out{ 0, nullptr };
    // DPAPI checks the blob's own integrity code before it decrypts, so a
    // changed byte anywhere fails here rather than coming back as a wrong
    // value.
    if (!CryptUnprotectData(&in, nullptr, &salt, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        setError(error, QStringLiteral("does not decrypt: %1").arg(windowsError(GetLastError())));
        return Status::Corrupt;
    }
    *secret = QByteArray(reinterpret_cast<const char *>(out.pbData), qsizetype(out.cbData));
    // The plaintext is copied out; the system's copy does not outlive this.
    if (out.pbData) {
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
    }
    return Status::Ok;
}

SecretStore::Status SecretStore::platformRemove(const QString &name, QString *error)
{
    QFile file(filePath(name));
    if (!file.exists())
        return Status::Ok;
    if (!file.remove()) {
        setError(error, QStringLiteral("could not delete: %1").arg(file.errorString()));
        return Status::Failed;
    }
    return Status::Ok;
}

#endif // Q_OS_WIN
