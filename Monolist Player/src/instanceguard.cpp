#include "instanceguard.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QLocalSocket>
#include <QTimer>

#include <memory>

namespace {

// What a second start that only asks whether the first runs says (a start
// at sign-in): one byte, so it is never heard in halves. Saying nothing, as
// every Monolist before this one does, asks for the window.
const char kQuiet = 'q';
// What a second start given files says first: then their paths, each ended
// by a nul, which no path holds, and it lets go once it has said them all.
// It asks for the window too. A Monolist from before this hears a byte that
// is not kQuiet, and brings its window forward.
const char kFiles = 'f';

} // namespace

InstanceGuard::InstanceGuard(const QString &dataDirectory, bool wake, const QStringList &files, QObject *parent)
    : QObject(parent), m_lock(QDir(dataDirectory).filePath(QStringLiteral("monolist.lock"))), m_files(files)
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
        m_answered = askFirst(wake, files);
        if (!m_answered && m_lock.removeStaleLockFile())
            m_first = m_lock.tryLock(300);
    }
    if (!m_first)
        return;
    // The system's own way of opening a file with the app: sent to the
    // application object, which every event passes through here first.
    QCoreApplication::instance()->installEventFilter(this);
    QLocalServer::removeServer(m_name);
    m_server.listen(m_name);
    connect(&m_server, &QLocalServer::newConnection, this, [this]() {
        while (QLocalSocket *socket = m_server.nextPendingConnection())
            hear(socket);
    });
}

bool InstanceGuard::askFirst(bool wake, const QStringList &files)
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
    } else if (!files.isEmpty()) {
        QByteArray said(1, kFiles);
        for (const QString &file : files)
            said += file.toUtf8() + '\0';
        socket.write(said);
        socket.waitForBytesWritten(3000);
    }
    socket.disconnectFromServer();
    return true;
}

// The window brought forward, unless the second start said it only asks
// whether this one runs. Decided once: as soon as it has said so, when it
// lets go having said nothing, or after a second in any case. Files are
// read whole, once it has let go of them (or after a few seconds).
void InstanceGuard::hear(QLocalSocket *socket)
{
    const auto heard = std::make_shared<bool>(false);
    const auto decide = [this, socket, heard](bool letGo) {
        if (*heard)
            return;
        if (!letGo && socket->peek(1) == QByteArray(1, kFiles))
            return;
        *heard = true;
        const QByteArray said = socket->readAll();
        socket->disconnectFromServer();
        socket->deleteLater();
        if (said.startsWith(kQuiet))
            return;
        if (said.startsWith(kFiles)) {
            QStringList files;
            for (const QByteArray &path : said.mid(1).split('\0')) {
                if (!path.isEmpty())
                    files.append(QString::fromUtf8(path));
            }
            addFiles(files);
        }
        Q_EMIT wakeRequested();
    };
    connect(socket, &QLocalSocket::readyRead, this, [decide]() { decide(false); });
    connect(socket, &QLocalSocket::disconnected, this, [decide]() { decide(true); });
    QTimer::singleShot(1000, socket, [decide]() { decide(false); });
    QTimer::singleShot(5000, socket, [decide]() { decide(true); });
    if (socket->state() != QLocalSocket::ConnectedState)
        decide(true);
    else if (socket->bytesAvailable() > 0)
        decide(false);
}

bool InstanceGuard::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::FileOpen) {
        // Not a file of this computer (a URL the app was opened with) is
        // nothing to play, and is left to whatever else would take it.
        const QString file = static_cast<QFileOpenEvent *>(event)->file();
        if (!file.isEmpty()) {
            addFiles({ file });
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}

// Said once for files that come one after another, as a Mac sends several
// opened together: each its own event, all in one turn of the event loop.
void InstanceGuard::addFiles(const QStringList &files)
{
    if (files.isEmpty())
        return;
    m_files += files;
    if (m_telling)
        return;
    m_telling = true;
    QTimer::singleShot(0, this, [this]() {
        m_telling = false;
        if (!m_files.isEmpty())
            Q_EMIT filesArrived();
    });
}

QStringList InstanceGuard::filesIn(const QStringList &arguments)
{
    QStringList files;
    for (const QString &argument : arguments.mid(1)) {
        const QFileInfo file(argument);
        if (argument.startsWith(QLatin1Char('-')) || !file.isFile())
            return {};
        files.append(file.absoluteFilePath());
    }
    return files;
}
