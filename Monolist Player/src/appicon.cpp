#include "appicon.h"

#include "library.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QTimer>

#include <iterator>

#ifdef Q_OS_WIN
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

namespace {

const char kSettingKey[] = "app.icon";
const char kDefault[] = "note";

struct Choice {
    const char *id;
    const char *name;
};
// In the executable's order: IDI_ICON1 is the first, and Qt's own default
// for every window, so the default comes first.
const Choice kChoices[] = {
    { "note", "Note" },
    { "note-red", "Red note" },
};
const int kSizes[] = { 16, 24, 32, 48, 64, 128, 256 };

QString picture(const QString &id, int size)
{
    return QStringLiteral(":/qt/qml/Monolist/icons/app/%1-%2.png").arg(id).arg(size);
}

} // namespace

AppIcon::AppIcon(Library *library, QObject *parent)
    : QObject(parent), m_library(library), m_current(QString::fromLatin1(kDefault)),
      m_folders(defaultShortcutFolders())
{
}

QVariantList AppIcon::choices() const
{
    QVariantList list;
    for (const Choice &choice : kChoices) {
        const QString id = QString::fromLatin1(choice.id);
        list.append(QVariantMap{
            { QStringLiteral("id"), id },
            { QStringLiteral("name"), QString::fromLatin1(choice.name) },
            { QStringLiteral("preview"), QStringLiteral("qrc") + picture(id, 128) },
        });
    }
    return list;
}

int AppIcon::indexOf(const QString &id)
{
    for (int i = 0; i < int(std::size(kChoices)); ++i) {
        if (id == QLatin1String(kChoices[i].id))
            return i;
    }
    return -1;
}

QIcon AppIcon::iconFor(const QString &id)
{
    const QString known = indexOf(id) >= 0 ? id : QString::fromLatin1(kDefault);
    QIcon icon;
    for (int size : kSizes)
        icon.addFile(picture(known, size), QSize(size, size));
    return icon;
}

void AppIcon::restore()
{
    const QString saved = m_library ? m_library->settingValue(QString::fromLatin1(kSettingKey)) : QString();
    if (indexOf(saved) >= 0)
        m_current = saved;
#ifndef Q_OS_MACOS
    // The Mac's icon is the bundle's, drawn on Apple's grid (macos/), and
    // the Dock would show this one larger than every other app's.
    QGuiApplication::setWindowIcon(iconFor(m_current));
#endif
    // A few seconds on: nothing about it is worth a moment of the launch.
    QTimer::singleShot(4000, this, &AppIcon::pointOwnShortcuts);
}

void AppIcon::choose(const QString &id)
{
    if (indexOf(id) < 0 || id == m_current)
        return;
    m_current = id;
    if (m_library)
        m_library->setSetting(QString::fromLatin1(kSettingKey), id);
#ifndef Q_OS_MACOS
    QGuiApplication::setWindowIcon(iconFor(id));
#endif
    pointOwnShortcuts();
    Q_EMIT currentChanged();
}

void AppIcon::pointOwnShortcuts()
{
    const int changed = pointShortcuts(m_folders, QCoreApplication::applicationFilePath(), indexOf(m_current));
    if (changed > 0)
        qInfo("app icon: %d shortcut(s) now show \"%s\"", changed, qPrintable(m_current));
}

QStringList AppIcon::defaultShortcutFolders()
{
    QStringList folders;
#ifdef Q_OS_WIN
    // The installer's (packaging/monolist.iss): the Start menu's Programs and
    // the desktop, for this user; and where Windows keeps pinned taskbar
    // buttons. The all-users ones are left alone: they need an administrator.
    folders << QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation)
            << QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    const QString roaming = qEnvironmentVariable("APPDATA");
    if (!roaming.isEmpty())
        folders << QDir(roaming).filePath(QStringLiteral("Microsoft/Internet Explorer/Quick Launch/User Pinned/TaskBar"));
#endif
    folders.removeAll(QString());
    return folders;
}

int AppIcon::pointShortcuts(const QStringList &folders, const QString &executable, int index)
{
#ifdef Q_OS_WIN
    if (index < 0)
        return 0;
    const QString ours = QFileInfo(executable).canonicalFilePath();
    if (ours.isEmpty())
        return 0;
    const std::wstring icon = QDir::toNativeSeparators(ours).toStdWString();

    // Qt's thread is already the single-threaded apartment the shell's
    // objects want; this only balances itself where it was not.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int changed = 0;
    for (const QString &folder : folders) {
        const QFileInfoList shortcuts = QDir(folder).entryInfoList({ QStringLiteral("*.lnk") }, QDir::Files);
        for (const QFileInfo &shortcut : shortcuts) {
            IShellLinkW *link = nullptr;
            if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                        reinterpret_cast<void **>(&link))))
                continue;
            IPersistFile *file = nullptr;
            if (FAILED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&file)))) {
                link->Release();
                continue;
            }
            const std::wstring path = QDir::toNativeSeparators(shortcut.absoluteFilePath()).toStdWString();
            // Read first, read-only: a shortcut to anything but this
            // executable is never opened for writing.
            wchar_t target[MAX_PATH] = {};
            wchar_t current[MAX_PATH] = {};
            int currentIndex = 0;
            const bool read = SUCCEEDED(file->Load(path.c_str(), STGM_READ))
                              && SUCCEEDED(link->GetPath(target, MAX_PATH, nullptr, 0));
            const bool mine = read
                              && QFileInfo(QString::fromWCharArray(target)).canonicalFilePath()
                                         .compare(ours, Qt::CaseInsensitive) == 0;
            if (mine && SUCCEEDED(link->GetIconLocation(current, MAX_PATH, &currentIndex))) {
                // No icon of its own is the target's first: the default.
                const QString was = QString::fromWCharArray(current);
                const bool already = was.isEmpty()
                        ? index == 0
                        : (QFileInfo(was).canonicalFilePath().compare(ours, Qt::CaseInsensitive) == 0
                           && currentIndex == index);
                if (!already && SUCCEEDED(link->SetIconLocation(icon.c_str(), index))
                    && SUCCEEDED(file->Save(path.c_str(), TRUE))) {
                    ++changed;
                    // Explorer redraws that one shortcut, wherever it shows.
                    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, path.c_str(), nullptr);
                }
            }
            file->Release();
            link->Release();
        }
    }
    if (SUCCEEDED(com))
        CoUninitialize();
    return changed;
#else
    Q_UNUSED(folders);
    Q_UNUSED(executable);
    Q_UNUSED(index);
    return 0;
#endif
}
