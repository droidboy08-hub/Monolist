#pragma once

#include <QLocalServer>
#include <QLockFile>
#include <QObject>
#include <QString>
#include <QStringList>

#include <utility>

class QLocalSocket;

// One Monolist a library. Two running on the same data folder would each
// write the database the other reads, and a library was found damaged after
// just that (2026-10-01): pages of one table written over another's. So the
// first to start holds a lock file in the data folder; a second start on the
// same folder asks the first to bring its window forward, and ends. Another
// data folder (MONOLIST_DATA_DIR: the self-tests, a scratch library) is
// another lock, and runs alongside.
//
// A start at sign-in (--login, Startup) is made with `wake` false: it only
// asks whether the first runs, and ends, leaving that one's window where it
// is. The user opened Monolist before Windows got round to its startup apps,
// and it must not jump in front of whatever they are doing by then.
//
// It is also the door files opened with Monolist come in by, since they are
// for the one Monolist of the library: the system's (a double-click in the
// Finder, Open With, a file dropped on the Dock icon, which a Mac hands the
// running app as a QFileOpenEvent, or to the app as it starts), the command
// line's (`files`), and a second start's, which it passes to the first.
class InstanceGuard : public QObject
{
    Q_OBJECT
public:
    explicit InstanceGuard(const QString &dataDirectory, bool wake = true, const QStringList &files = {},
                           QObject *parent = nullptr);

    // This is the only Monolist on its library.
    bool first() const { return m_first; }
    // A second start whose first answered; false when the lock could not be
    // judged (no rights, a lock nobody holds), and this one should run anyway.
    bool answered() const { return m_answered; }
    // From a second start: asks the first to show its window and open
    // `files`, or (`wake` false) only whether it runs. True when it answered.
    bool askFirst(bool wake, const QStringList &files = {});

    // The files opened with Monolist since last asked, in the order they
    // came. Kept until taken: a Mac sends the ones it was started for as
    // the app starts, before anything is there to play them.
    QStringList takeFiles() { return std::exchange(m_files, {}); }

    // The files a start was given on its command line, as Open With gives
    // them on Windows: every argument a file that exists, made absolute.
    // Empty when any argument is something else (the self-tests' flags), so
    // that a file a test is given is never played.
    static QStringList filesIn(const QStringList &arguments);

Q_SIGNALS:
    // A second start asked for the window.
    void wakeRequested();
    // takeFiles() has something: once for files that came together, as
    // several selected in the Finder do.
    void filesArrived();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // A second start's question, heard by the first.
    void hear(QLocalSocket *socket);
    void addFiles(const QStringList &files);

    QLockFile m_lock;
    QLocalServer m_server;
    QString m_name;
    QStringList m_files;
    bool m_first = false;
    bool m_answered = false;
    bool m_telling = false;   // filesArrived is on its way
};
