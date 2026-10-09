#pragma once

#include <QLocalServer>
#include <QLockFile>
#include <QObject>
#include <QString>

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
class InstanceGuard : public QObject
{
    Q_OBJECT
public:
    explicit InstanceGuard(const QString &dataDirectory, bool wake = true, QObject *parent = nullptr);

    // This is the only Monolist on its library.
    bool first() const { return m_first; }
    // A second start whose first answered; false when the lock could not be
    // judged (no rights, a lock nobody holds), and this one should run anyway.
    bool answered() const { return m_answered; }
    // From a second start: asks the first to show its window, or (`wake`
    // false) only whether it runs. True when it answered.
    bool askFirst(bool wake);

Q_SIGNALS:
    // A second start asked for the window.
    void wakeRequested();

private:
    // A second start's question, heard by the first.
    void hear(QLocalSocket *socket);

    QLockFile m_lock;
    QLocalServer m_server;
    QString m_name;
    bool m_first = false;
    bool m_answered = false;
};
