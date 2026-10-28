#include "appupdaterselftest.h"

#include "appinfo.h"
#include "appupdater.h"
#ifdef Q_OS_MACOS
#include "macos/appupdater_mac.h"
#endif

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QProcess>
#include <QRandomGenerator>
#include <QSysInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <functional>
#include <memory>

#ifdef Q_OS_MACOS
#include <csignal>
#include <sys/stat.h>
#include <unistd.h>
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
        qWarning("update-test: %s  %s", ok ? "ok  " : "FAIL", qPrintable(line));
        return ok;
    }
    void note(const QString &text) { qWarning("update-test: %s", qPrintable(text)); }
    int finish()
    {
        qWarning("update-test: %d checks, %d failed", m_count, m_failed);
        return m_failed;
    }

private:
    int m_count = 0;
    int m_failed = 0;
};

bool waitUntil(const std::function<bool()> &done, int timeoutMs)
{
    if (done())
        return true;
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(5);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
        if (done())
            loop.quit();
    });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();
    return done();
}

void pause(int ms)
{
    waitUntil([]() { return false; }, ms);
}

struct Ran {
    int code = -1;
    QByteArray out;
    QByteArray err;
};

Ran runCommand(const QString &program, const QStringList &arguments, int timeoutMs = 120000)
{
    Ran ran;
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted(10000))
        return ran;
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(2000);
        return ran;
    }
    ran.code = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    ran.out = process.readAllStandardOutput();
    ran.err = process.readAllStandardError();
    return ran;
}

QString sha256Of(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(data) == data.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// Every file whose name ends so, anywhere under `folder`.
QStringList filesEnding(const QString &folder, const QString &end)
{
    QStringList found;
    const QFileInfoList entries = QDir(folder).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot
                                                             | QDir::Hidden);
    for (const QFileInfo &entry : entries) {
        if (entry.isDir() && !entry.isSymLink())
            found << filesEnding(entry.filePath(), end);
        else if (entry.fileName().endsWith(end))
            found << entry.filePath();
    }
    return found;
}

// — the stand-in —

// GitHub, as far as the update uses it: a path answered as `routes` says,
// each request kept. Answers can come a slice at a time, slowly (for a
// cancel), or stop short of what they promised (a cut). Every answer sets a
// cookie, so that one sent back would show.
class StandIn : public QObject
{
public:
    struct Request {
        QByteArray method;
        QByteArray path;
        QList<QPair<QByteArray, QByteArray>> headers;   // names in lower case
        bool has(const QByteArray &name) const
        {
            for (const auto &pair : headers) {
                if (pair.first == name)
                    return true;
            }
            return false;
        }
    };
    struct Answer {
        int status = 200;
        QByteArray body;
        QByteArray type = "application/octet-stream";
        QList<QByteArray> extra;
        int cutShort = 0;   // bytes promised and never sent
        int chunk = 0;      // with delayMs: this many bytes at a time
        int delayMs = 0;
    };

    QHash<QByteArray, Answer> routes;
    QList<Request> requests;
    int abandoned = 0;      // answers the client hung up on part way

    bool listen()
    {
        connect(&m_server, &QTcpServer::newConnection, this, &StandIn::accept);
        return m_server.listen(QHostAddress::LocalHost, 0);
    }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }
    QString url(const QByteArray &path) const { return base() + QString::fromLatin1(path); }
    int count(const QByteArray &path) const
    {
        int n = 0;
        for (const Request &request : requests)
            n += request.path == path ? 1 : 0;
        return n;
    }
    bool anyCookie() const
    {
        for (const Request &request : requests) {
            if (request.has("cookie"))
                return true;
        }
        return false;
    }

private:
    void accept()
    {
        while (QTcpSocket *socket = m_server.nextPendingConnection()) {
            auto buffer = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer]() {
                *buffer += socket->readAll();
                handle(socket, *buffer);
            });
        }
    }

    void handle(QTcpSocket *socket, QByteArray &buffer)
    {
        const int end = int(buffer.indexOf("\r\n\r\n"));
        if (end < 0)
            return;
        Request request;
        const QList<QByteArray> lines = buffer.left(end).split('\n');
        const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
        request.method = first.value(0);
        request.path = first.value(1);
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            const int colon = int(line.indexOf(':'));
            if (colon > 0)
                request.headers.append({ line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed() });
        }
        buffer.clear();
        requests.append(request);

        Answer answer;
        if (routes.contains(request.path)) {
            answer = routes.value(request.path);
        } else {
            answer.status = 404;
            answer.body = "not here";
            answer.type = "text/plain";
        }
        QByteArray head = "HTTP/1.1 " + QByteArray::number(answer.status) + " Stand-in\r\n"
                          "Content-Type: " + answer.type + "\r\n"
                          "Content-Length: " + QByteArray::number(answer.body.size() + answer.cutShort) + "\r\n"
                          "Set-Cookie: UPDATETEST=" + QByteArray::number(requests.size()) + "; Path=/\r\n"
                          "Connection: close\r\n";
        for (const QByteArray &line : std::as_const(answer.extra))
            head += line + "\r\n";
        head += "\r\n";
        socket->write(head);
        if (answer.delayMs <= 0 || answer.chunk <= 0) {
            socket->write(answer.body);
            socket->disconnectFromHost();
            return;
        }
        stream(QPointer<QTcpSocket>(socket), answer.body, 0, answer.chunk, answer.delayMs);
    }

    void stream(QPointer<QTcpSocket> socket, const QByteArray &body, int offset, int chunk, int delayMs)
    {
        if (!socket || socket->state() != QAbstractSocket::ConnectedState) {
            ++abandoned;
            return;
        }
        socket->write(body.mid(offset, chunk));
        const int next = offset + chunk;
        if (next >= body.size()) {
            socket->disconnectFromHost();
            return;
        }
        QTimer::singleShot(delayMs, this, [this, socket, body, next, chunk, delayMs]() {
            stream(socket, body, next, chunk, delayMs);
        });
    }

    QTcpServer m_server;
};

UpdateAsset asset(const QString &name, const QString &url, qint64 size = 0)
{
    UpdateAsset made;
    made.name = name;
    made.url = QUrl(url);
    made.size = size;
    return made;
}

UpdateRelease release(const QString &version, const QList<UpdateAsset> &assets)
{
    UpdateRelease made;
    made.version = version;
    made.page = QUrl(QStringLiteral("https://github.com/droidboy08-hub/Monolist/releases/tag/v") + version);
    made.assets = assets;
    return made;
}

QByteArray feedJson(const UpdateRelease &release)
{
    QJsonArray assets;
    for (const UpdateAsset &file : release.assets) {
        assets.append(QJsonObject{ { QStringLiteral("name"), file.name },
                                   { QStringLiteral("browser_download_url"), file.url.toString() },
                                   { QStringLiteral("size"), double(file.size) } });
    }
    return QJsonDocument(QJsonObject{ { QStringLiteral("tag_name"), QStringLiteral("v") + release.version },
                                      { QStringLiteral("html_url"), release.page.toString() },
                                      { QStringLiteral("assets"), assets } })
        .toJson();
}

bool settled(const AppUpdater &updater)
{
    return updater.phase() == AppUpdater::Ready || updater.phase() == AppUpdater::Failed
           || updater.phase() == AppUpdater::Manual;
}

QString phaseName(AppUpdater::Phase phase)
{
    switch (phase) {
    case AppUpdater::Idle: return QStringLiteral("idle");
    case AppUpdater::Downloading: return QStringLiteral("downloading");
    case AppUpdater::Preparing: return QStringLiteral("preparing");
    case AppUpdater::Ready: return QStringLiteral("ready");
    case AppUpdater::Manual: return QStringLiteral("manual");
    case AppUpdater::Failed: return QStringLiteral("failed");
    }
    return QStringLiteral("?");
}

QString describe(const AppUpdater &updater)
{
    return phaseName(updater.phase()) + QStringLiteral(": ") + updater.message();
}

const QString kVersion = QStringLiteral("99.0.1");

// — the rules —

void checkRules(Checks &c)
{
    const QString arch = QSysInfo::currentCpuArchitecture();
    const QString base = QStringLiteral("https://github.com/droidboy08-hub/Monolist/releases/download/v99.0.1/");
    const QStringList names = {
        QStringLiteral("Monolist-99.0.1-setup-win64.exe"),
        QStringLiteral("Monolist-99.0.1-win64.zip"),
        QStringLiteral("Monolist-99.0.1-macos-arm64.dmg"),
        QStringLiteral("Monolist-99.0.1-macos-arm64-unsigned.dmg"),
    };
    QList<UpdateAsset> assets;
    for (const QString &name : names) {
        assets << asset(name, base + name, 1000);
        assets << asset(name + QStringLiteral(".sha256"), base + name + QStringLiteral(".sha256"), 80);
    }
    const UpdateRelease full = release(kVersion, assets);

    const UpdateRelease parsed = UpdateRules::parseRelease(feedJson(full));
    c.check(parsed.version == kVersion, QStringLiteral("the feed's tag is read without its v"), parsed.version);
    c.check(parsed.assets.size() == 8 && parsed.assets.first().url.toString() == base + names.first()
                && parsed.assets.first().size == 1000,
            QStringLiteral("the feed's files are read with their links and sizes"));
    c.check(parsed.page == full.page, QStringLiteral("the feed's release page is read"));
    c.check(!UpdateRules::parseRelease("{}").isValid() && !UpdateRules::parseRelease("not json").isValid(),
            QStringLiteral("a feed with no tag is no release"));

    UpdateTarget mac;
    mac.kind = UpdateTarget::MacBundle;
    mac.developerId = true;
    UpdateRules::Choice choice = UpdateRules::choose(full, mac, QStringLiteral("arm64"));
    c.check(choice.file.name == names.at(2) && choice.checksum.name == names.at(2) + QStringLiteral(".sha256"),
            QStringLiteral("a Developer ID Mac takes the signed disk image, and its checksum"), choice.file.name);
    mac.developerId = false;
    choice = UpdateRules::choose(full, mac, QStringLiteral("arm64"));
    c.check(choice.file.name == names.at(3), QStringLiteral("an ad hoc Mac takes the unsigned disk image"),
            choice.file.name);
    UpdateRelease signedOnly = full;
    signedOnly.assets.removeIf([](const UpdateAsset &a) { return a.name.contains(QLatin1String("unsigned")); });
    choice = UpdateRules::choose(signedOnly, mac, QStringLiteral("arm64"));
    c.check(choice.file.name == names.at(2),
            QStringLiteral("an ad hoc Mac takes a release's only disk image when it has no unsigned one"),
            choice.file.name);
    mac.developerId = true;
    UpdateRelease unsignedOnly = full;
    unsignedOnly.assets.removeIf([](const UpdateAsset &a) { return a.name.contains(QLatin1String("arm64.dmg")); });
    choice = UpdateRules::choose(unsignedOnly, mac, QStringLiteral("arm64"));
    c.check(choice.file.isNull() && !choice.missing.isEmpty(),
            QStringLiteral("a Developer ID Mac never takes the unsigned image"), choice.missing);
    choice = UpdateRules::choose(full, mac, QStringLiteral("x86_64"));
    c.check(choice.file.isNull(), QStringLiteral("a Mac of another processor finds nothing for it"));

    UpdateTarget windows;
    windows.kind = UpdateTarget::WindowsSetup;
    choice = UpdateRules::choose(full, windows, arch);
    c.check(choice.file.name == names.at(0), QStringLiteral("a copy the setup installed takes the setup"),
            choice.file.name);
    windows.kind = UpdateTarget::WindowsPortable;
    choice = UpdateRules::choose(full, windows, arch);
    c.check(choice.file.name == names.at(1), QStringLiteral("a portable copy takes the zip"), choice.file.name);
    windows.kind = UpdateTarget::Unsupported;
    choice = UpdateRules::choose(full, windows, arch);
    c.check(choice.file.isNull() && !choice.missing.isEmpty(),
            QStringLiteral("a copy that cannot update itself takes nothing"));

    UpdateRelease noSums = full;
    noSums.assets.removeIf([](const UpdateAsset &a) { return a.name.endsWith(QLatin1String(".sha256")); });
    windows.kind = UpdateTarget::WindowsSetup;
    choice = UpdateRules::choose(noSums, windows, arch);
    c.check(choice.file.isNull() && choice.missing.contains(QLatin1String("checksum")),
            QStringLiteral("a file with no checksum beside it is not taken"), choice.missing);
    const UpdateRelease oddNames = release(kVersion, {
        asset(QStringLiteral("Monolist-99.0.0-setup-win64.exe"), base + QStringLiteral("a")),
        asset(QStringLiteral("Monolist-99.0.0-setup-win64.exe.sha256"), base + QStringLiteral("b")),
        asset(QStringLiteral("../Monolist-99.0.1-setup-win64.exe"), base + QStringLiteral("c")),
        asset(QStringLiteral("../Monolist-99.0.1-setup-win64.exe.sha256"), base + QStringLiteral("d")),
        asset(QStringLiteral("Monolist-99.0.1-win64-dirty.zip"), base + QStringLiteral("e")),
    });
    c.check(UpdateRules::choose(oddNames, windows, arch).file.isNull(),
            QStringLiteral("a file of another version, or with a folder in its name, is never taken"));
    windows.kind = UpdateTarget::WindowsPortable;
    c.check(UpdateRules::choose(oddNames, windows, arch).file.isNull(),
            QStringLiteral("a -dirty zip is never taken"));

    // Where a download may come from: the project's release files on
    // github.com, and GitHub's two download hosts as places it redirects to.
    const QUrl github(QStringLiteral("https://api.github.com/repos/droidboy08-hub/Monolist/releases/latest"));
    const QUrl loopback(QStringLiteral("http://127.0.0.1:8123/feed"));
    struct Case {
        const char *url;
        const QUrl *feed;
        bool redirect;
        bool allowed;
    };
    const Case cases[] = {
        { "https://github.com/droidboy08-hub/Monolist/releases/download/v1/x.dmg", &github, false, true },
        { "HTTPS://GitHub.com/droidboy08-hub/monolist/releases/download/v1/x.dmg", &github, false, true },
        { "https://github.com/droidboy08-hub/Monolist/releases/download/v1/x.dmg", &github, true, true },
        { "https://objects.githubusercontent.com/github-production-release-asset/1/2", &github, true, true },
        { "https://release-assets.githubusercontent.com/github-production-release-asset/1/2", &github, true, true },
        { "https://objects.githubusercontent.com/github-production-release-asset/1/2", &github, false, false },
        { "https://raw.githubusercontent.com/droidboy08-hub/Monolist/main/x.dmg", &github, true, false },
        { "https://gist.githubusercontent.com/someone/1/raw/x.dmg", &github, true, false },
        { "https://user-images.githubusercontent.com/1/x.dmg", &github, true, false },
        { "https://github.com/someone/else/releases/download/v1/x.dmg", &github, false, false },
        { "https://github.com/someone/else/releases/download/v1/x.dmg", &github, true, false },
        { "https://github.com/droidboy08-hub/Monolist/releases/download/../../../someone/x.dmg", &github, false, false },
        { "https://github.com/droidboy08-hub/Monolist/raw/main/x.dmg", &github, false, false },
        { "https://github.com/x", &github, false, false },
        { "http://github.com/droidboy08-hub/Monolist/releases/download/v1/x.dmg", &github, false, false },
        { "https://github.com:8443/droidboy08-hub/Monolist/releases/download/v1/x.dmg", &github, false, false },
        { "https://user:secret@github.com/droidboy08-hub/Monolist/releases/download/v1/x.dmg", &github, false, false },
        { "https://example.com/x.dmg", &github, false, false },
        { "https://githubusercontent.com.example.com/x", &github, true, false },
        { "https://evilgithubusercontent.com/x", &github, true, false },
        { "https://github.com.example.com/droidboy08-hub/Monolist/releases/download/v1/x", &github, false, false },
        { "ftp://github.com/droidboy08-hub/Monolist/releases/download/v1/x", &github, false, false },
        { "http://127.0.0.1:8123/files/x", &github, false, false },
        { "http://127.0.0.1:8123/files/x", &loopback, false, true },
        { "http://127.0.0.1:8123/files/x", &loopback, true, true },
        { "http://localhost:9/x", &loopback, false, true },
        { "http://[::1]:9/x", &loopback, false, true },
        { "http://example.com/x", &loopback, false, false },
        { "https://github.com/droidboy08-hub/Monolist/releases/download/v1/x", &loopback, false, true },
    };
    for (const Case &test : cases) {
        const bool allowed = UpdateRules::allowedSource(QUrl(QString::fromLatin1(test.url)), *test.feed,
                                                        test.redirect);
        c.check(allowed == test.allowed,
                QStringLiteral("%1 %2 (feed %3, %4)")
                    .arg(QString::fromLatin1(test.url),
                         test.allowed ? QStringLiteral("allowed") : QStringLiteral("refused"),
                         test.feed == &github ? QStringLiteral("GitHub") : QStringLiteral("loopback"),
                         test.redirect ? QStringLiteral("as a redirect") : QStringLiteral("as the link")));
    }
    // This computer's stand-in only for a scratch library: a feed setting
    // left behind in a real one never opens it.
    {
        const QByteArray scratch = qgetenv("MONOLIST_DATA_DIR");
        qunsetenv("MONOLIST_DATA_DIR");
        const bool allowed = UpdateRules::allowedSource(QUrl(QStringLiteral("http://127.0.0.1:8123/files/x")),
                                                        loopback);
        qputenv("MONOLIST_DATA_DIR", scratch);
        c.check(!allowed, QStringLiteral("a stand-in on this computer is refused without a scratch library"));
    }

    // The release page: GitHub's, the project's, or its list of releases.
    const QString releases = QStringLiteral("https://github.com/droidboy08-hub/Monolist/releases");
    c.check(UpdateRules::safePage(QUrl(QStringLiteral("https://github.com/droidboy08-hub/Monolist/releases/tag/v1")))
                    .toString() == QStringLiteral("https://github.com/droidboy08-hub/Monolist/releases/tag/v1")
                && UpdateRules::safePage(QUrl(QStringLiteral("https://example.com/x"))).toString() == releases
                && UpdateRules::safePage(QUrl(QStringLiteral("javascript:alert(1)"))).toString() == releases
                && UpdateRules::safePage(QUrl(QStringLiteral("https://github.com/someone/else"))).toString()
                       == releases
                && UpdateRules::safePage(QUrl(QStringLiteral("http://github.com/droidboy08-hub/Monolist/x")))
                           .toString() == releases,
            QStringLiteral("the release page is only ever the project's on GitHub"));
    UpdateRelease oddPage = full;
    oddPage.page = QUrl(QStringLiteral("file:///etc/passwd"));
    c.check(UpdateRules::parseRelease(feedJson(oddPage)).page.toString() == releases,
            QStringLiteral("a feed's release page elsewhere is replaced by the list of releases"));

    // Links as they may be written down, and how much may arrive.
    c.check(UpdateRules::withoutQuery(QStringLiteral(
                "Error transferring https://release-assets.githubusercontent.com/a/b?sp=r&sig=SECRET&jwt=x.y.z - "
                "server replied: Forbidden"))
                == QStringLiteral("Error transferring https://release-assets.githubusercontent.com/a/b - server "
                                  "replied: Forbidden"),
            QStringLiteral("a link's signed query is left out of what is logged"));
    c.check(UpdateRules::sizeLimit(1000) == 1000 + 1024 * 1024
                && UpdateRules::sizeLimit(0) == qint64(1024) * 1024 * 1024,
            QStringLiteral("a download may be a little over what the feed says, or 1 GB when it says nothing"));

    // Checksum files.
    const QString hex = QStringLiteral("ab12").repeated(16);
    c.check(UpdateRules::sha256From((hex + QStringLiteral("  Monolist-1.0.1-win64.zip\r\n")).toUtf8(),
                                    QStringLiteral("Monolist-1.0.1-win64.zip")) == hex,
            QStringLiteral("a checksum file's hash is read (CRLF and all)"));
    c.check(UpdateRules::sha256From((hex.toUpper() + QStringLiteral("  x")).toUtf8(), QStringLiteral("y")) == hex,
            QStringLiteral("the only line's hash is read, in lower case"));
    c.check(UpdateRules::sha256From(("0000  a\n" + hex + "  b\n").toUtf8(), QStringLiteral("b")) == hex,
            QStringLiteral("the hash on the line naming the file is read"));
    c.check(UpdateRules::sha256From(QByteArray("not a checksum"), QStringLiteral("x")).isEmpty()
                && UpdateRules::sha256From((hex + QStringLiteral("ff  x")).toUtf8(), QStringLiteral("x")).isEmpty(),
            QStringLiteral("no hash, or a longer one, reads as none"));
    c.check(UpdateRules::versionInName(QStringLiteral("Monolist-0.1.191-macos-arm64-unsigned.dmg.sha256"))
                    == QLatin1String("0.1.191")
                && UpdateRules::versionInName(QStringLiteral("Monolist-0.1.191-win64.zip.part"))
                       == QLatin1String("0.1.191")
                && UpdateRules::versionInName(QStringLiteral("notes.txt")).isEmpty(),
            QStringLiteral("a release file's version is read from its name"));

    // Windows' folders, as text.
    c.check(WinUpdate::sameFolder(QStringLiteral("C:\\Users\\A\\AppData\\Local\\Programs\\Monolist\\"),
                                  QStringLiteral("c:/users/a/appdata/local/programs/monolist")),
            QStringLiteral("the setup's install folder matches this one whatever its case and slashes"));
    c.check(!WinUpdate::sameFolder(QStringLiteral("C:\\Programs\\Monolist"), QStringLiteral("C:/Programs/Monolist2"))
                && !WinUpdate::sameFolder(QString(), QStringLiteral("C:/x")),
            QStringLiteral("another folder, or none, does not"));
}

void checkWindowsFolders(Checks &c, const QString &scratch)
{
    const QString package = QDir(scratch).filePath(QStringLiteral("folders/Monolist"));
    writeFile(package + QStringLiteral("/LICENSES/Monolist-MIT.txt"), "MIT");
    writeFile(package + QStringLiteral("/monolist.exe"), "MZ");
    UpdateTarget target = WinUpdate::describeFolder(package, QString(), QString());
    c.check(target.kind == UpdateTarget::WindowsPortable && target.blocker.isEmpty(),
            QStringLiteral("a package's folder, not installed, is a portable copy"), target.blocker);
    target = WinUpdate::describeFolder(package, QDir::toNativeSeparators(package) + QLatin1Char('\\'), QString());
    c.check(target.kind == UpdateTarget::WindowsSetup && !target.allUsers,
            QStringLiteral("the folder this user's uninstall key names is the setup's"));
    target = WinUpdate::describeFolder(package, QStringLiteral("C:\\Elsewhere"), package);
    c.check(target.kind == UpdateTarget::WindowsSetup && target.allUsers,
            QStringLiteral("the folder everyone's uninstall key names is the setup's, for all users"));
    // The user's own folders inside it: never swapped.
    target = WinUpdate::describeFolder(package, QString(), QString(),
                                       { QStringLiteral("C:/Users/A/Music/Monolist"),
                                         QDir(package).filePath(QStringLiteral("Downloads")) });
    c.check(target.kind == UpdateTarget::WindowsPortable && target.blocker.contains(QLatin1String("your downloads")),
            QStringLiteral("a portable copy whose folder holds the downloads is not swapped"), target.blocker);
    target = WinUpdate::describeFolder(package, QString(), QString(), { package });
    c.check(!target.blocker.isEmpty(), QStringLiteral("nor one that is the library's folder itself"));
    target = WinUpdate::describeFolder(package, QString(), QString(),
                                       { package + QStringLiteral("2/library"), QStringLiteral("C:/Elsewhere") });
    c.check(target.blocker.isEmpty(), QStringLiteral("folders beside it, or elsewhere, do not count"),
            target.blocker);
    c.check(WinUpdate::isWithin(QStringLiteral("C:\\Apps\\Monolist\\music"), QStringLiteral("c:/apps/monolist/"))
                && WinUpdate::isWithin(QStringLiteral("C:/Apps/Monolist"), QStringLiteral("C:/Apps/Monolist"))
                && !WinUpdate::isWithin(QStringLiteral("C:/Apps/Monolist2"), QStringLiteral("C:/Apps/Monolist"))
                && !WinUpdate::isWithin(QString(), QStringLiteral("C:/Apps")),
            QStringLiteral("inside a folder, whatever the case and slashes; not one beside it"));
    writeFile(package + QStringLiteral("/unins000.exe"), "MZ");
    target = WinUpdate::describeFolder(package, QStringLiteral("C:\\Elsewhere"), QString());
    c.check(target.kind == UpdateTarget::Unsupported,
            QStringLiteral("a folder the setup made, that no uninstall key names, is never taken for a portable one"));
    QFile::remove(package + QStringLiteral("/unins000.exe"));
    writeFile(package + QStringLiteral("/CMakeCache.txt"), "CMAKE");
    target = WinUpdate::describeFolder(package, QString(), QString());
    c.check(target.kind == UpdateTarget::Unsupported,
            QStringLiteral("a build folder is never taken for a portable copy"));
    const QString bare = QDir(scratch).filePath(QStringLiteral("folders/Other"));
    writeFile(bare + QStringLiteral("/monolist.exe"), "MZ");
    c.check(WinUpdate::describeFolder(bare, QString(), QString()).kind == UpdateTarget::Unsupported,
            QStringLiteral("nor is a folder that is not a package"));
}

// — downloads, on the stand-in —

struct Payload {
    QByteArray data;
    QString sha;
};

Payload payload(int bytes, quint32 seed)
{
    Payload made;
    QRandomGenerator random(seed);
    made.data.resize(bytes);
    for (int i = 0; i < bytes; i += 4) {
        const quint32 word = random.generate();
        for (int j = 0; j < 4 && i + j < bytes; ++j)
            made.data[i + j] = char((word >> (8 * j)) & 0xff);
    }
    made.sha = sha256Of(made.data);
    return made;
}

void serve(StandIn &stand, const QByteArray &folder, const QString &name, const QByteArray &data,
           const QString &sha)
{
    StandIn::Answer file;
    file.body = data;
    stand.routes.insert(folder + '/' + name.toLatin1(), file);
    StandIn::Answer sum;
    sum.body = (sha + QStringLiteral("  ") + name + QStringLiteral("\r\n")).toUtf8();
    sum.type = "text/plain";
    stand.routes.insert(folder + '/' + name.toLatin1() + ".sha256", sum);
}

UpdateRelease servedRelease(const StandIn &stand, const QByteArray &folder, const QString &name, qint64 size,
                            const QByteArray &fileFolder = QByteArray())
{
    const QByteArray from = fileFolder.isEmpty() ? folder : fileFolder;
    return release(kVersion, {
        asset(name, stand.url(from + '/' + name.toLatin1()), size),
        asset(name + QStringLiteral(".sha256"), stand.url(folder + '/' + name.toLatin1() + ".sha256")),
    });
}

void checkDownloads(Checks &c, const QString &scratch, StandIn &stand, QNetworkAccessManager &network)
{
    const QString name = QStringLiteral("Monolist-%1-setup-win64.exe").arg(kVersion);
    const Payload good = payload(3 * 1024 * 1024 + 123, 7);
    const QUrl feed(stand.url("/feed"));

    UpdateTarget setup;
    setup.kind = UpdateTarget::WindowsSetup;
    setup.path = QDir(scratch).filePath(QStringLiteral("installed"));

    const auto fresh = [&](const QString &folder) {
        auto updater = std::make_unique<AppUpdater>(&network);
        updater->setTarget(setup);
        const QString work = QDir(scratch).filePath(QStringLiteral("updates-") + folder);
        QDir(work).removeRecursively();
        updater->setDirectory(work);
        return updater;
    };

    // A good download.
    serve(stand, "/good", name, good.data, good.sha);
    {
        auto updater = fresh(QStringLiteral("good"));
        QList<int> seen;
        QObject::connect(updater.get(), &AppUpdater::changed, updater.get(), [&]() {
            if (updater->phase() == AppUpdater::Downloading && updater->percent() >= 0)
                seen << updater->percent();
        });
        const int before = int(stand.requests.size());
        updater->start(servedRelease(stand, "/good", name, good.data.size()), feed);
        waitUntil([&]() { return settled(*updater); }, 30000);
        const QString file = QDir(updater->workDirectory()).filePath(name);
        c.check(updater->phase() == AppUpdater::Ready, QStringLiteral("a download that matches its checksum is ready"),
                describe(*updater));
        c.check(readFile(file) == good.data && updater->downloadedFile() == file,
                QStringLiteral("the file is kept whole, under its own name"));
        c.check(filesEnding(updater->workDirectory(), QStringLiteral(".part")).isEmpty(),
                QStringLiteral("no .part is left once it is complete"));
        c.check(!seen.isEmpty() && seen.last() == 100, QStringLiteral("its progress is told, to 100 %"),
                QStringLiteral("%1 steps").arg(seen.size()));
        c.check(int(stand.requests.size()) - before == 2 && stand.requests.at(before).path.endsWith(".sha256"),
                QStringLiteral("the checksum is fetched first, then the file"));
        c.check(updater->message().contains(kVersion) && updater->message().contains(QLatin1String("restarts")),
                QStringLiteral("ready says Monolist restarts to finish"), updater->message());

        // The same release again: the file is not fetched twice.
        const int files = stand.count("/good/" + name.toLatin1());
        updater->start(servedRelease(stand, "/good", name, good.data.size()), feed);
        waitUntil([&]() { return settled(*updater); }, 30000);
        c.check(updater->phase() == AppUpdater::Ready && stand.count("/good/" + name.toLatin1()) == files,
                QStringLiteral("a file downloaded before, and still matching, is used again"), describe(*updater));
        // Changed since: fetched afresh.
        QFile tamper(file);
        if (tamper.open(QIODevice::ReadWrite)) {
            tamper.seek(100);
            tamper.write("tampered");
            tamper.close();
        }
        updater->start(servedRelease(stand, "/good", name, good.data.size()), feed);
        waitUntil([&]() { return settled(*updater); }, 30000);
        c.check(updater->phase() == AppUpdater::Ready && stand.count("/good/" + name.toLatin1()) == files + 1
                    && readFile(file) == good.data,
                QStringLiteral("one changed since is downloaded again"), describe(*updater));
    }

    // Through a redirect GitHub would make, to a host the rule allows.
    {
        StandIn::Answer hop;
        hop.status = 302;
        hop.extra << "Location: " + stand.url("/good/" + name.toLatin1()).toLatin1();
        stand.routes.insert("/hop/" + name.toLatin1(), hop);
        auto updater = fresh(QStringLiteral("hop"));
        updater->start(servedRelease(stand, "/good", name, good.data.size(), "/hop"), feed);
        waitUntil([&]() { return settled(*updater); }, 30000);
        c.check(updater->phase() == AppUpdater::Ready, QStringLiteral("a redirect to an allowed host is followed"),
                describe(*updater));
    }

    // A redirect elsewhere.
    {
        StandIn::Answer away;
        away.status = 302;
        away.extra << QByteArray("Location: https://example.invalid/") + name.toLatin1();
        stand.routes.insert("/away/" + name.toLatin1(), away);
        auto updater = fresh(QStringLiteral("away"));
        updater->start(servedRelease(stand, "/good", name, good.data.size(), "/away"), feed);
        waitUntil([&]() { return settled(*updater); }, 30000);
        c.check(updater->phase() == AppUpdater::Failed
                    && updater->message().contains(QLatin1String("other than GitHub"))
                    && updater->message().contains(QLatin1String("example.invalid")),
                QStringLiteral("a redirect to a host that is not GitHub's is refused, and said"), describe(*updater));
        c.check(QDir(updater->workDirectory()).entryList(QDir::Files).isEmpty(),
                QStringLiteral("and nothing of it is kept"));
    }

    // A link to a host that is not GitHub's, though the feed is a stand-in.
    {
        auto updater = fresh(QStringLiteral("elsewhere"));
        const int before = int(stand.requests.size());
        updater->start(release(kVersion, { asset(name, QStringLiteral("https://example.com/") + name),
                                           asset(name + QStringLiteral(".sha256"),
                                                 stand.url("/good/" + name.toLatin1() + ".sha256")) }),
                       feed);
        waitUntil([&]() { return settled(*updater); }, 5000);
        c.check(updater->phase() == AppUpdater::Failed && updater->message().contains(QLatin1String("example.com"))
                    && int(stand.requests.size()) == before,
                QStringLiteral("a link to a host that is not GitHub's is never followed"), describe(*updater));
        // Plain HTTP to a host other than this computer, likewise.
        updater->start(release(kVersion, { asset(name, QStringLiteral("http://example.com/") + name),
                                           asset(name + QStringLiteral(".sha256"),
                                                 QStringLiteral("http://example.com/x.sha256")) }),
                       feed);
        waitUntil([&]() { return settled(*updater); }, 5000);
        c.check(updater->phase() == AppUpdater::Failed && int(stand.requests.size()) == before,
                QStringLiteral("nor plain HTTP to another computer"), describe(*updater));
        // And this computer's stand-in from a feed on GitHub.
        updater->start(servedRelease(stand, "/good", name, good.data.size()),
                       QUrl(QStringLiteral("https://api.github.com/repos/droidboy08-hub/Monolist/releases/latest")));
        waitUntil([&]() { return settled(*updater); }, 5000);
        c.check(updater->phase() == AppUpdater::Failed && int(stand.requests.size()) == before,
                QStringLiteral("nor this computer, when the feed is GitHub's"), describe(*updater));
    }

    // The checksum does not match.
    {
        serve(stand, "/wrong", name, good.data, sha256Of("something else"));
        auto updater = fresh(QStringLiteral("wrong"));
        updater->start(servedRelease(stand, "/wrong", name, good.data.size()), feed);
        waitUntil([&]() { return settled(*updater); }, 30000);
        c.check(updater->phase() == AppUpdater::Failed
                    && updater->message().contains(QLatin1String("did not match its published checksum")),
                QStringLiteral("a download that does not match its checksum fails, and says so"), describe(*updater));
        c.check(QDir(updater->workDirectory()).entryList(QDir::Files).isEmpty(),
                QStringLiteral("and is deleted, its .part too"));
    }

    // No checksum at all.
    {
        auto updater = fresh(QStringLiteral("nosum"));
        const int before = int(stand.requests.size());
        updater->start(release(kVersion, { asset(name, stand.url("/good/" + name.toLatin1())) }), feed);
        waitUntil([&]() { return settled(*updater); }, 5000);
        c.check(updater->phase() == AppUpdater::Failed && updater->message().contains(QLatin1String("checksum"))
                    && int(stand.requests.size()) == before,
                QStringLiteral("a file with no published checksum is not downloaded"), describe(*updater));
        // A checksum file that names no hash.
        StandIn::Answer junk;
        junk.body = "no hash here";
        stand.routes.insert("/junk/" + name.toLatin1() + ".sha256", junk);
        updater->start(release(kVersion, { asset(name, stand.url("/good/" + name.toLatin1())),
                                           asset(name + QStringLiteral(".sha256"),
                                                 stand.url("/junk/" + name.toLatin1() + ".sha256")) }),
                       feed);
        waitUntil([&]() { return settled(*updater); }, 10000);
        c.check(updater->phase() == AppUpdater::Failed
                    && updater->message().contains(QLatin1String("could not be read")),
                QStringLiteral("nor one whose checksum file has no hash"), describe(*updater));
    }

    // The connection ends before the file does.
    {
        StandIn::Answer cut;
        cut.body = good.data.left(good.data.size() / 2);
        cut.cutShort = good.data.size() - cut.body.size();
        stand.routes.insert("/cut/" + name.toLatin1(), cut);
        auto updater = fresh(QStringLiteral("cut"));
        updater->start(servedRelease(stand, "/good", name, good.data.size(), "/cut"), feed);
        waitUntil([&]() { return settled(*updater); }, 30000);
        c.check(updater->phase() == AppUpdater::Failed
                    && QDir(updater->workDirectory()).entryList(QDir::Files).isEmpty(),
                QStringLiteral("a download cut short fails and leaves nothing"), describe(*updater));
    }

    // More than the feed said the file is: stopped before it can fill the
    // disk, and deleted.
    {
        StandIn::Answer big;
        big.body = good.data;
        stand.routes.insert("/big/" + name.toLatin1(), big);
        auto updater = fresh(QStringLiteral("big"));
        updater->start(servedRelease(stand, "/good", name, 1024 * 1024, "/big"), feed);
        waitUntil([&]() { return settled(*updater); }, 30000);
        c.check(updater->phase() == AppUpdater::Failed && updater->message().contains(QLatin1String("bigger than"))
                    && QDir(updater->workDirectory()).entryList(QDir::Files).isEmpty(),
                QStringLiteral("a download bigger than the feed said is stopped and deleted"), describe(*updater));
    }

    // A launch's cleanup while a download is under way (UPDATE pressed in
    // the first seconds): the download is left alone.
    {
        StandIn::Answer slow;
        slow.body = good.data;
        slow.chunk = 128 * 1024;
        slow.delayMs = 40;
        stand.routes.insert("/during/" + name.toLatin1(), slow);
        auto updater = fresh(QStringLiteral("during"));
        updater->start(servedRelease(stand, "/good", name, good.data.size(), "/during"), feed);
        const QString part = QDir(updater->workDirectory()).filePath(name + QStringLiteral(".part"));
        waitUntil([&]() { return updater->percent() >= 10; }, 20000);
        const QString said = updater->housekeeping();
        const bool partKept = QFileInfo::exists(part);
        waitUntil([&]() { return settled(*updater); }, 30000);
        c.check(said.isEmpty() && partKept && updater->phase() == AppUpdater::Ready,
                QStringLiteral("a launch's cleanup during a download leaves it to finish"), describe(*updater));
        updater->housekeeping();
        c.check(QFileInfo::exists(updater->downloadedFile()),
                QStringLiteral("nor clears one that is ready to go in"));
    }

    // Cancelled, and quit, part way.
    {
        StandIn::Answer slow;
        slow.body = good.data;
        slow.chunk = 64 * 1024;
        slow.delayMs = 40;
        stand.routes.insert("/slow/" + name.toLatin1(), slow);
        auto updater = fresh(QStringLiteral("slow"));
        updater->start(servedRelease(stand, "/good", name, good.data.size(), "/slow"), feed);
        const QString part = QDir(updater->workDirectory()).filePath(name + QStringLiteral(".part"));
        QList<int> seen;
        QObject::connect(updater.get(), &AppUpdater::changed, updater.get(), [&]() {
            if (updater->phase() == AppUpdater::Downloading && updater->percent() >= 0)
                seen << updater->percent();
        });
        waitUntil([&]() { return updater->percent() >= 20; }, 20000);
        c.check(seen.size() >= 3 && updater->percent() < 100
                    && updater->message() == QStringLiteral("Downloading Monolist %1… %2 %")
                                                 .arg(kVersion).arg(updater->percent()),
                QStringLiteral("its progress is told as it arrives, a step at a time"), updater->message());
        const bool partWas = QFileInfo::exists(part);
        const int abandoned = stand.abandoned;
        updater->cancel();
        c.check(partWas && updater->phase() == AppUpdater::Idle && !QFileInfo::exists(part)
                    && QDir(updater->workDirectory()).entryList(QDir::Files).isEmpty(),
                QStringLiteral("a cancel part way stops it and deletes the .part"), describe(*updater));
        waitUntil([&]() { return stand.abandoned > abandoned; }, 5000);
        c.check(stand.abandoned > abandoned, QStringLiteral("and the connection is closed"));

        updater->start(servedRelease(stand, "/good", name, good.data.size(), "/slow"), feed);
        waitUntil([&]() { return updater->percent() >= 5; }, 20000);
        const bool partAgain = QFileInfo::exists(part);
        updater.reset();
        c.check(partAgain && !QFileInfo::exists(part),
                QStringLiteral("a quit part way (the updater gone) leaves no .part"));
    }

    c.check(!stand.anyCookie(), QStringLiteral("no request carried a cookie, though every answer set one"));
}

// — About, as Settings uses it —

void checkAbout(Checks &c, const QString &scratch, StandIn &stand)
{
    const QString arch = QSysInfo::currentCpuArchitecture();
    const QString name = QStringLiteral("Monolist-%1-setup-win64.exe").arg(kVersion);
    const Payload good = payload(512 * 1024, 11);
    serve(stand, "/about", name, good.data, good.sha);
    StandIn::Answer slow;
    slow.body = good.data;
    slow.chunk = 16 * 1024;
    slow.delayMs = 60;
    stand.routes.insert("/aboutslow/" + name.toLatin1(), slow);

    // With a Mac's image too: a release is only a Mac's once it has one.
    const QString image = QStringLiteral("Monolist-%1-macos-%2.dmg").arg(kVersion, arch);
    UpdateRelease offered = servedRelease(stand, "/about", name, good.data.size());
    offered.assets << asset(image, stand.url("/about/" + image.toLatin1()));
    StandIn::Answer feed;
    feed.body = feedJson(offered);
    feed.type = "application/json";
    stand.routes.insert("/feed-about", feed);
    UpdateRelease offeredSlow = servedRelease(stand, "/about", name, good.data.size(), "/aboutslow");
    offeredSlow.assets << asset(image, stand.url("/about/" + image.toLatin1()));
    StandIn::Answer feedSlow;
    feedSlow.body = feedJson(offeredSlow);
    feedSlow.type = "application/json";
    stand.routes.insert("/feed-slow", feedSlow);

    UpdateTarget setup;
    setup.kind = UpdateTarget::WindowsSetup;
    setup.path = QDir(scratch).filePath(QStringLiteral("installed"));

    const QString previousFeed = AppInfo::updateFeed();
    AppInfo::setUpdateFeed(stand.url("/feed-about"));
    {
        AppInfo info;
        info.updater()->setTarget(setup);
        info.updater()->setDirectory(QDir(scratch).filePath(QStringLiteral("updates-about")));
        int readySaid = 0;
        QObject::connect(&info, &AppInfo::updateReady, &info, [&readySaid]() { ++readySaid; });
        info.checkForUpdate();
        waitUntil([&]() { return !info.checking(); }, 15000);
        c.check(info.updateAvailable() && info.canUpdateInApp()
                    && info.updateMessage() == QStringLiteral("Monolist %1 is available.").arg(kVersion)
                    && info.updateUrl() == offered.page.toString(),
                QStringLiteral("CHECK finds the release and offers UPDATE, with its page as the last resort"),
                info.updateMessage());
        info.downloadUpdate();
        c.check(info.downloading() || info.readyToRestart(), QStringLiteral("UPDATE starts the download"),
                info.updateMessage());
        waitUntil([&]() { return info.readyToRestart() || info.updateFailed(); }, 30000);
        c.check(info.readyToRestart() && readySaid == 1
                    && info.updateMessage() == QStringLiteral("Monolist %1 is ready: Monolist restarts to finish.")
                                                   .arg(kVersion),
                QStringLiteral("and it ends ready to RESTART, said once for a toast"), info.updateMessage());
    }
    AppInfo::setUpdateFeed(stand.url("/feed-slow"));
    {
        AppInfo info;
        info.updater()->setTarget(setup);
        info.updater()->setDirectory(QDir(scratch).filePath(QStringLiteral("updates-about-slow")));
        info.checkForUpdate();
        waitUntil([&]() { return !info.checking(); }, 15000);
        info.downloadUpdate();
        // A double-click on UPDATE: its second click lands on CANCEL.
        info.cancelUpdate();
        c.check(info.downloading(), QStringLiteral("a CANCEL a moment after UPDATE (a double-click) is not a cancel"),
                info.updateMessage());
        QElapsedTimer since;
        since.start();
        waitUntil([&]() { return info.downloading() && info.updateProgress() >= 5 && since.elapsed() > 700; },
                  20000);
        const bool wasDownloading = info.downloading()
                                    && info.updateMessage().startsWith(QStringLiteral("Downloading Monolist %1… ")
                                                                           .arg(kVersion))
                                    && info.updateMessage().endsWith(QLatin1String(" %"));
        info.cancelUpdate();
        c.check(wasDownloading && info.updateAvailable() && info.canUpdateInApp(),
                QStringLiteral("CANCEL goes back to UPDATE"), info.updateMessage());
    }
    {
        // TRY AGAIN looks for the release again, then downloads it: here the
        // checksum is wrong the first time, and put right before the second.
        const QString retryName = name;
        serve(stand, "/retry", retryName, good.data, sha256Of("not the file"));
        UpdateRelease retry = servedRelease(stand, "/retry", retryName, good.data.size());
        retry.assets << asset(image, stand.url("/about/" + image.toLatin1()));
        StandIn::Answer answer;
        answer.body = feedJson(retry);
        stand.routes.insert("/feed-retry", answer);
        AppInfo::setUpdateFeed(stand.url("/feed-retry"));
        AppInfo info;
        info.updater()->setTarget(setup);
        info.updater()->setDirectory(QDir(scratch).filePath(QStringLiteral("updates-about-retry")));
        info.checkForUpdate();
        waitUntil([&]() { return !info.checking(); }, 15000);
        info.downloadUpdate();
        waitUntil([&]() { return info.updateFailed() || info.readyToRestart(); }, 30000);
        const bool failedFirst = info.updateFailed() && !info.updateUrl().isEmpty();
        serve(stand, "/retry", retryName, good.data, good.sha);
        const int feeds = stand.count("/feed-retry");
        info.downloadUpdate();
        waitUntil([&]() { return info.readyToRestart() || (info.updateFailed() && stand.count("/feed-retry") > feeds); },
                  30000);
        c.check(failedFirst && info.readyToRestart() && stand.count("/feed-retry") == feeds + 1,
                QStringLiteral("TRY AGAIN checks for the release again, then downloads it"), info.updateMessage());
    }
    {
        // What a launch says of a RESTART that did not go in: said as a
        // failure, with TRY AGAIN and the release's page; and not before the
        // helper has left its word.
        const QString work = QDir(scratch).filePath(QStringLiteral("updates-about-launch"));
        QDir(work).removeRecursively();
        AppInfo info;
        info.updater()->setDirectory(work);
        writeFile(QDir(work).filePath(QStringLiteral("pending.json")),
                  QJsonDocument(QJsonObject{ { QStringLiteral("from"), QStringLiteral("0.0.1") },
                                             { QStringLiteral("to"), kVersion } })
                      .toJson());
        QString noticed;
        QObject::connect(&info, &AppInfo::notice, &info, [&noticed](const QString &text) { noticed = text; });
        info.startUpdateHousekeeping();
        const bool waited = noticed.isEmpty() && QFileInfo::exists(QDir(work).filePath(QStringLiteral("pending.json")));
        writeFile(QDir(work).filePath(QStringLiteral("result.txt")),
                  "failed: the new Monolist did not start, so the one you had is back.\n");
        waitUntil([&]() { return !noticed.isEmpty(); }, 8000);
        c.check(waited, QStringLiteral("a launch waits for the helper's word before saying anything"));
        c.check(info.updateFailed() && info.updateMessage().contains(QLatin1String("did not start"))
                    && info.updateUrl() == QStringLiteral("https://github.com/droidboy08-hub/Monolist/releases/tag/v")
                                               + kVersion
                    && noticed == info.updateMessage(),
                QStringLiteral("then says it did not go in, with TRY AGAIN and the release's page"),
                info.updateMessage() + QStringLiteral(" / ") + info.updateUrl());
    }
    {
        // A copy that cannot update itself is shown the page instead.
        AppInfo info;
        info.updater()->setTarget(UpdateTarget());
        info.checkForUpdate();
        waitUntil([&]() { return !info.checking(); }, 15000);
        c.check(info.updateAvailable() && !info.canUpdateInApp(),
                QStringLiteral("a copy that cannot update itself is offered GET IT, the release page"),
                info.updateMessage());
    }
    {
        // Nothing newer.
        UpdateRelease old = offered;
        old.version = QStringLiteral("0.0.1");
        StandIn::Answer answer;
        answer.body = feedJson(old);
        stand.routes.insert("/feed-old", answer);
        AppInfo::setUpdateFeed(stand.url("/feed-old"));
        AppInfo info;
        info.checkForUpdate();
        waitUntil([&]() { return !info.checking(); }, 15000);
        c.check(info.updateState() == AppInfo::UpToDate, QStringLiteral("an older release is not offered"),
                info.updateMessage());
    }
#ifdef Q_OS_MACOS
    {
        // The Mac's rule: a newer release with no disk image for this Mac.
        UpdateRelease windowsOnly = servedRelease(stand, "/about", name, good.data.size());
        StandIn::Answer answer;
        answer.body = feedJson(windowsOnly);
        stand.routes.insert("/feed-windows", answer);
        AppInfo::setUpdateFeed(stand.url("/feed-windows"));
        AppInfo info;
        info.checkForUpdate();
        waitUntil([&]() { return !info.checking(); }, 15000);
        c.check(info.updateState() == AppInfo::UpToDate
                    && info.updateMessage().contains(QLatin1String("no Mac download yet")),
                QStringLiteral("on a Mac, a release without a disk image for it is not offered"),
                info.updateMessage());
    }
#endif
    AppInfo::setUpdateFeed(previousFeed == QLatin1String(
                               "https://api.github.com/repos/droidboy08-hub/Monolist/releases/latest")
                               ? QString() : previousFeed);
}

// — what a launch says of the last RESTART —

void checkHousekeeping(Checks &c, const QString &scratch, QNetworkAccessManager &network)
{
    AppUpdater updater(&network);
    const QString work = QDir(scratch).filePath(QStringLiteral("updates-housekeeping"));
    QDir(work).removeRecursively();
    updater.setDirectory(work);
    const QString running = AppUpdater::runningVersion();
    const auto pending = [&](const QString &to) {
        writeFile(QDir(work).filePath(QStringLiteral("pending.json")),
                  QJsonDocument(QJsonObject{ { QStringLiteral("from"), QStringLiteral("0.0.1") },
                                             { QStringLiteral("to"), to } })
                      .toJson());
    };

    bool ok = true;
    pending(kVersion);
    writeFile(QDir(work).filePath(QStringLiteral("result.txt")),
              "failed: the new Monolist could not be put in place, so the one you had is back.\n");
    writeFile(QDir(work).filePath(QStringLiteral("Monolist-%1-macos-arm64.dmg").arg(kVersion)), "newer");
    writeFile(QDir(work).filePath(QStringLiteral("Monolist-0.0.1-win64.zip")), "older");
    writeFile(QDir(work).filePath(QStringLiteral("Monolist-0.0.1-win64.zip.sha256")), "older");
    writeFile(QDir(work).filePath(QStringLiteral("Monolist-%1-win64.zip.part").arg(kVersion)), "half");
    writeFile(QDir(work).filePath(QStringLiteral("staged/new/Monolist.app/x")), "staged");
    QString said = updater.housekeeping(&ok);
    c.check(!ok && said.contains(QLatin1String("was not put in place"))
                && said.contains(QLatin1String("The new Monolist could not be put in place, so the one you had is back."))
                && said.contains(running),
            QStringLiteral("a launch after a RESTART that failed says why, and that this is still the old one"), said);
    c.check(!QFileInfo::exists(QDir(work).filePath(QStringLiteral("pending.json")))
                && !QFileInfo::exists(QDir(work).filePath(QStringLiteral("result.txt"))),
            QStringLiteral("and says it once"));
    c.check(QFileInfo::exists(QDir(work).filePath(QStringLiteral("Monolist-%1-macos-arm64.dmg").arg(kVersion)))
                && !QFileInfo::exists(QDir(work).filePath(QStringLiteral("Monolist-0.0.1-win64.zip")))
                && !QFileInfo::exists(QDir(work).filePath(QStringLiteral("Monolist-0.0.1-win64.zip.sha256")))
                && filesEnding(work, QStringLiteral(".part")).isEmpty()
                && !QFileInfo::exists(QDir(work).filePath(QStringLiteral("staged"))),
            QStringLiteral("half downloads, older releases' files and a staged copy are cleared; a newer one kept"));

    pending(kVersion);
    said = updater.housekeeping(&ok);
    c.check(!ok && said.contains(QLatin1String("It did not finish.")),
            QStringLiteral("a RESTART that left no word says it did not finish"), said);

    pending(running);
    c.check(!updater.helperDone(), QStringLiteral("a RESTART whose helper has left no word is not done"));
    writeFile(QDir(work).filePath(QStringLiteral("result.txt")), "ok\n");
    c.check(updater.helperDone(), QStringLiteral("and is once it has"));
    said = updater.housekeeping(&ok);
    c.check(ok && said == QStringLiteral("Monolist is now %1.").arg(running),
            QStringLiteral("a launch of the version a RESTART put in place says so"), said);
    c.check(QString::fromUtf8(readFile(QDir(work).filePath(QStringLiteral("last.txt")))).trimmed() == said,
            QStringLiteral("and keeps what it said, for a look afterwards"));
    said = updater.housekeeping(&ok);
    c.check(ok && said.isEmpty(), QStringLiteral("and nothing the next time"));

    pending(running);
    writeFile(QDir(work).filePath(QStringLiteral("result.txt")), "ok\nkept: C:/Apps/Monolist.old-1234\n");
    said = updater.housekeeping(&ok);
    c.check(ok && said.contains(QLatin1String("kept at")) && said.contains(QLatin1String("Monolist.old-1234")),
            QStringLiteral("a portable copy's old folder kept for the files in it is said, with where"), said);
}

#ifdef Q_OS_MACOS

// — the Mac: scratch bundles in disk images —

QByteArray plist(const QString &id, const QString &build)
{
    return QStringLiteral(
               "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
               "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
               "<plist version=\"1.0\"><dict>\n"
               "<key>CFBundleExecutable</key><string>Monolist</string>\n"
               "<key>CFBundleIdentifier</key><string>%1</string>\n"
               "<key>CFBundleName</key><string>Monolist</string>\n"
               "<key>CFBundlePackageType</key><string>APPL</string>\n"
               "<key>CFBundleShortVersionString</key><string>99.0</string>\n"
               "<key>CFBundleVersion</key><string>%2</string>\n"
               "</dict></plist>\n")
        .arg(id, build)
        .toUtf8();
}

// A scratch Monolist.app: /usr/bin/true as its program, a resource, signed
// ad hoc as a build here is.
bool makeBundle(const QString &bundle, const QString &id, const QString &build)
{
    QDir(bundle).removeRecursively();
    if (!writeFile(bundle + QStringLiteral("/Contents/Info.plist"), plist(id, build))
        || !writeFile(bundle + QStringLiteral("/Contents/Resources/note.txt"), "build " + build.toUtf8()))
        return false;
    const QString program = bundle + QStringLiteral("/Contents/MacOS/Monolist");
    QDir().mkpath(QFileInfo(program).absolutePath());
    if (!QFile::copy(QStringLiteral("/usr/bin/true"), program))
        return false;
    QFile(program).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                                  | QFileDevice::ReadGroup | QFileDevice::ExeGroup);
    return runCommand(QStringLiteral("/usr/bin/codesign"), { QStringLiteral("--force"), QStringLiteral("--sign"),
                                                             QStringLiteral("-"), bundle }).code == 0;
}

// A disk image holding `bundle` as Monolist.app, as build-macos.sh makes one.
bool makeImage(const QString &bundle, const QString &image)
{
    const QString folder = image + QStringLiteral(".src");
    QDir(folder).removeRecursively();
    QDir().mkpath(folder);
    if (!bundle.isEmpty()
        && runCommand(QStringLiteral("/usr/bin/ditto"), { bundle, folder + QStringLiteral("/Monolist.app") }).code != 0)
        return false;
    writeFile(folder + QStringLiteral("/Read me.txt"), "a disk image of the test's own");
    QFile::remove(image);
    const Ran made = runCommand(QStringLiteral("/usr/bin/hdiutil"),
                                { QStringLiteral("create"), QStringLiteral("-quiet"), QStringLiteral("-volname"),
                                  QStringLiteral("Monolist"), QStringLiteral("-srcfolder"), folder,
                                  QStringLiteral("-ov"), QStringLiteral("-format"), QStringLiteral("UDZO"), image });
    QDir(folder).removeRecursively();
    return made.code == 0;
}

bool imageAttached(const QString &image)
{
    if (image.isEmpty())
        return false;
    const Ran info = runCommand(QStringLiteral("/usr/bin/hdiutil"), { QStringLiteral("info") });
    const QString canonical = QFileInfo(image).canonicalFilePath();
    return (!canonical.isEmpty() && info.out.contains(QFile::encodeName(canonical)))
           || info.out.contains(QFile::encodeName(image));
}

QString buildOf(const QString &bundle)
{
    return MacUpdate::bundleInfo(bundle).value(QStringLiteral("build")).toString();
}

// A process that has already ended: the Monolist the helper waits for, quit.
qint64 gone()
{
    qint64 pid = 0;
    QProcess::startDetached(QStringLiteral("/usr/bin/true"), {}, QString(), &pid);
    pause(300);
    return pid;
}

// An opener or lsregister that writes down how it was called, one line per
// call, answers as the file `<it>.answer` says (0 when there is none), and
// makes the file `<it>.touch` names, if there is one: the word a Monolist
// it opened would leave once started.
QString recorder(const QString &path)
{
    writeFile(path, "#!/bin/sh\n"
                    "log=\"$0.log\"\n"
                    "printf '%s\\n' \"$*\" >> \"$log\"\n"
                    "if [ -f \"$0.touch\" ]; then : > \"$(cat \"$0.touch\")\"; fi\n"
                    "if [ -f \"$0.answer\" ]; then exit \"$(cat \"$0.answer\")\"; fi\n"
                    "exit 0\n");
    QFile(path).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return path;
}

void checkMac(Checks &c, const QString &scratch, StandIn &stand, QNetworkAccessManager &network)
{
    const QString arch = QSysInfo::currentCpuArchitecture();
    const QString root = QDir(scratch).filePath(QStringLiteral("mac"));
    QDir(root).removeRecursively();
    QDir().mkpath(root);
    const QString id = QStringLiteral("io.github.droidboy08-hub.monolist");

    // What this copy is.
    const UpdateTarget self = AppUpdater::describeThisCopy();
    c.check(self.kind == UpdateTarget::MacBundle && self.path.endsWith(QLatin1String(".app")),
            QStringLiteral("this copy is a Mac bundle"), self.path);
    c.note(QStringLiteral("this copy: %1, %2, %3")
               .arg(self.path, MacUpdate::runningIsDeveloperId() ? QStringLiteral("Developer ID")
                                                                 : QStringLiteral("not Developer ID"),
                    self.blocker.isEmpty() ? QStringLiteral("replaceable in place") : self.blocker));

    // The bundles: the copy in place (build 100), and what updates might hold.
    const QString installed = root + QStringLiteral("/Applications/Monolist.app");
    const QString images = root + QStringLiteral("/images");
    QDir().mkpath(images);
    const QString unsignedName = QStringLiteral("Monolist-%1-macos-%2-unsigned.dmg").arg(kVersion, arch);
    struct Image {
        QByteArray folder;
        QString bundleId;
        QString build;
        bool tamper;
        bool empty;
    };
    const Image kinds[] = {
        { "/mac-new", id, QStringLiteral("101"), false, false },
        { "/mac-other", QStringLiteral("com.example.notmonolist"), QStringLiteral("101"), false, false },
        { "/mac-same", id, QStringLiteral("100"), false, false },
        { "/mac-tampered", id, QStringLiteral("101"), true, false },
        { "/mac-empty", id, QStringLiteral("101"), false, true },
    };
    bool made = makeBundle(installed, id, QStringLiteral("100"));
    for (const Image &kind : kinds) {
        const QString bundle = images + QString::fromLatin1(kind.folder) + QStringLiteral("/Monolist.app");
        made = made && makeBundle(bundle, kind.bundleId, kind.build);
        if (kind.tamper)
            made = made && writeFile(bundle + QStringLiteral("/Contents/Resources/note.txt"), "changed after signing");
        const QString image = images + QString::fromLatin1(kind.folder) + QStringLiteral(".dmg");
        made = made && makeImage(kind.empty ? QString() : bundle, image);
        const QByteArray data = readFile(image);
        serve(stand, kind.folder, unsignedName, data, sha256Of(data));
    }
    if (!c.check(made, QStringLiteral("scratch bundles signed ad hoc, in disk images")))
        return;

    c.check(MacUpdate::blockerFor(installed).isEmpty(),
            QStringLiteral("a bundle in a folder Monolist may change can be replaced in place"),
            MacUpdate::blockerFor(installed));
    c.check(MacUpdate::bundleInfo(installed).value(QStringLiteral("id")).toString() == id
                && buildOf(installed) == QLatin1String("100"),
            QStringLiteral("a bundle's identifier and build are read from its Info.plist"));
    const Ran requirement = runCommand(QStringLiteral("/usr/bin/codesign"),
                                       { QStringLiteral("--verify"),
                                         QStringLiteral("-R=") + MacUpdate::developerIdRequirement(), installed });
    c.check(requirement.code != 0 && !requirement.err.contains("syntax") && requirement.err.contains("requirement"),
            QStringLiteral("the Developer ID requirement reads, and an ad hoc bundle fails it"),
            QString::fromUtf8(requirement.err).trimmed());

    // Why a copy cannot be replaced where it is.
    {
        const QString translocated = root + QStringLiteral("/AppTranslocation/ABCD/d/Monolist.app");
        makeBundle(translocated, id, QStringLiteral("100"));
        c.check(MacUpdate::blockerFor(translocated).contains(QLatin1String("temporary place")),
                QStringLiteral("a translocated copy is not replaced in place"), MacUpdate::blockerFor(translocated));
        const QString built = root + QStringLiteral("/build/Monolist.app");
        makeBundle(built, id, QStringLiteral("100"));
        writeFile(root + QStringLiteral("/build/CMakeCache.txt"), "CMAKE");
        c.check(MacUpdate::blockerFor(built).contains(QLatin1String("build folder")),
                QStringLiteral("nor a build in its build folder"), MacUpdate::blockerFor(built));
        const QString locked = root + QStringLiteral("/Locked/Monolist.app");
        makeBundle(locked, id, QStringLiteral("100"));
        QFile(root + QStringLiteral("/Locked")).setPermissions(QFileDevice::ReadOwner | QFileDevice::ExeOwner);
        c.check(MacUpdate::blockerFor(locked).contains(QLatin1String("may not change")),
                QStringLiteral("nor one in a folder Monolist may not change"), MacUpdate::blockerFor(locked));
        QFile(root + QStringLiteral("/Locked")).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                                                | QFileDevice::ExeOwner);
        // Attached as the update attaches it; and, as it does, tried once
        // more when macOS attaches the image and mounts nothing of it.
        QString point;
        QString device;
        for (int attempt = 0; attempt < 2 && point.isEmpty(); ++attempt) {
            const Ran attached = runCommand(QStringLiteral("/usr/bin/hdiutil"),
                                            { QStringLiteral("attach"), QStringLiteral("-nobrowse"),
                                              QStringLiteral("-readonly"), QStringLiteral("-noautoopen"),
                                              QStringLiteral("-plist"), QStringLiteral("-mountrandom"), root,
                                              images + QStringLiteral("/mac-new.dmg") });
            point = MacUpdate::mountPointFrom(attached.out);
            device = MacUpdate::deviceFrom(attached.out);
            if (point.isEmpty() && !device.isEmpty()) {
                c.note(QStringLiteral("attached at %1 with nothing mounted; once more").arg(device));
                runCommand(QStringLiteral("/usr/bin/hdiutil"), { QStringLiteral("detach"), QStringLiteral("-force"),
                                                                 device });
                pause(1000);
            }
        }
        c.check(!point.isEmpty() && device.startsWith(QLatin1String("/dev/disk")),
                QStringLiteral("hdiutil's answer gives the mount point and the image's device"),
                point + QLatin1Char(' ') + device);
        if (!point.isEmpty()) {
            const QString blocker = MacUpdate::blockerFor(point + QStringLiteral("/Monolist.app"));
            c.check(blocker.contains(QLatin1String("disk image")),
                    QStringLiteral("nor one running from a disk image"), blocker);
            runCommand(QStringLiteral("/usr/bin/hdiutil"), { QStringLiteral("detach"), device });
            c.check(!imageAttached(images + QStringLiteral("/mac-new.dmg")),
                    QStringLiteral("an image is let go of by its device"));
        }
        c.check(MacUpdate::mountPointFrom("garbage").isEmpty() && MacUpdate::deviceFrom("garbage").isEmpty(),
                QStringLiteral("no answer gives no mount point"));
    }

    const UpdateTarget target = MacUpdate::describeBundle(installed, false);
    const QUrl feed(stand.url("/feed"));
    const QString work = root + QStringLiteral("/updates");
    const auto prepare = [&](AppUpdater &updater, const QByteArray &folder, bool developerId) {
        UpdateTarget wanted = target;
        wanted.developerId = developerId;
        updater.setTarget(wanted);
        updater.setDirectory(work);
        const QString image = images + QString::fromLatin1(folder) + QStringLiteral(".dmg");
        updater.start(servedRelease(stand, folder, unsignedName, QFileInfo(image).size()), feed);
        waitUntil([&]() { return settled(updater); }, 180000);
        return updater.phase();
    };

    {
        AppUpdater updater(&network);
        prepare(updater, "/mac-new", false);
        c.check(updater.phase() == AppUpdater::Ready && buildOf(updater.stagedPath()) == QLatin1String("101"),
                QStringLiteral("the disk image's app is copied out, checked and ready"), describe(updater));
        c.check(!imageAttached(updater.downloadedFile()), QStringLiteral("and the image is let go of"));
        const QString first = updater.stagedPath();
        updater.cancel();
        c.check(updater.phase() == AppUpdater::Idle && !QFileInfo::exists(first),
                QStringLiteral("a cancel when ready removes the staged copy"));
    }
    {
        // The Developer ID image is the same scratch bundle, signed ad hoc.
        AppUpdater updater(&network);
        UpdateTarget wanted = target;
        wanted.developerId = true;
        updater.setTarget(wanted);
        updater.setDirectory(work);
        const QString signedName = QStringLiteral("Monolist-%1-macos-%2.dmg").arg(kVersion, arch);
        const QByteArray data = readFile(images + QStringLiteral("/mac-new.dmg"));
        serve(stand, "/mac-signed", signedName, data, sha256Of(data));
        updater.start(servedRelease(stand, "/mac-signed", signedName, data.size()), feed);
        waitUntil([&]() { return settled(updater); }, 180000);
        c.check(updater.phase() == AppUpdater::Failed
                    && updater.message().contains(QLatin1String("not signed by Monolist's developer")),
                QStringLiteral("a Developer ID copy refuses an update that is not signed with its Developer ID"),
                describe(updater));
        c.check(!QFileInfo::exists(root + QStringLiteral("/updates/staged/new/Monolist.app")),
                QStringLiteral("and removes what it had copied out"));
    }
    struct Refusal {
        QByteArray folder;
        const char *said;
        const char *what;
    };
    const Refusal refusals[] = {
        { "/mac-other", "another app", "an update that is another app is refused" },
        { "/mac-same", "not newer", "an update that is not a newer build is refused" },
        { "/mac-tampered", "signature check", "an update whose seal is broken is refused" },
        { "/mac-empty", "no Monolist in it", "a disk image with no Monolist in it is refused" },
    };
    for (const Refusal &refusal : refusals) {
        AppUpdater updater(&network);
        prepare(updater, refusal.folder, false);
        c.check(updater.phase() == AppUpdater::Failed
                    && updater.message().contains(QString::fromLatin1(refusal.said)),
                QString::fromLatin1(refusal.what), describe(updater));
        c.check(!imageAttached(work + QLatin1Char('/') + unsignedName)
                    && !QFileInfo::exists(work + QLatin1Char('/') + unsignedName),
                QStringLiteral("  and its image is let go of, and deleted"));
    }
    c.check(buildOf(installed) == QLatin1String("100"), QStringLiteral("none of that touched the copy in place"));

    // A copy that cannot be replaced where it is: the download checked all
    // the same, and only one that passes shown, marked as downloaded.
    {
        AppUpdater updater(&network);
        UpdateTarget blocked = target;
        blocked.blocker = QStringLiteral("Monolist may not change the folder it is in");
        updater.setTarget(blocked);
        updater.setDirectory(work);
        updater.start(servedRelease(stand, "/mac-new", unsignedName, 0), feed);
        waitUntil([&]() { return settled(updater); }, 180000);
        c.check(updater.phase() == AppUpdater::Manual && updater.message().contains(QLatin1String("Drag Monolist"))
                    && QFileInfo::exists(updater.downloadedFile()),
                QStringLiteral("a copy that cannot be replaced in place gets the disk image, checked, and the words "
                               "to drag it"), describe(updater));
        c.check(MacUpdate::isQuarantined(updater.downloadedFile()),
                QStringLiteral("marked as downloaded, so that Gatekeeper checks what is dragged out of it"));
        c.check(updater.stagedPath().isEmpty() && !QFileInfo::exists(work + QStringLiteral("/staged/new/Monolist.app"))
                    && !imageAttached(updater.downloadedFile()),
                QStringLiteral("and the copy made to check it is gone, its image let go of"));
    }
    {
        AppUpdater updater(&network);
        UpdateTarget blocked = target;
        blocked.blocker = QStringLiteral("Monolist may not change the folder it is in");
        blocked.developerId = true;
        updater.setTarget(blocked);
        updater.setDirectory(work);
        const QString signedName = QStringLiteral("Monolist-%1-macos-%2.dmg").arg(kVersion, arch);
        updater.start(servedRelease(stand, "/mac-signed", signedName, 0), feed);
        waitUntil([&]() { return settled(updater); }, 180000);
        c.check(updater.phase() == AppUpdater::Failed
                    && updater.message().contains(QLatin1String("not signed by Monolist's developer"))
                    && !QFileInfo::exists(work + QLatin1Char('/') + signedName),
                QStringLiteral("nor is an image whose app is not signed as this copy is shown: it is deleted"),
                describe(updater));
        updater.showDownloaded();
        c.check(updater.phase() == AppUpdater::Failed, QStringLiteral("  and SHOW has nothing to show"));
    }
    {
        AppUpdater updater(&network);
        UpdateTarget blocked = target;
        blocked.blocker = QStringLiteral("Monolist may not change the folder it is in");
        updater.setTarget(blocked);
        updater.setDirectory(work);
        updater.start(servedRelease(stand, "/mac-other", unsignedName, 0), feed);
        waitUntil([&]() { return settled(updater); }, 180000);
        c.check(updater.phase() == AppUpdater::Failed && updater.message().contains(QLatin1String("another app")),
                QStringLiteral("nor one that is another app"), describe(updater));
    }

    // A disk image of the update's that a crash left attached: let go of at
    // the next launch.
    {
        const QString leftover = root + QStringLiteral("/leftover");
        QDir().mkpath(leftover + QStringLiteral("/mount"));
        const QString image = leftover + QLatin1Char('/') + unsignedName;
        QFile::copy(images + QStringLiteral("/mac-new.dmg"), image);
        runCommand(QStringLiteral("/usr/bin/hdiutil"),
                   { QStringLiteral("attach"), QStringLiteral("-nobrowse"), QStringLiteral("-readonly"),
                     QStringLiteral("-noautoopen"), QStringLiteral("-mountrandom"), leftover + QStringLiteral("/mount"),
                     image });
        const Ran info = runCommand(QStringLiteral("/usr/bin/hdiutil"), { QStringLiteral("info"), QStringLiteral("-plist") });
        const QStringList devices = MacUpdate::devicesAttachedFrom(info.out, leftover);
        const bool was = imageAttached(image);
        AppUpdater updater(&network);
        updater.setDirectory(leftover);
        updater.housekeeping();
        c.check(was && devices.size() == 1 && !imageAttached(image),
                QStringLiteral("an image a crash left attached is let go of at the next launch"),
                devices.join(QLatin1Char(' ')));
        c.check(MacUpdate::devicesAttachedFrom(info.out, images).isEmpty(),
                QStringLiteral("  and only the update's own"));
    }

    // The helper: this program, run as --update-swap.
    const QString tools = root + QStringLiteral("/tools");
    const QString opener = recorder(tools + QStringLiteral("/open"));
    const QString lsregister = recorder(tools + QStringLiteral("/lsregister"));
    const QString openerLog = opener + QStringLiteral(".log");
    const QString lsregisterLog = lsregister + QStringLiteral(".log");
    const QString result = work + QStringLiteral("/result.txt");
    const QString started = work + QStringLiteral("/started");
    const auto handOver = [&](AppUpdater &updater, qint64 pid, int timeout, QString *error, bool starts = true,
                              int startSeconds = 2) {
        AppUpdater::HelperOptions options;
        options.pid = pid;
        options.opener = opener;
        options.lsregister = lsregister;
        options.timeoutSeconds = timeout;
        options.startSeconds = startSeconds;
        options.wait = true;
        updater.setHelperOptions(options);
        QFile::remove(openerLog);
        QFile::remove(lsregisterLog);
        // The new copy, opened, leaves the word it started (as the app does
        // once its window is up), or does not.
        if (starts)
            writeFile(opener + QStringLiteral(".touch"), started.toUtf8());
        else
            QFile::remove(opener + QStringLiteral(".touch"));
        const bool handed = updater.handOver(error);
        QFile::remove(opener + QStringLiteral(".touch"));
        return handed;
    };
    const auto said = [&]() { return QString::fromUtf8(readFile(result)).trimmed(); };

    {
        AppUpdater updater(&network);
        prepare(updater, "/mac-new", false);
        // Detached, as Monolist is to the helper: no zombie of this test's
        // own for it to wait on.
        qint64 running = 0;
        QProcess::startDetached(QStringLiteral("/bin/sleep"), { QStringLiteral("1") }, QString(), &running);
        QString error;
        const bool handed = handOver(updater, running, 30, &error);
        const QString log = QString::fromUtf8(readFile(openerLog));
        c.check(handed && said() == QLatin1String("ok"), QStringLiteral("the helper puts the new app in place"),
                error + said());
        c.check(buildOf(installed) == QLatin1String("101"), QStringLiteral("at the same path, as the new build"),
                buildOf(installed));
        c.check(!QFileInfo::exists(work + QStringLiteral("/staged/old/Monolist.app"))
                    && !QFileInfo::exists(work + QStringLiteral("/staged/new/Monolist.app")),
                QStringLiteral("and the old one is gone, once the new one has started"));
        c.check(log.startsWith(QStringLiteral("-n ")) && log.trimmed().endsWith(installed)
                    && log.contains(QStringLiteral("--env MONOLIST_DATA_DIR=") + qEnvironmentVariable("MONOLIST_DATA_DIR")),
                QStringLiteral("it opens the new one, as a new instance, with this library"), log.trimmed());
        c.check(QString::fromUtf8(readFile(lsregisterLog)).trimmed() == QStringLiteral("-f ") + installed,
                QStringLiteral("after registering it with LaunchServices"));
        const QJsonObject pending = QJsonDocument::fromJson(readFile(work + QStringLiteral("/pending.json"))).object();
        c.check(pending.value(QStringLiteral("to")).toString() == kVersion
                    && pending.value(QStringLiteral("from")).toString() == AppUpdater::runningVersion(),
                QStringLiteral("RESTART leaves word of what it was for, for the next launch"));
        // A second RESTART: nothing more is started.
        QFile::remove(result);
        QString again;
        c.check(updater.handOver(&again) && !QFileInfo::exists(result)
                    && QString::fromUtf8(readFile(openerLog)).count(QLatin1Char('\n')) == 1,
                QStringLiteral("a second RESTART starts no second helper"));
        updater.cancel();
        c.check(updater.handedOver(), QStringLiteral("  and nothing cancels one handed over"));
    }

    // The way back: the new app cannot be moved in (made immutable, by the
    // end-to-end test's fault, here as there).
    {
        makeBundle(installed, id, QStringLiteral("100"));
        AppUpdater updater(&network);
        prepare(updater, "/mac-new", false);
        const QString staged = updater.stagedPath();
        qputenv("MONOLIST_UPDATE_TEST_FAULT", "move-new");
        QString error;
        handOver(updater, gone(), 30, &error);
        qunsetenv("MONOLIST_UPDATE_TEST_FAULT");
        c.check(said().startsWith(QLatin1String("failed: the new Monolist could not be put in place, so the one you "
                                                "had is back")),
                QStringLiteral("when the new app cannot be moved in, the helper says so"), said());
        c.check(buildOf(installed) == QLatin1String("100"), QStringLiteral("and the old one is back where it was"),
                buildOf(installed));
        c.check(QString::fromUtf8(readFile(openerLog)).trimmed().endsWith(installed),
                QStringLiteral("and opened again"));
        ::chflags(QFile::encodeName(staged).constData(), 0);
    }

    // The way back: the new app will not open.
    {
        makeBundle(installed, id, QStringLiteral("100"));
        AppUpdater updater(&network);
        prepare(updater, "/mac-new", false);
        writeFile(opener + QStringLiteral(".answer"), "1");
        QString error;
        handOver(updater, gone(), 30, &error);
        QFile::remove(opener + QStringLiteral(".answer"));
        c.check(said().startsWith(QLatin1String("failed: the new Monolist would not open, so the one you had is back")),
                QStringLiteral("when the new app will not open, the helper says so"), said());
        c.check(buildOf(installed) == QLatin1String("100"), QStringLiteral("and puts the old one back"),
                buildOf(installed));
        c.check(QString::fromUtf8(readFile(lsregisterLog)).count(QStringLiteral("-f ")) == 2,
                QStringLiteral("registered again, the old one too"));
    }

    // The way back: the new app opens, and ends before it has started (a
    // part of it missing): the old one is kept until then, and put back.
    {
        makeBundle(installed, id, QStringLiteral("100"));
        AppUpdater updater(&network);
        prepare(updater, "/mac-new", false);
        QString error;
        QElapsedTimer clock;
        clock.start();
        handOver(updater, gone(), 30, &error, false, 1);
        c.check(said().startsWith(QLatin1String("failed: the new Monolist did not start, so the one you had is back")),
                QStringLiteral("when the new app does not start, the helper says so"),
                said() + QStringLiteral(" after %1 ms").arg(clock.elapsed()));
        c.check(buildOf(installed) == QLatin1String("100")
                    && QString::fromUtf8(readFile(openerLog)).count(QLatin1Char('\n')) == 2,
                QStringLiteral("and puts the old one back, and opens it"), buildOf(installed));
    }

    // Checked once more just before it goes in: changed since it was checked.
    {
        makeBundle(installed, id, QStringLiteral("100"));
        AppUpdater updater(&network);
        prepare(updater, "/mac-new", false);
        writeFile(updater.stagedPath() + QStringLiteral("/Contents/Resources/note.txt"), "changed while it waited");
        QString error;
        handOver(updater, gone(), 30, &error);
        c.check(said().startsWith(QLatin1String("failed: the new Monolist no longer passed its signature check"))
                    && buildOf(installed) == QLatin1String("100")
                    && QString::fromUtf8(readFile(openerLog)).trimmed().endsWith(installed),
                QStringLiteral("a new app changed after its check is not put in place; the old one is opened again"),
                said());
    }

    // A path that a shell would read as commands: taken as a path.
    {
        const QString odd = root + QStringLiteral("/a b'c $(touch PWNED) `id` \"d;e");
        const QString oddInstalled = odd + QStringLiteral("/Monolist.app");
        makeBundle(oddInstalled, id, QStringLiteral("100"));
        AppUpdater updater(&network);
        UpdateTarget wanted = MacUpdate::describeBundle(oddInstalled, false);
        updater.setTarget(wanted);
        const QString oddWork = odd + QStringLiteral("/updates");
        updater.setDirectory(oddWork);
        updater.start(servedRelease(stand, "/mac-new", unsignedName, 0), feed);
        waitUntil([&]() { return settled(updater); }, 180000);
        AppUpdater::HelperOptions options;
        options.pid = gone();
        options.opener = opener;
        options.lsregister = lsregister;
        options.startSeconds = 2;
        options.wait = true;
        updater.setHelperOptions(options);
        writeFile(opener + QStringLiteral(".touch"), (oddWork + QStringLiteral("/started")).toUtf8());
        QString error;
        updater.handOver(&error);
        QFile::remove(opener + QStringLiteral(".touch"));
        c.check(QString::fromUtf8(readFile(oddWork + QStringLiteral("/result.txt"))).trimmed() == QLatin1String("ok")
                    && buildOf(oddInstalled) == QLatin1String("101") && filesEnding(root, QStringLiteral("PWNED")).isEmpty()
                    && !QFileInfo::exists(QDir::currentPath() + QStringLiteral("/PWNED")),
                QStringLiteral("a folder named with quotes, $( ) and backticks is only ever a path"),
                QString::fromUtf8(readFile(oddWork + QStringLiteral("/result.txt"))).trimmed());
    }

    // Monolist does not quit.
    {
        makeBundle(installed, id, QStringLiteral("100"));
        AppUpdater updater(&network);
        prepare(updater, "/mac-new", false);
        qint64 stuck = 0;
        QProcess::startDetached(QStringLiteral("/bin/sleep"), { QStringLiteral("30") }, QString(), &stuck);
        QElapsedTimer clock;
        clock.start();
        QString error;
        handOver(updater, stuck, 1, &error);
        const qint64 waited = clock.elapsed();
        if (stuck > 0)
            ::kill(pid_t(stuck), SIGTERM);
        c.check(said() == QLatin1String("failed: Monolist did not quit, so nothing was changed.")
                    && buildOf(installed) == QLatin1String("100") && waited < 10000,
                QStringLiteral("a Monolist that does not quit is waited for, then left as it is"),
                said() + QStringLiteral(" after %1 ms").arg(waited));
        QFile::remove(work + QStringLiteral("/pending.json"));
    }

    // A helper given arguments of another build's: does nothing.
    {
        const Ran ran = runCommand(QCoreApplication::applicationFilePath(),
                                   { QStringLiteral("--update-swap"), QStringLiteral("0"), QStringLiteral("1") });
        c.check(ran.code == 2, QStringLiteral("the helper refuses arguments it does not know"));
    }
}

#endif // Q_OS_MACOS

#ifdef Q_OS_WIN

// — Windows: a portable copy's folder, the setup —

QString systemTool(const QString &name)
{
    return QDir::toNativeSeparators(QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows")))
                                        .filePath(QStringLiteral("System32/") + name));
}

// A scratch portable copy: `program` (whoami.exe: it starts, says who, and
// ends) as its monolist.exe, a package's licence, and a mark of which it is.
bool makeCopy(const QString &folder, const QByteArray &mark,
              const QString &program = QStringLiteral("whoami.exe"))
{
    QDir(folder).removeRecursively();
    return writeFile(folder + QStringLiteral("/LICENSES/Monolist-MIT.txt"), "MIT")
           && writeFile(folder + QStringLiteral("/") + QString::fromLatin1(mark) + QStringLiteral(".txt"), mark)
           && QFile::copy(systemTool(program), folder + QStringLiteral("/monolist.exe"));
}

// `folder` zipped as package-windows.ps1 zips the package: its one folder.
QByteArray zipOf(const QString &parent, const QString &folder, const QString &zip)
{
    QFile::remove(zip);
    runCommand(systemTool(QStringLiteral("tar.exe")),
               { QStringLiteral("-a"), QStringLiteral("-c"), QStringLiteral("-f"), QDir::toNativeSeparators(zip),
                 QStringLiteral("-C"), QDir::toNativeSeparators(parent), folder });
    return readFile(zip);
}

void checkWindows(Checks &c, const QString &scratch, StandIn &stand, QNetworkAccessManager &network)
{
    const QString root = QDir(scratch).filePath(QStringLiteral("win"));
    QDir(root).removeRecursively();
    QDir().mkpath(root);
    const UpdateTarget self = AppUpdater::describeThisCopy();
    c.note(QStringLiteral("this copy: kind %1, %2").arg(int(self.kind)).arg(self.path));

    // The new copy, zipped as package-windows.ps1 zips it; and one whose
    // monolist.exe ends with an error at once (findstr.exe, given nothing).
    const QString zipName = QStringLiteral("Monolist-%1-win64.zip").arg(kVersion);
    bool made = makeCopy(root + QStringLiteral("/zip/Monolist"), "new");
    const QByteArray zipped = zipOf(root + QStringLiteral("/zip"), QStringLiteral("Monolist"),
                                    root + QStringLiteral("/") + zipName);
    serve(stand, "/win", zipName, zipped, sha256Of(zipped));
    made = made && makeCopy(root + QStringLiteral("/broken/Monolist"), "new", QStringLiteral("findstr.exe"));
    const QByteArray broken = zipOf(root + QStringLiteral("/broken"), QStringLiteral("Monolist"),
                                    root + QStringLiteral("/broken.zip"));
    serve(stand, "/winbroken", zipName, broken, sha256Of(broken));
    if (!c.check(made && !zipped.isEmpty() && !broken.isEmpty(), QStringLiteral("scratch portable copies, zipped")))
        return;

    const QString app = root + QStringLiteral("/apps x/Monolist");
    const QString work = root + QStringLiteral("/updates");
    const QUrl feed(stand.url("/feed"));
    const auto prepare = [&](AppUpdater &updater, const QByteArray &folder) {
        const UpdateTarget target = WinUpdate::describeFolder(app, QString(), QString());
        updater.setTarget(target);
        updater.setDirectory(work);
        updater.start(servedRelease(stand, folder, zipName, 0), feed);
        waitUntil([&]() { return settled(updater); }, 120000);
    };
    const QString result = work + QStringLiteral("/result.txt");
    const auto said = [&]() { return QString::fromUtf8(readFile(result)).trimmed(); };
    // Detached, as Monolist is to the helper.
    const auto standInMonolist = []() {
        qint64 running = 0;
        QProcess::startDetached(systemTool(QStringLiteral("PING.EXE")),
                                { QStringLiteral("-n"), QStringLiteral("2"), QStringLiteral("127.0.0.1") }, QString(),
                                &running);
        return running;
    };
    const auto handOver = [&](AppUpdater &updater, QString *error) {
        AppUpdater::HelperOptions options;
        options.pid = standInMonolist();
        options.timeoutSeconds = 30;
        options.startSeconds = 10;
        options.wait = true;
        updater.setHelperOptions(options);
        return updater.handOver(error);
    };
    const QDir parent(root + QStringLiteral("/apps x"));
    const auto folders = [&]() { return parent.entryList(QDir::Dirs | QDir::NoDotAndDotDot); };

    // Swapped, with nothing of the old folder's but what the new one has
    // too: the old one is deleted. Started the way Monolist starts it: its
    // current folder Monolist's own, which the helper must not keep.
    makeCopy(app, "new");
    {
        AppUpdater updater(&network);
        prepare(updater, "/win");
        c.check(updater.phase() == AppUpdater::Ready
                    && QFileInfo::exists(updater.stagedPath() + QStringLiteral("/monolist.exe"))
                    && QFileInfo::exists(updater.stagedPath() + QStringLiteral("/new.txt")),
                QStringLiteral("a portable copy's zip is unpacked beside it, ready"), describe(updater));
        writeFile(app + QStringLiteral("/new.txt"), "the old copy's, to be replaced");
        AppUpdater::HelperOptions options;
        options.pid = standInMonolist();
        options.timeoutSeconds = 30;
        options.startSeconds = 10;
        updater.setHelperOptions(options);
        QFile::remove(result);
        const QString was = QDir::currentPath();
        QDir::setCurrent(app);
        QString error;
        const bool handed = updater.handOver(&error);
        // Monolist quits, and lets go of its folder; the helper must not
        // have taken it over.
        QDir::setCurrent(was);
        waitUntil([&]() { return QFileInfo::exists(result); }, 90000);
        c.check(handed && said() == QLatin1String("ok"),
                QStringLiteral("the helper, started from inside Monolist's folder, swaps the folders"), error + said());
        c.check(readFile(app + QStringLiteral("/new.txt")) == "new",
                QStringLiteral("the new copy is in the old one's folder"));
        c.check(folders() == QStringList{ QStringLiteral("Monolist") },
                QStringLiteral("and the old copy, all of whose files the new one has, is gone; the unpacked zip's "
                               "folder too"),
                folders().join(QLatin1Char(' ')));
    }

    // A file of the user's in the old folder (one the new copy does not
    // have): the old folder is kept, and said where; never deleted.
    makeCopy(app, "old");
    writeFile(app + QStringLiteral("/My notes.txt"), "mine");
    {
        AppUpdater updater(&network);
        prepare(updater, "/win");
        QString error;
        QFile::remove(result);
        const bool handed = handOver(updater, &error);
        const QString kept = QString::fromUtf8(readFile(result)).section(QLatin1Char('\n'), 1, 1).trimmed();
        c.check(handed && said().startsWith(QLatin1String("ok")) && kept.startsWith(QLatin1String("kept: ")),
                QStringLiteral("a swap that leaves files of the old folder behind says it kept it"), said());
        const QString aside = kept.mid(6);
        c.check(QFileInfo::exists(app + QStringLiteral("/new.txt"))
                    && readFile(aside + QStringLiteral("/My notes.txt")) == "mine"
                    && QFileInfo::exists(aside + QStringLiteral("/old.txt")),
                QStringLiteral("the new copy is in place, and the user's file is kept, in the old folder"), aside);
        // A launch clears a zip left unpacked, never a folder kept.
        QDir().mkpath(app + QStringLiteral(".update/Monolist"));
        QFile::copy(systemTool(QStringLiteral("whoami.exe")), app + QStringLiteral(".update/Monolist/monolist.exe"));
        AppUpdater later(&network);
        later.setTarget(WinUpdate::describeFolder(app, QString(), QString()));
        later.setDirectory(work);
        later.housekeeping();
        c.check(!QFileInfo::exists(app + QStringLiteral(".update"))
                    && QFileInfo::exists(aside + QStringLiteral("/My notes.txt")),
                QStringLiteral("a launch clears a zip left unpacked beside it, and keeps the old folder kept"));
        QDir(aside).removeRecursively();
    }

    // The user's downloads inside the copy's folder: never swapped; the zip
    // downloaded for them to copy in by hand.
    makeCopy(app, "old");
    {
        AppUpdater updater(&network);
        const UpdateTarget target = WinUpdate::describeFolder(app, QString(), QString(),
                                                              { app + QStringLiteral("/Music") });
        updater.setTarget(target);
        updater.setDirectory(work);
        updater.start(servedRelease(stand, "/win", zipName, 0), feed);
        waitUntil([&]() { return settled(updater); }, 60000);
        c.check(updater.phase() == AppUpdater::Manual && updater.message().contains(QLatin1String("your downloads"))
                    && updater.message().contains(QLatin1String("copy everything"))
                    && QFileInfo::exists(app + QStringLiteral("/old.txt"))
                    && !folders().join(QLatin1Char(' ')).contains(QLatin1String(".old-")),
                QStringLiteral("a copy whose folder holds the downloads is not swapped; the zip is for copying in"),
                describe(updater));
    }

    // The way back: the new folder held open, so it cannot be moved in.
    makeCopy(app, "old");
    {
        AppUpdater updater(&network);
        prepare(updater, "/win");
        QFile held(updater.stagedPath() + QStringLiteral("/new.txt"));
        const bool holding = held.open(QIODevice::ReadOnly);
        QString error;
        handOver(updater, &error);
        held.close();
        c.check(holding && said().startsWith(QLatin1String("failed: the new Monolist could not be put in place, so "
                                                            "the one you had is back")),
                QStringLiteral("when the new folder cannot be moved in, the helper says so"), said());
        c.check(QFileInfo::exists(app + QStringLiteral("/old.txt")),
                QStringLiteral("and the old copy is back in its folder"));
        updater.cancel();
    }

    // The way back: the new monolist.exe ends with an error at once.
    makeCopy(app, "old");
    {
        AppUpdater updater(&network);
        prepare(updater, "/winbroken");
        QString error;
        handOver(updater, &error);
        c.check(said().startsWith(QLatin1String("failed: the new Monolist would not start, so the one you had is back"))
                    && QFileInfo::exists(app + QStringLiteral("/old.txt"))
                    && !folders().join(QLatin1Char(' ')).contains(QLatin1String(".old-")),
                QStringLiteral("a new copy that will not start is taken out, and the old one put back"),
                said() + QStringLiteral(" / ") + folders().join(QLatin1Char(' ')));
        updater.cancel();
    }

    // The setup: started quietly, with this folder; a failure leaves the
    // copy as it was and starts it again.
    {
        makeCopy(app, "old");
        const QString setupName = QStringLiteral("Monolist-%1-setup-win64.exe").arg(kVersion);
        const QByteArray whoami = readFile(systemTool(QStringLiteral("whoami.exe")));
        serve(stand, "/winsetup", setupName, whoami, sha256Of(whoami));
        AppUpdater updater(&network);
        UpdateTarget installed = WinUpdate::describeFolder(app, app, QString());
        updater.setTarget(installed);
        updater.setDirectory(work);
        updater.start(servedRelease(stand, "/winsetup", setupName, whoami.size()), feed);
        waitUntil([&]() { return settled(updater); }, 60000);
        c.check(updater.phase() == AppUpdater::Ready, QStringLiteral("the setup, downloaded, is ready as it is"),
                describe(updater));
        QString error;
        handOver(updater, &error);
        c.check(said().startsWith(QLatin1String("failed: the installer stopped (code ")),
                QStringLiteral("a setup that fails is reported, and the copy left as it was"), said());
        c.check(QFileInfo::exists(app + QStringLiteral("/old.txt")), QStringLiteral("  untouched"));

        // And one that works, run as the helper runs it, recording what it
        // was given; and one changed since it was checked, never run.
        const QString fake = root + QStringLiteral("/fake setup.cmd");
        const QString record = root + QStringLiteral("/fake-setup.txt");
        writeFile(fake, "@echo %* > \"%~dp0fake-setup.txt\"\r\n@exit /b 0\r\n");
        const QString script = work + QStringLiteral("/") + AppUpdater::helperFileName();
        writeFile(script, AppUpdater::helperScript());
        const auto runSetup = [&](const QString &sha) {
            QFile::remove(result);
            QFile::remove(record);
            return runCommand(QStringLiteral("powershell.exe"),
                              { QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                                QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                                QStringLiteral("-File"), QDir::toNativeSeparators(script),
                                QStringLiteral("-Mode"), QStringLiteral("setup"),
                                QStringLiteral("-App"), QDir::toNativeSeparators(app),
                                QStringLiteral("-Result"), QDir::toNativeSeparators(result),
                                QStringLiteral("-Setup"), QDir::toNativeSeparators(fake),
                                QStringLiteral("-Sha256"), sha });
        };
        const Ran ran = runSetup(sha256Of(readFile(fake)));
        const QString given = QString::fromLocal8Bit(readFile(record)).trimmed();
        c.check(ran.code == 0 && said() == QLatin1String("ok"), QStringLiteral("a setup that works is reported"),
                said() + QString::fromLocal8Bit(ran.err));
        c.check(given.contains(QLatin1String("/VERYSILENT")) && given.contains(QLatin1String("/SUPPRESSMSGBOXES"))
                    && given.contains(QLatin1String("/NORESTART")) && given.contains(QLatin1String("/CLOSEAPPLICATIONS"))
                    && given.contains(QLatin1String("/RELAUNCH")) && given.contains(QLatin1String("/CURRENTUSER"))
                    && given.contains(QStringLiteral("/DIR=\"") + QDir::toNativeSeparators(app) + QLatin1Char('"')),
                QStringLiteral("the setup is given this folder (spaces and all), quietly, and asked to start Monolist "
                               "again"), given);
        runSetup(sha256Of("something else"));
        c.check(said().startsWith(QLatin1String("failed: the installer had changed since it was checked"))
                    && !QFileInfo::exists(record),
                QStringLiteral("a setup changed since it was checked is never run"), said());
    }
}

#endif // Q_OS_WIN

} // namespace

int runUpdateSelfTest()
{
    Checks c;
    const QString scratch = qEnvironmentVariable("MONOLIST_DATA_DIR");
    if (scratch.isEmpty()) {
        qWarning("update-test: needs MONOLIST_DATA_DIR (a scratch folder)");
        return 1;
    }
    const QString root = QDir(scratch).filePath(QStringLiteral("update-test"));
    QDir(root).removeRecursively();
    QDir().mkpath(root);

    checkRules(c);
    checkWindowsFolders(c, root);

    StandIn stand;
    if (!c.check(stand.listen(), QStringLiteral("a stand-in for GitHub on this computer")))
        return c.finish();
    QNetworkAccessManager network;
    checkDownloads(c, root, stand, network);
    checkAbout(c, root, stand);
    checkHousekeeping(c, root, network);
#ifdef Q_OS_MACOS
    checkMac(c, root, stand, network);
#endif
#ifdef Q_OS_WIN
    checkWindows(c, root, stand, network);
#endif
    return c.finish();
}

int runUpdateRun()
{
    if (qEnvironmentVariableIsEmpty("MONOLIST_DATA_DIR")) {
        qWarning("update-run: replaces this copy of Monolist, so it runs in MONOLIST_DATA_DIR only");
        return 2;
    }
    AppInfo info;
    int code = 1;
    int lastState = -1;
    int lastTen = -1;
    QObject::connect(&info, &AppInfo::updateChanged, &info, [&]() {
        const int state = info.updateState();
        const int ten = info.updateProgress() / 10;
        if (state != lastState || ten != lastTen)
            qWarning("update-run: %s", qPrintable(info.updateMessage()));
        lastState = state;
        lastTen = ten;
        switch (state) {
        case AppInfo::Available:
            if (info.canUpdateInApp())
                QTimer::singleShot(0, &info, [&info]() { info.downloadUpdate(); });
            else
                QCoreApplication::exit(1);
            break;
        case AppInfo::Ready:
            QTimer::singleShot(0, &info, [&info, &code]() {
                code = 0;
                info.restartToUpdate();
            });
            break;
        case AppInfo::UpToDate:
        case AppInfo::NotConfigured:
        case AppInfo::Failed:
        case AppInfo::DownloadFailed:
        case AppInfo::Manual:
            code = 1;
            QCoreApplication::exit(1);
            break;
        default:
            break;
        }
    });
    info.checkForUpdate();
    const int exec = QCoreApplication::exec();
    qWarning("update-run: %s", code == 0 && exec == 0 ? "handed over; quitting" : "stopped");
    return code == 0 && exec == 0 ? 0 : 1;
}
