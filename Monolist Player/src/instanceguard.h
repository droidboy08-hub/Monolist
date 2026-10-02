#pragma once

#include <QLocalServer>
#include <QLockFile>
#include <QObject>
#include <QString>

// One Monolist a library. Two running on the same data folder would each
// write the database the other reads, and a library was found damaged after
// just that (2026-10-01): pages of one table written over another's. So the
// first to start holds a lock file in the data folder; a second start on the
// same folder asks the first to bring its window forward, and ends. Another
// data folder (MONOLIST_DATA_DIR: the self-tests, a scratch library) is
// another lock, and runs alongside.
class InstanceGuard : public QObject
{
    Q_OBJECT
public:
    explicit InstanceGuard(const QString &dataDirectory, QObject *parent = nullptr);

    // This is the only Monolist on its library.
    bool first() const { return m_first; }
    // From a second start: asks the first to show its window. True when it
    // answered.
    bool wakeFirst();

Q_SIGNALS:
    // A second start asked for the window.
    void wakeRequested();

private:
    QLockFile m_lock;
    QLocalServer m_server;
    QString m_name;
    bool m_first = false;
};
