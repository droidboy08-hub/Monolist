#include "startupselftest.h"

#include "instanceguard.h"
#include "startup.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#ifdef Q_OS_MACOS
#include <QSaveFile>

#include <CoreFoundation/CoreFoundation.h>
#endif

namespace {

// One line per check, and a count at the end, as in the other self-tests.
class Checks
{
public:
    bool check(bool ok, const QString &what, const QString &detail = QString())
    {
        ++m_count;
        if (!ok)
            ++m_failed;
        const QString line = ok || detail.isEmpty() ? what : what + QStringLiteral("  -- ") + detail;
        qWarning("startup-test: %s  %s", ok ? "ok  " : "FAIL", qPrintable(line));
        return ok;
    }

    int finish()
    {
        qWarning("startup-test: %d checks, %d failed", m_count, m_failed);
        return m_failed;
    }

private:
    int m_count = 0;
    int m_failed = 0;
};

#ifdef Q_OS_WIN
// The test's own key, under HKEY_CURRENT_USER; nothing of Windows' own.
const wchar_t kScratch[] = L"Software\\Monolist-selftest";

QString scratchRunCommand()
{
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Monolist-selftest\\Run", L"Monolist", RRF_RT_REG_SZ, nullptr,
                     nullptr, &bytes) != ERROR_SUCCESS)
        return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Monolist-selftest\\Run", L"Monolist", RRF_RT_REG_SZ, nullptr,
                 value.data(), &bytes);
    return QString::fromWCharArray(value.c_str());
}

void setApproved(quint8 first)
{
    HKEY key = nullptr;
    RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Monolist-selftest\\Approved", 0, nullptr, 0, KEY_SET_VALUE,
                    nullptr, &key, nullptr);
    const BYTE state[12] = { first, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    RegSetValueExW(key, L"Monolist", 0, REG_BINARY, state, sizeof(state));
    RegCloseKey(key);
}

// A Run value as some other copy of Monolist, or an older one, left it.
void setScratchRun(const QString &command)
{
    HKEY key = nullptr;
    RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Monolist-selftest\\Run", 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                    &key, nullptr);
    const std::wstring data = command.toStdWString();
    RegSetValueExW(key, L"Monolist", 0, REG_SZ, reinterpret_cast<const BYTE *>(data.c_str()),
                   DWORD((data.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}
#endif

#ifdef Q_OS_MACOS
// What a login agent file says, read as launchd reads it.
struct Agent {
    bool parsed = false;
    QString label;
    bool runAtLoad = false;
    QStringList arguments;
};

Agent readAgent(const QString &path)
{
    Agent agent;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return agent;
    const QByteArray bytes = file.readAll();
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(bytes.constData()),
                                  CFIndex(bytes.size()));
    CFPropertyListRef list = CFPropertyListCreateWithData(kCFAllocatorDefault, data, kCFPropertyListImmutable,
                                                          nullptr, nullptr);
    CFRelease(data);
    if (list && CFGetTypeID(list) == CFDictionaryGetTypeID()) {
        const auto dict = static_cast<CFDictionaryRef>(list);
        agent.parsed = true;
        const void *label = CFDictionaryGetValue(dict, CFSTR("Label"));
        if (label && CFGetTypeID(label) == CFStringGetTypeID())
            agent.label = QString::fromCFString(static_cast<CFStringRef>(label));
        agent.runAtLoad = CFDictionaryGetValue(dict, CFSTR("RunAtLoad")) == kCFBooleanTrue;
        const void *arguments = CFDictionaryGetValue(dict, CFSTR("ProgramArguments"));
        if (arguments && CFGetTypeID(arguments) == CFArrayGetTypeID()) {
            const auto array = static_cast<CFArrayRef>(arguments);
            for (CFIndex i = 0; i < CFArrayGetCount(array); ++i)
                agent.arguments << QString::fromCFString(static_cast<CFStringRef>(CFArrayGetValueAtIndex(array, i)));
        }
    }
    if (list)
        CFRelease(list);
    return agent;
}

// An agent as some other copy of Monolist, or an older one, left it.
void writeAgent(const QString &path, const QStringList &arguments)
{
    QString strings;
    for (const QString &argument : arguments)
        strings += QStringLiteral("<string>%1</string>").arg(argument.toHtmlEscaped());
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?><plist version=\"1.0\"><dict>"
                                  "<key>Label</key><string>io.github.droidboy08-hub.monolist.login</string>"
                                  "<key>ProgramArguments</key><array>%1</array>"
                                  "<key>RunAtLoad</key><true/></dict></plist>").arg(strings).toUtf8());
        file.commit();
    }
}
#endif

// The event loop run for a while, as the app's own would: where the first
// Monolist hears a second start.
void settle(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

} // namespace

int runStartupSelfTest()
{
    Checks t;
    // Its scratch folders are in the scratch library, and nothing here asks
    // anything of Windows' own Run key: the one Startup made on it is only
    // asked whether it is read-only.
    const QString scratchLibrary = qEnvironmentVariable("MONOLIST_DATA_DIR");
    if (scratchLibrary.isEmpty()) {
        qWarning("startup-test: needs MONOLIST_DATA_DIR (a scratch folder)");
        return 1;
    }

    // A start at sign-in (--login) that finds Monolist already open on its
    // library: answered, so it ends, and that one's window left where it is;
    // any other second start still brings it forward (InstanceGuard).
    {
        const QString folder = QDir(scratchLibrary).filePath(QStringLiteral("guard"));
        InstanceGuard running(folder);
        int wakes = 0;
        QObject::connect(&running, &InstanceGuard::wakeRequested, [&wakes]() { ++wakes; });
        // Each second start on a thread of its own, as in a process of its
        // own, while this thread's event loop, the first Monolist's, hears
        // it. True when it was answered, and so ends.
        const auto secondStart = [&folder](bool wake) {
            bool first = true;
            bool answered = false;
            QThread *start = QThread::create([&folder, wake, &first, &answered]() {
                InstanceGuard guard(folder, wake);
                first = guard.first();
                answered = guard.answered();
            });
            QEventLoop loop;
            QObject::connect(start, &QThread::finished, &loop, &QEventLoop::quit);
            QTimer::singleShot(10000, &loop, &QEventLoop::quit);
            start->start();
            loop.exec();
            start->wait();
            delete start;
            settle(300);
            return !first && answered;
        };
        const bool atSignIn = secondStart(false);
        t.check(running.first() && atSignIn && wakes == 0,
                QStringLiteral("a start at sign-in finding Monolist open: answered, its window left alone"),
                QStringLiteral("wakes %1").arg(wakes));
        const bool opened = secondStart(true);
        t.check(opened && wakes == 1, QStringLiteral("  any other second start brings the window forward"),
                QStringLiteral("wakes %1").arg(wakes));
    }

#ifdef Q_OS_WIN
    RegDeleteTreeW(HKEY_CURRENT_USER, kScratch);
    const QString scratchRun = QStringLiteral("Software\\Monolist-selftest\\Run");
    const QString scratchApproved = QStringLiteral("Software\\Monolist-selftest\\Approved");
    Startup startup(scratchRun, scratchApproved);
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());

    t.check(startup.supported(), QStringLiteral("offered on Windows"));
    t.check(startup.mode() == QLatin1String("off") && !startup.systemOff(),
            QStringLiteral("no value: off, and Windows has nothing against it"));

    int changes = 0;
    QObject::connect(&startup, &Startup::changed, [&changes]() { ++changes; });
    startup.setMode(QStringLiteral("open"));
    t.check(scratchRunCommand() == QLatin1Char('"') + exe + QStringLiteral("\" --login"),
            QStringLiteral("open: this executable, quoted, told it is a sign-in start"), scratchRunCommand());
    t.check(startup.mode() == QLatin1String("open") && changes == 1, QStringLiteral("  read back as open, said once"));
    startup.setMode(QStringLiteral("minimised"));
    t.check(scratchRunCommand().endsWith(QLatin1String(" --login --minimised"))
                && startup.mode() == QLatin1String("minimised"),
            QStringLiteral("minimised: the same, with --minimised"), scratchRunCommand());
    startup.setMode(QStringLiteral("minimised"));
    t.check(changes == 2, QStringLiteral("  the same choice again changes nothing"));
    startup.setMode(QStringLiteral("mini"));
    t.check(scratchRunCommand().endsWith(QLatin1String(" --login --mini")) && startup.mode() == QLatin1String("mini"),
            QStringLiteral("the mini player: --mini, read back as itself and not as minimised"), scratchRunCommand());
    startup.setMode(QStringLiteral("minimised"));
    t.check(startup.mode() == QLatin1String("minimised"), QStringLiteral("  and back to minimised"));
    startup.setMode(QStringLiteral("sometimes"));
    t.check(startup.mode() == QLatin1String("minimised"), QStringLiteral("  an unknown choice changes nothing"));

    setApproved(0x03);
    t.check(startup.systemOff(), QStringLiteral("Windows' Startup apps turned it off: said"));
    t.check(startup.mode() == QLatin1String("minimised"), QStringLiteral("  and the choice here is left as it is"));
    setApproved(0x02);
    t.check(!startup.systemOff(), QStringLiteral("turned on there again: nothing said"));

    // The copy the value names moved, or its folder (an older download's)
    // was deleted: made this one's at launch, the same choice kept.
    const QString gone = QStringLiteral("\"C:\\Monolist-selftest-nowhere\\Monolist-0.1.1\\monolist.exe\" --login --mini");
    setScratchRun(gone);
    t.check(startup.mode() == QLatin1String("mini") && startup.otherCopy().isEmpty(),
            QStringLiteral("a value naming a Monolist that is gone: still the mini player, no other copy to name"),
            startup.otherCopy());
    startup.repair();
    t.check(scratchRunCommand() == Startup::commandFor(exe, QStringLiteral("mini")),
            QStringLiteral("  at launch, this executable in its place, the same choice"), scratchRunCommand());
    const int afterRepair = changes;
    startup.repair();
    t.check(changes == afterRepair && scratchRunCommand() == Startup::commandFor(exe, QStringLiteral("mini")),
            QStringLiteral("  and this one's own is left as it is"));

    // Another copy, still there (an older download kept beside this one):
    // left at launch, since two copies would take it in turns, but said;
    // choosing the same again makes it this one.
    const QString otherFolder = QDir(scratchLibrary).filePath(QStringLiteral("other-copy"));
    QDir().mkpath(otherFolder);
    QFile otherExe(QDir(otherFolder).filePath(QStringLiteral("monolist.exe")));
    if (otherExe.open(QIODevice::WriteOnly))
        otherExe.close();
    setScratchRun(Startup::commandFor(otherExe.fileName(), QStringLiteral("open")));
    startup.repair();
    t.check(scratchRunCommand() == Startup::commandFor(otherExe.fileName(), QStringLiteral("open")),
            QStringLiteral("another copy still there: left as it is at launch"), scratchRunCommand());
    t.check(startup.otherCopy().compare(QDir::toNativeSeparators(QFileInfo(otherFolder).absoluteFilePath()),
                                        Qt::CaseInsensitive) == 0,
            QStringLiteral("  and named, for Settings to say"), startup.otherCopy());
    startup.setMode(QStringLiteral("open"));
    t.check(scratchRunCommand() == Startup::commandFor(exe, QStringLiteral("open")) && startup.otherCopy().isEmpty(),
            QStringLiteral("  the same choice again: this copy now"), scratchRunCommand());

    startup.setMode(QStringLiteral("off"));
    t.check(scratchRunCommand().isEmpty() && startup.mode() == QLatin1String("off"),
            QStringLiteral("off: the value taken away"));
    startup.setMode(QStringLiteral("off"));
    t.check(startup.mode() == QLatin1String("off"), QStringLiteral("  and off again is no trouble"));

    t.check(Startup::commandFor(QStringLiteral("C:/Program Files/Monolist/monolist.exe"), QStringLiteral("open"))
                == QStringLiteral("\"C:\\Program Files\\Monolist\\monolist.exe\" --login"),
            QStringLiteral("a path with a space is quoted whole"));
    t.check(Startup::executableIn(QStringLiteral("\"C:\\Program Files\\Monolist\\monolist.exe\" --login --mini"))
                == QStringLiteral("C:\\Program Files\\Monolist\\monolist.exe"),
            QStringLiteral("  and read back whole"));

    // Windows' own Run key, from a run on a scratch library (this one):
    // read-only. Only asked whether it is; nothing here would undo a write
    // there, so none is ever tried.
    Startup real;
    t.check(real.readOnly(), QStringLiteral("a scratch library never writes the real Run key: read-only"));
    // And read-only holds, shown on the scratch key: neither a choice nor
    // a repair writes anything.
    Startup guarded(scratchRun, scratchApproved, true);
    guarded.setMode(QStringLiteral("open"));
    t.check(scratchRunCommand().isEmpty() && guarded.mode() == QLatin1String("off"),
            QStringLiteral("  read-only: a choice writes nothing"), scratchRunCommand());
    setScratchRun(gone);
    guarded.repair();
    t.check(scratchRunCommand() == gone, QStringLiteral("  nor does a repair"), scratchRunCommand());

    RegDeleteTreeW(HKEY_CURRENT_USER, kScratch);
#elif defined(Q_OS_MACOS)
    // A login agent of the test's own, in the scratch library: nothing in
    // the user's own LaunchAgents is touched, and macOS has never seen it.
    const QString agents = QDir(scratchLibrary).filePath(QStringLiteral("LaunchAgents"));
    QDir().mkpath(agents);
    const QString agentPath = QDir(agents).filePath(QStringLiteral("io.github.droidboy08-hub.monolist.login.plist"));
    QFile::remove(agentPath);
    Startup startup(agentPath, QString());
    const QString exe = QCoreApplication::applicationFilePath();
    const auto said = [&agentPath]() { return readAgent(agentPath).arguments.join(QLatin1Char(' ')); };

    t.check(startup.supported(), QStringLiteral("offered on the Mac"));
    t.check(startup.mode() == QLatin1String("off") && !startup.systemOff(),
            QStringLiteral("no agent: off, and macOS has nothing against it"));

    int changes = 0;
    QObject::connect(&startup, &Startup::changed, [&changes]() { ++changes; });
    startup.setMode(QStringLiteral("open"));
    {
        const Agent agent = readAgent(agentPath);
        t.check(agent.parsed && agent.label == QLatin1String("io.github.droidboy08-hub.monolist.login")
                    && agent.runAtLoad,
                QStringLiteral("open: an agent launchd can read, labelled as its file, run at login"), agent.label);
        t.check(agent.arguments == QStringList({ exe, QStringLiteral("--login") }),
                QStringLiteral("  this executable, told it is a login start"), said());
    }
    t.check(startup.mode() == QLatin1String("open") && changes == 1, QStringLiteral("  read back as open, said once"));
    startup.setMode(QStringLiteral("minimised"));
    t.check(readAgent(agentPath).arguments == QStringList({ exe, QStringLiteral("--login"), QStringLiteral("--minimised") })
                && startup.mode() == QLatin1String("minimised"),
            QStringLiteral("minimised: the same, with --minimised"), said());
    startup.setMode(QStringLiteral("minimised"));
    t.check(changes == 2, QStringLiteral("  the same choice again changes nothing"));
    startup.setMode(QStringLiteral("mini"));
    t.check(readAgent(agentPath).arguments.endsWith(QStringLiteral("--mini")) && startup.mode() == QLatin1String("mini"),
            QStringLiteral("the mini player: --mini, read back as itself and not as minimised"), said());
    startup.setMode(QStringLiteral("minimised"));
    t.check(startup.mode() == QLatin1String("minimised"), QStringLiteral("  and back to minimised"));
    startup.setMode(QStringLiteral("sometimes"));
    t.check(startup.mode() == QLatin1String("minimised"), QStringLiteral("  an unknown choice changes nothing"));
    t.check(!startup.systemOff(), QStringLiteral("an agent macOS has never seen is not said to be turned off"));

    // The copy the agent names moved, or was deleted: made this one's at
    // launch, the same choice kept.
    const QStringList gone = { QStringLiteral("/Monolist-selftest-nowhere/Monolist.app/Contents/MacOS/Monolist"),
                               QStringLiteral("--login"), QStringLiteral("--mini") };
    writeAgent(agentPath, gone);
    t.check(startup.mode() == QLatin1String("mini") && startup.otherCopy().isEmpty(),
            QStringLiteral("an agent naming a Monolist that is gone: still the mini player, no other copy to name"),
            startup.otherCopy());
    startup.repair();
    t.check(readAgent(agentPath).arguments == QStringList({ exe, QStringLiteral("--login"), QStringLiteral("--mini") }),
            QStringLiteral("  at launch, this executable in its place, the same choice"), said());
    const int afterRepair = changes;
    startup.repair();
    t.check(changes == afterRepair, QStringLiteral("  and this one's own is left as it is"));

    // Another copy, still there (an older Monolist.app kept elsewhere): left
    // at launch, but named by its .app, for Settings to say.
    const QString otherApp = QDir(scratchLibrary).filePath(QStringLiteral("other & co/Monolist.app"));
    QDir().mkpath(otherApp + QStringLiteral("/Contents/MacOS"));
    const QString otherExe = otherApp + QStringLiteral("/Contents/MacOS/Monolist");
    QFile other(otherExe);
    if (other.open(QIODevice::WriteOnly))
        other.close();
    writeAgent(agentPath, { otherExe, QStringLiteral("--login") });
    startup.repair();
    t.check(readAgent(agentPath).arguments.value(0) == otherExe,
            QStringLiteral("another copy still there: left as it is at launch"), said());
    t.check(startup.otherCopy() == otherApp, QStringLiteral("  and named by its .app, '&' and all"), startup.otherCopy());
    startup.setMode(QStringLiteral("open"));
    t.check(readAgent(agentPath).arguments.value(0) == exe && startup.otherCopy().isEmpty(),
            QStringLiteral("  the same choice again: this copy now"), said());

    startup.setMode(QStringLiteral("off"));
    t.check(!QFileInfo::exists(agentPath) && startup.mode() == QLatin1String("off"),
            QStringLiteral("off: the agent taken away"));
    startup.setMode(QStringLiteral("off"));
    t.check(startup.mode() == QLatin1String("off"), QStringLiteral("  and off again is no trouble"));

    // The user's own LaunchAgents, from a run on a scratch library (this
    // one): read-only, and only asked whether it is.
    Startup real;
    t.check(real.readOnly(), QStringLiteral("a scratch library never writes the real login agent: read-only"));
    Startup guarded(agentPath, QString(), true);
    guarded.setMode(QStringLiteral("open"));
    t.check(!QFileInfo::exists(agentPath) && guarded.mode() == QLatin1String("off"),
            QStringLiteral("  read-only: a choice writes nothing"));
    writeAgent(agentPath, gone);
    guarded.repair();
    t.check(readAgent(agentPath).arguments == gone, QStringLiteral("  nor does a repair"), said());
    QDir(agents).removeRecursively();
#else
    Startup startup;
    t.check(!startup.supported() && startup.mode() == QLatin1String("off"),
            QStringLiteral("not offered here, and off"));
#endif
    return t.finish();
}
