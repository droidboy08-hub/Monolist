#include "startup.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

// Windows' own keys, under HKEY_CURRENT_USER.
const char kRunKey[] = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const char kApprovedKey[] = "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

// What a start at sign-in is told (main.cpp): --login, so that one finding
// Monolist already open leaves it as it is rather than bringing it forward;
// and --minimised for a window that waits on the taskbar, or --mini for the
// mini player alone.
const char kLoginFlag[] = "--login";
const char kMinimisedFlag[] = "--minimised";
const char kMiniFlag[] = "--mini";

#ifdef Q_OS_WIN
std::wstring wide(const QString &text)
{
    return text.toStdWString();
}

QString readString(const QString &key, const QString &name)
{
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, wide(key).c_str(), wide(name).c_str(), RRF_RT_REG_SZ, nullptr, nullptr,
                     &bytes) != ERROR_SUCCESS || bytes == 0)
        return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, wide(key).c_str(), wide(name).c_str(), RRF_RT_REG_SZ, nullptr, value.data(),
                     &bytes) != ERROR_SUCCESS)
        return {};
    return QString::fromWCharArray(value.c_str());
}

QByteArray readBinary(const QString &key, const QString &name)
{
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, wide(key).c_str(), wide(name).c_str(), RRF_RT_REG_BINARY, nullptr, nullptr,
                     &bytes) != ERROR_SUCCESS || bytes == 0)
        return {};
    QByteArray value(int(bytes), '\0');
    if (RegGetValueW(HKEY_CURRENT_USER, wide(key).c_str(), wide(name).c_str(), RRF_RT_REG_BINARY, nullptr,
                     value.data(), &bytes) != ERROR_SUCCESS)
        return {};
    return value;
}

bool writeString(const QString &key, const QString &name, const QString &value)
{
    HKEY handle = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, wide(key).c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle,
                        nullptr) != ERROR_SUCCESS)
        return false;
    const std::wstring data = wide(value);
    const LONG result = RegSetValueExW(handle, wide(name).c_str(), 0, REG_SZ,
                                       reinterpret_cast<const BYTE *>(data.c_str()),
                                       DWORD((data.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(handle);
    return result == ERROR_SUCCESS;
}

bool removeValue(const QString &key, const QString &name)
{
    const LONG result = RegDeleteKeyValueW(HKEY_CURRENT_USER, wide(key).c_str(), wide(name).c_str());
    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
}

// One executable however its path is spelt: slashes either way, and any
// case, as Windows' file names have none.
bool sameExecutable(const QString &a, const QString &b)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(a))
                   .compare(QDir::cleanPath(QDir::fromNativeSeparators(b)), Qt::CaseInsensitive) == 0;
}
#endif

} // namespace

Startup::Startup(QObject *parent)
    : Startup(QString::fromLatin1(kRunKey), QString::fromLatin1(kApprovedKey),
              !qEnvironmentVariable("MONOLIST_DATA_DIR").isEmpty(), parent)
{
}

Startup::Startup(const QString &runKey, const QString &approvedKey, bool readOnly, QObject *parent)
    : QObject(parent), m_runKey(runKey), m_approvedKey(approvedKey), m_readOnly(readOnly)
{
}

bool Startup::supported() const
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

QString Startup::valueName()
{
    return QStringLiteral("Monolist");
}

QString Startup::commandFor(const QString &executable, const QString &mode)
{
    QString command = QLatin1Char('"') + QDir::toNativeSeparators(executable) + QStringLiteral("\" ")
                      + QLatin1String(kLoginFlag);
    if (mode == QLatin1String("minimised"))
        command += QLatin1Char(' ') + QLatin1String(kMinimisedFlag);
    else if (mode == QLatin1String("mini"))
        command += QLatin1Char(' ') + QLatin1String(kMiniFlag);
    return command;
}

QString Startup::executableIn(const QString &command)
{
    const QString trimmed = command.trimmed();
    if (trimmed.startsWith(QLatin1Char('"'))) {
        const qsizetype end = trimmed.indexOf(QLatin1Char('"'), 1);
        return end < 0 ? trimmed.mid(1) : trimmed.mid(1, end - 1);
    }
    const qsizetype space = trimmed.indexOf(QLatin1Char(' '));
    return space < 0 ? trimmed : trimmed.left(space);
}

QString Startup::mode() const
{
#ifdef Q_OS_WIN
    const QString command = readString(m_runKey, valueName());
    if (command.isEmpty())
        return QStringLiteral("off");
    // Word by word: "--mini" is where "--minimised" begins.
    const QStringList words = command.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.contains(QLatin1String(kMiniFlag)))
        return QStringLiteral("mini");
    return words.contains(QLatin1String(kMinimisedFlag)) ? QStringLiteral("minimised") : QStringLiteral("open");
#else
    return QStringLiteral("off");
#endif
}

// Windows keeps a value beside each startup app: its first byte even while
// the app is allowed to start, odd once the user turned it off in Settings >
// Apps > Startup or Task Manager. No value: never touched, so allowed.
bool Startup::systemOff() const
{
#ifdef Q_OS_WIN
    const QByteArray state = readBinary(m_approvedKey, valueName());
    return !state.isEmpty() && (quint8(state.at(0)) & 1) == 1;
#else
    return false;
#endif
}

QString Startup::otherCopy() const
{
#ifdef Q_OS_WIN
    const QString command = readString(m_runKey, valueName());
    if (command.isEmpty())
        return {};
    const QString executable = executableIn(command);
    if (sameExecutable(executable, QCoreApplication::applicationFilePath()) || !QFileInfo::exists(executable))
        return {};
    return QDir::toNativeSeparators(QFileInfo(executable).absolutePath());
#else
    return {};
#endif
}

void Startup::repair()
{
#ifdef Q_OS_WIN
    if (m_readOnly)
        return;
    const QString command = readString(m_runKey, valueName());
    if (command.isEmpty())
        return;
    const QString executable = executableIn(command);
    const QString self = QCoreApplication::applicationFilePath();
    if (sameExecutable(executable, self) || QFileInfo::exists(executable))
        return;
    const QString kept = mode();
    if (writeString(m_runKey, valueName(), commandFor(self, kept)))
        qInfo("startup: at sign-in, %s, now this copy (the one it named is gone)", qPrintable(kept));
    else
        qWarning("startup: Windows did not take this copy in place of the one gone");
    Q_EMIT changed();
#endif
}

void Startup::setMode(const QString &mode)
{
#ifdef Q_OS_WIN
    if (mode != QLatin1String("off") && mode != QLatin1String("open") && mode != QLatin1String("minimised")
        && mode != QLatin1String("mini"))
        return;
    // Already so, and for this executable: nothing to do. The same choice
    // naming another copy is written again, for this one.
    if (mode == this->mode()
        && (mode == QLatin1String("off")
            || readString(m_runKey, valueName()) == commandFor(QCoreApplication::applicationFilePath(), mode)))
        return;
    if (m_readOnly) {
        qWarning("startup: not changed, as this run's library is a scratch one (MONOLIST_DATA_DIR)");
        return;
    }
    const bool done = mode == QLatin1String("off")
            ? removeValue(m_runKey, valueName())
            : writeString(m_runKey, valueName(), commandFor(QCoreApplication::applicationFilePath(), mode));
    if (!done)
        qWarning("startup: Windows did not take the change to \"%s\"", qPrintable(mode));
    else
        qInfo("startup: at sign-in, %s", qPrintable(mode));
    Q_EMIT changed();
#else
    Q_UNUSED(mode);
#endif
}
