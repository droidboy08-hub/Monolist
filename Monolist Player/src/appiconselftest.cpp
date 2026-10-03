#include "appiconselftest.h"

#include "appicon.h"
#include "library.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>

#ifdef Q_OS_WIN
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
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
        qWarning("icon-test: %s  %s", ok ? "ok  " : "FAIL", qPrintable(line));
        return ok;
    }

    int finish()
    {
        qWarning("icon-test: %d checks, %d failed", m_count, m_failed);
        return m_failed;
    }

private:
    int m_count = 0;
    int m_failed = 0;
};

#ifdef Q_OS_WIN
// A shortcut to `target`, made the way the installer's are (no icon of its
// own), in the scratch folder.
bool makeShortcut(const QString &path, const QString &target)
{
    IShellLinkW *link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                reinterpret_cast<void **>(&link))))
        return false;
    IPersistFile *file = nullptr;
    bool ok = SUCCEEDED(link->SetPath(QDir::toNativeSeparators(target).toStdWString().c_str()))
              && SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&file)))
              && SUCCEEDED(file->Save(QDir::toNativeSeparators(path).toStdWString().c_str(), TRUE));
    if (file)
        file->Release();
    link->Release();
    return ok;
}

// Its icon as {file, index}; an empty file for none of its own.
QPair<QString, int> iconOf(const QString &path)
{
    QPair<QString, int> result{ QString(), -1 };
    IShellLinkW *link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                reinterpret_cast<void **>(&link))))
        return result;
    IPersistFile *file = nullptr;
    wchar_t icon[MAX_PATH] = {};
    int index = -1;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&file)))
        && SUCCEEDED(file->Load(QDir::toNativeSeparators(path).toStdWString().c_str(), STGM_READ))
        && SUCCEEDED(link->GetIconLocation(icon, MAX_PATH, &index)))
        result = { QString::fromWCharArray(icon), index };
    if (file)
        file->Release();
    link->Release();
    return result;
}
#endif

QByteArray digest(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha1);
}

} // namespace

int runAppIconSelfTest(Library *library)
{
    Checks checks;
    const QString data = qEnvironmentVariable("MONOLIST_DATA_DIR");
    if (data.isEmpty()) {
        qWarning("icon-test: needs MONOLIST_DATA_DIR (a scratch folder), so no real shortcut is touched");
        return 1;
    }

    // — the choices —
    AppIcon icons(library);
    // Never the real Start menu, desktop or taskbar from here.
    icons.setShortcutFolders({});
    const QVariantList choices = icons.choices();
    checks.check(choices.size() == 2, QStringLiteral("two icons to choose from"));
    checks.check(icons.current() == QLatin1String("note"), QStringLiteral("the note is the default"));
    checks.check(AppIcon::indexOf(QStringLiteral("note")) == 0 && AppIcon::indexOf(QStringLiteral("note-red")) == 1,
                 QStringLiteral("the default is the executable's first icon, the red its second"));
    checks.check(AppIcon::indexOf(QStringLiteral("nothing")) == -1, QStringLiteral("an unknown id is no choice"));
    for (const QVariant &choice : choices) {
        const QVariantMap map = choice.toMap();
        const QString id = map.value(QStringLiteral("id")).toString();
        const QIcon icon = AppIcon::iconFor(id);
        checks.check(!icon.isNull() && icon.availableSizes().size() >= 7,
                     QStringLiteral("%1 has a picture at every size").arg(id),
                     QString::number(icon.availableSizes().size()));
        const QString preview = map.value(QStringLiteral("preview")).toString().mid(3); // "qrc" -> ":"
        checks.check(QFile::exists(preview), QStringLiteral("%1's preview is in the resources").arg(id), preview);
    }

    // — kept in the settings —
    icons.choose(QStringLiteral("note-red"));
    checks.check(icons.current() == QLatin1String("note-red")
                         && library->settingValue(QStringLiteral("app.icon")) == QLatin1String("note-red"),
                 QStringLiteral("a choice is kept in the settings"));
    icons.choose(QStringLiteral("nothing"));
    checks.check(icons.current() == QLatin1String("note-red"), QStringLiteral("an unknown id changes nothing"));
    {
        AppIcon next(library);
        next.setShortcutFolders({});
        next.restore();
        checks.check(next.current() == QLatin1String("note-red"), QStringLiteral("the next launch takes it"));
    }
    library->setSetting(QStringLiteral("app.icon"), QStringLiteral("bogus"));
    {
        AppIcon next(library);
        next.setShortcutFolders({});
        next.restore();
        checks.check(next.current() == QLatin1String("note"), QStringLiteral("a bad setting falls back to the note"));
    }

#ifdef Q_OS_WIN
    // — the executable's icons —
    const QString exe = QCoreApplication::applicationFilePath();
    const UINT count = ExtractIconExW(QDir::toNativeSeparators(exe).toStdWString().c_str(), -1, nullptr, nullptr, 0);
    checks.check(count >= 2, QStringLiteral("the executable carries both icons"), QString::number(count));

    // — shortcuts, in a scratch folder —
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const QString folder = QDir(data).filePath(QStringLiteral("shortcuts"));
    QDir(folder).removeRecursively();
    QDir().mkpath(folder);
    const QString mine = QDir(folder).filePath(QStringLiteral("Monolist.lnk"));
    const QString other = QDir(folder).filePath(QStringLiteral("Notepad.lnk"));
    const QString notepad = QDir(qEnvironmentVariable("SystemRoot")).filePath(QStringLiteral("notepad.exe"));
    checks.check(makeShortcut(mine, exe) && makeShortcut(other, notepad), QStringLiteral("two scratch shortcuts made"));
    const QByteArray otherBefore = digest(other);

    checks.check(AppIcon::pointShortcuts({ folder }, exe, 0) == 0,
                 QStringLiteral("the default asks nothing of an installer's shortcut"));
    checks.check(AppIcon::pointShortcuts({ folder }, exe, 1) == 1, QStringLiteral("the red: one shortcut changes"));
    const auto red = iconOf(mine);
    checks.check(QFileInfo(red.first).canonicalFilePath().compare(QFileInfo(exe).canonicalFilePath(), Qt::CaseInsensitive) == 0
                         && red.second == 1,
                 QStringLiteral("it shows the executable's second icon"),
                 red.first + QStringLiteral(",") + QString::number(red.second));
    checks.check(digest(other) == otherBefore, QStringLiteral("a shortcut to another program is left exactly as it was"));
    checks.check(AppIcon::pointShortcuts({ folder }, exe, 1) == 0, QStringLiteral("asked again, nothing changes"));
    checks.check(AppIcon::pointShortcuts({ folder }, exe, 0) == 1 && iconOf(mine).second == 0,
                 QStringLiteral("back to the note"));
    checks.check(AppIcon::pointShortcuts({ folder, QDir(data).filePath(QStringLiteral("none")) }, exe, -1) == 0,
                 QStringLiteral("no choice and a missing folder: nothing happens"));
    checks.check(AppIcon::pointShortcuts({ folder }, notepad, 1) == 1
                         && iconOf(mine).second == 0,
                 QStringLiteral("another program's run points only its own shortcut"));
    QDir(folder).removeRecursively();
#endif

    return checks.finish();
}
