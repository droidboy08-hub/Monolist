#include "startup.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#ifdef Q_OS_MACOS
#include "macos/loginitem.h"

#include <QFile>
#include <QSaveFile>

#include <CoreFoundation/CoreFoundation.h>
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

#ifdef Q_OS_MACOS
// The Mac's equivalent of the Run value: a LaunchAgent of the user's own,
// which launchd starts once at login (RunAtLoad), with the arguments in it.
// The file is the record, as the value is on Windows; one the user deleted
// reads as off. Its label names the file, as launchd expects.
const char kAgentLabel[] = "io.github.droidboy08-hub.monolist.login";
const char kBundleId[] = "io.github.droidboy08-hub.monolist";

QString agentPath()
{
    return QDir::homePath() + QStringLiteral("/Library/LaunchAgents/") + QLatin1String(kAgentLabel)
           + QStringLiteral(".plist");
}

// The agent's ProgramArguments: the executable, then the flags. Empty when
// there is no agent, or the file is not one (in either of the plist formats,
// so one that plutil or another tool rewrote still reads).
QStringList readArguments(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray bytes = file.read(64 * 1024);
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(bytes.constData()),
                                  CFIndex(bytes.size()));
    CFPropertyListRef list = CFPropertyListCreateWithData(kCFAllocatorDefault, data, kCFPropertyListImmutable,
                                                          nullptr, nullptr);
    CFRelease(data);
    QStringList arguments;
    if (list && CFGetTypeID(list) == CFDictionaryGetTypeID()) {
        const void *value = CFDictionaryGetValue(static_cast<CFDictionaryRef>(list), CFSTR("ProgramArguments"));
        if (value && CFGetTypeID(value) == CFArrayGetTypeID()) {
            const auto array = static_cast<CFArrayRef>(value);
            for (CFIndex i = 0; i < CFArrayGetCount(array); ++i) {
                const void *item = CFArrayGetValueAtIndex(array, i);
                if (CFGetTypeID(item) != CFStringGetTypeID()) {
                    arguments.clear();
                    break;
                }
                arguments << QString::fromCFString(static_cast<CFStringRef>(item));
            }
        }
    }
    if (list)
        CFRelease(list);
    return arguments;
}

// The executable a start at login runs, told it is one, and how to open.
QStringList argumentsFor(const QString &executable, const QString &mode)
{
    QStringList arguments = { executable, QLatin1String(kLoginFlag) };
    if (mode == QLatin1String("minimised"))
        arguments << QLatin1String(kMinimisedFlag);
    else if (mode == QLatin1String("mini"))
        arguments << QLatin1String(kMiniFlag);
    return arguments;
}

bool writeAgent(const QString &path, const QStringList &arguments)
{
    QString strings;
    for (const QString &argument : arguments)
        strings += QStringLiteral("\t\t<string>%1</string>\n").arg(argument.toHtmlEscaped());
    // Interactive: an app the user sees, not a background job for launchd
    // to hold back. AssociatedBundleIdentifiers: listed as Monolist, with
    // its icon, in System Settings' Login Items.
    const QString plist = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "\t<key>Label</key>\n"
        "\t<string>%1</string>\n"
        "\t<key>ProgramArguments</key>\n"
        "\t<array>\n"
        "%2"
        "\t</array>\n"
        "\t<key>RunAtLoad</key>\n"
        "\t<true/>\n"
        "\t<key>LimitLoadToSessionType</key>\n"
        "\t<string>Aqua</string>\n"
        "\t<key>ProcessType</key>\n"
        "\t<string>Interactive</string>\n"
        "\t<key>AssociatedBundleIdentifiers</key>\n"
        "\t<string>%3</string>\n"
        "</dict>\n"
        "</plist>\n").arg(QLatin1String(kAgentLabel), strings, QLatin1String(kBundleId));
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QSaveFile file(path);
    const QByteArray bytes = plist.toUtf8();
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

bool removeAgent(const QString &path)
{
    return !QFileInfo::exists(path) || QFile::remove(path);
}

// One executable however its path is spelt. The Mac's disks ignore case
// unless formatted otherwise, as Windows' do.
bool sameExecutable(const QString &a, const QString &b)
{
    return QDir::cleanPath(a).compare(QDir::cleanPath(b), Qt::CaseInsensitive) == 0;
}

// What a person knows a copy by: the .app around the executable.
QString appOf(const QString &executable)
{
    const qsizetype at = executable.indexOf(QStringLiteral(".app/Contents/MacOS/"), 0, Qt::CaseInsensitive);
    return at < 0 ? QFileInfo(executable).absolutePath() : executable.left(at + 4);
}
#endif

} // namespace

Startup::Startup(QObject *parent)
#ifdef Q_OS_MACOS
    // The Mac has one record, the agent's file; macOS's own switch is asked
    // about by that file's path.
    : Startup(agentPath(), QString(), !qEnvironmentVariable("MONOLIST_DATA_DIR").isEmpty(), parent)
#else
    : Startup(QString::fromLatin1(kRunKey), QString::fromLatin1(kApprovedKey),
              !qEnvironmentVariable("MONOLIST_DATA_DIR").isEmpty(), parent)
#endif
{
}

Startup::Startup(const QString &runKey, const QString &approvedKey, bool readOnly, QObject *parent)
    : QObject(parent), m_runKey(runKey), m_approvedKey(approvedKey), m_readOnly(readOnly)
{
}

bool Startup::supported() const
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
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
#elif defined(Q_OS_MACOS)
    const QStringList arguments = readArguments(m_runKey);
    if (arguments.isEmpty())
        return QStringLiteral("off");
    const QStringList flags = arguments.mid(1);
    if (flags.contains(QLatin1String(kMiniFlag)))
        return QStringLiteral("mini");
    return flags.contains(QLatin1String(kMinimisedFlag)) ? QStringLiteral("minimised") : QStringLiteral("open");
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
#elif defined(Q_OS_MACOS)
    // System Settings › General › Login Items & Extensions, "Allow in the
    // Background": asked only about an agent that is there.
    return QFileInfo::exists(m_runKey) && MacLoginItem::turnedOff(m_runKey);
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
#elif defined(Q_OS_MACOS)
    const QString executable = readArguments(m_runKey).value(0);
    if (executable.isEmpty() || sameExecutable(executable, QCoreApplication::applicationFilePath())
        || !QFileInfo::exists(executable))
        return {};
    return appOf(executable);
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
#elif defined(Q_OS_MACOS)
    if (m_readOnly)
        return;
    const QString executable = readArguments(m_runKey).value(0);
    if (executable.isEmpty())
        return;
    const QString self = QCoreApplication::applicationFilePath();
    if (sameExecutable(executable, self) || QFileInfo::exists(executable))
        return;
    const QString kept = mode();
    if (writeAgent(m_runKey, argumentsFor(self, kept)))
        qInfo("startup: at login, %s, now this copy (the one it named is gone)", qPrintable(kept));
    else
        qWarning("startup: could not point the login agent at this copy");
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
#elif defined(Q_OS_MACOS)
    if (mode != QLatin1String("off") && mode != QLatin1String("open") && mode != QLatin1String("minimised")
        && mode != QLatin1String("mini"))
        return;
    const QStringList wanted = argumentsFor(QCoreApplication::applicationFilePath(), mode);
    if (mode == this->mode() && (mode == QLatin1String("off") || readArguments(m_runKey) == wanted))
        return;
    if (m_readOnly) {
        qWarning("startup: not changed, as this run's library is a scratch one (MONOLIST_DATA_DIR)");
        return;
    }
    const bool done = mode == QLatin1String("off") ? removeAgent(m_runKey) : writeAgent(m_runKey, wanted);
    if (!done)
        qWarning("startup: the login agent could not be changed to \"%s\"", qPrintable(mode));
    else
        qInfo("startup: at login, %s", qPrintable(mode));
    Q_EMIT changed();
#else
    Q_UNUSED(mode);
#endif
}
