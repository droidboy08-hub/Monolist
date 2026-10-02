#include "instanceguard.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QLocalSocket>

InstanceGuard::InstanceGuard(const QString &dataDirectory, QObject *parent)
    : QObject(parent), m_lock(QDir(dataDirectory).filePath(QStringLiteral("monolist.lock")))
{
    QDir().mkpath(dataDirectory);
    // The server's name follows the folder, however it is spelt.
    const QString folder = QFileInfo(dataDirectory).absoluteFilePath().toLower();
    m_name = QStringLiteral("monolist-")
             + QString::fromLatin1(QCryptographicHash::hash(folder.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
    // A lock left by a Monolist that ended without letting go (a crash, the
    // power) is stale at once: QLockFile asks whether its process still runs.
    m_lock.setStaleLockTime(0);
    m_first = m_lock.tryLock(300);
    if (!m_first && m_lock.error() == QLockFile::LockFailedError) {
        // Held: by a Monolist that answers, or by a lock Qt cannot judge
        // (empty, or written on another machine), which goes if nothing
        // holds the file open.
        m_answered = wakeFirst();
        if (!m_answered && m_lock.removeStaleLockFile())
            m_first = m_lock.tryLock(300);
    }
    if (!m_first)
        return;
    QLocalServer::removeServer(m_name);
    m_server.listen(m_name);
    connect(&m_server, &QLocalServer::newConnection, this, [this]() {
        while (QLocalSocket *socket = m_server.nextPendingConnection())
            socket->deleteLater();
        Q_EMIT wakeRequested();
    });
}

bool InstanceGuard::wakeFirst()
{
    QLocalSocket socket;
    socket.connectToServer(m_name);
    const bool answered = socket.waitForConnected(1500);
    socket.disconnectFromServer();
    return answered;
}
