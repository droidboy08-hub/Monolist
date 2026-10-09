#include "instanceguard.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QLocalSocket>
#include <QTimer>

#include <memory>

namespace {

// What a second start that only asks whether the first runs says (a start
// at sign-in): one byte, so it is never heard in halves. Saying nothing, as
// every Monolist before this one does, asks for the window.
const char kQuiet = 'q';

} // namespace

InstanceGuard::InstanceGuard(const QString &dataDirectory, bool wake, QObject *parent)
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
        m_answered = askFirst(wake);
        if (!m_answered && m_lock.removeStaleLockFile())
            m_first = m_lock.tryLock(300);
    }
    if (!m_first)
        return;
    QLocalServer::removeServer(m_name);
    m_server.listen(m_name);
    connect(&m_server, &QLocalServer::newConnection, this, [this]() {
        while (QLocalSocket *socket = m_server.nextPendingConnection())
            hear(socket);
    });
}

bool InstanceGuard::askFirst(bool wake)
{
    QLocalSocket socket;
    socket.connectToServer(m_name);
    if (!socket.waitForConnected(1500))
        return false;
    if (!wake) {
        // Said, and held until the first has heard it and let go, so that
        // it is never taken for a second start that wants the window (one
        // that lets go having said nothing). A first that is busy a while
        // is waited for.
        socket.write(&kQuiet, 1);
        socket.waitForBytesWritten(1500);
        socket.waitForDisconnected(3000);
    }
    socket.disconnectFromServer();
    return true;
}

// The window brought forward, unless the second start said it only asks
// whether this one runs. Decided once: as soon as it has said so, when it
// lets go having said nothing, or after a second in any case.
void InstanceGuard::hear(QLocalSocket *socket)
{
    const auto heard = std::make_shared<bool>(false);
    const auto decide = [this, socket, heard]() {
        if (*heard)
            return;
        *heard = true;
        const bool quiet = socket->read(1) == QByteArray(1, kQuiet);
        socket->disconnectFromServer();
        socket->deleteLater();
        if (!quiet)
            Q_EMIT wakeRequested();
    };
    connect(socket, &QLocalSocket::readyRead, this, decide);
    connect(socket, &QLocalSocket::disconnected, this, decide);
    QTimer::singleShot(1000, socket, decide);
    if (socket->bytesAvailable() > 0 || socket->state() != QLocalSocket::ConnectedState)
        decide();
}
