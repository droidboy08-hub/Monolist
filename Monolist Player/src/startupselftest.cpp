#include "startupselftest.h"

#include "startup.h"

#include <QCoreApplication>
#include <QDir>

#ifdef Q_OS_WIN
#include <windows.h>
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
#endif

} // namespace

int runStartupSelfTest()
{
    Checks t;
    // The guard on the real Run key is this: without it, the last check
    // below would switch the real sign-in start.
    if (qEnvironmentVariable("MONOLIST_DATA_DIR").isEmpty()) {
        qWarning("startup-test: needs MONOLIST_DATA_DIR (a scratch folder)");
        return 1;
    }
#ifdef Q_OS_WIN
    RegDeleteTreeW(HKEY_CURRENT_USER, kScratch);
    Startup startup(QStringLiteral("Software\\Monolist-selftest\\Run"),
                    QStringLiteral("Software\\Monolist-selftest\\Approved"));
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
    startup.setMode(QStringLiteral("sometimes"));
    t.check(startup.mode() == QLatin1String("minimised"), QStringLiteral("  an unknown choice changes nothing"));

    setApproved(0x03);
    t.check(startup.systemOff(), QStringLiteral("Windows' Startup apps turned it off: said"));
    t.check(startup.mode() == QLatin1String("minimised"), QStringLiteral("  and the choice here is left as it is"));
    setApproved(0x02);
    t.check(!startup.systemOff(), QStringLiteral("turned on there again: nothing said"));

    startup.setMode(QStringLiteral("off"));
    t.check(scratchRunCommand().isEmpty() && startup.mode() == QLatin1String("off"),
            QStringLiteral("off: the value taken away"));
    startup.setMode(QStringLiteral("off"));
    t.check(startup.mode() == QLatin1String("off"), QStringLiteral("  and off again is no trouble"));

    t.check(Startup::commandFor(QStringLiteral("C:/Program Files/Monolist/monolist.exe"), QStringLiteral("open"))
                == QStringLiteral("\"C:\\Program Files\\Monolist\\monolist.exe\" --login"),
            QStringLiteral("a path with a space is quoted whole"));

    // The real key, from a run on a scratch library (this one): read, never
    // written. Asked for a mode it does not have, it leaves Windows' Run key
    // exactly as it was.
    Startup real;
    const QString before = real.mode();
    real.setMode(before == QLatin1String("off") ? QStringLiteral("open") : QStringLiteral("off"));
    t.check(real.mode() == before, QStringLiteral("a scratch library never writes the real Run key"), real.mode());

    RegDeleteTreeW(HKEY_CURRENT_USER, kScratch);
#else
    Startup startup;
    t.check(!startup.supported() && startup.mode() == QLatin1String("off"),
            QStringLiteral("not offered here, and off"));
#endif
    return t.finish();
}
