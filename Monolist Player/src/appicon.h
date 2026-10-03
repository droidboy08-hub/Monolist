#pragma once

#include <QIcon>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

class Library;

// Which picture is the app's icon: Settings > Appearance offers a choice
// (packaging/make-icon.ps1 draws them), kept in the settings as "app.icon".
//
// The running window takes it at once (the taskbar button). On Windows the
// icon Explorer shows for the app — the Start menu, the desktop, a pinned
// taskbar button, the media flyout — is its shortcut's, so the shortcuts that
// start this very monolist.exe are pointed at the chosen picture inside it
// (each choice is one of the executable's icons, src/monolist.rc.in). Only
// those: a shortcut to anything else, or to another copy of Monolist, is
// never opened for writing, and none is ever made or removed. A reinstall
// writes its shortcuts afresh with the first icon, so the launch points them
// again when the choice is another.
//
// Exposed to QML as the "AppIcon" singleton.
class AppIcon : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString current READ current NOTIFY currentChanged)
    // Each {id, name, preview}: preview is a picture for the interface.
    Q_PROPERTY(QVariantList choices READ choices CONSTANT)

public:
    explicit AppIcon(Library *library, QObject *parent = nullptr);

    QString current() const { return m_current; }
    QVariantList choices() const;

    // The choice the settings hold, on the window at once; the shortcuts a
    // few seconds later, once the window is up.
    void restore();
    Q_INVOKABLE void choose(const QString &id);

    // The icon for a choice, at every size drawn; the default's for an id
    // that is not one.
    static QIcon iconFor(const QString &id);
    // A choice's place among the executable's icons; -1 for no choice.
    static int indexOf(const QString &id);

    // Where the app's shortcuts are: the Start menu, the desktop, the pinned
    // taskbar buttons. Set otherwise only by the self-test.
    void setShortcutFolders(const QStringList &folders) { m_folders = folders; }
    QStringList shortcutFolders() const { return m_folders; }
    static QStringList defaultShortcutFolders();

    // Points every shortcut in those folders that starts `executable` at its
    // icon number `index`, and returns how many changed. Windows only (0
    // elsewhere).
    static int pointShortcuts(const QStringList &folders, const QString &executable, int index);

Q_SIGNALS:
    void currentChanged();

private:
    void pointOwnShortcuts();

    Library *m_library = nullptr;
    QString m_current;
    QStringList m_folders;
};
