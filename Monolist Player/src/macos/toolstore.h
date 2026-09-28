#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;

// yt-dlp and Deno on a Mac: shipped inside the app, kept current outside it.
//
// These two change faster than the app does — an old yt-dlp is the usual
// reason a track stops playing — so they cannot live only inside Monolist.app:
// a signed app may not be changed, and macOS calls one that was damaged. The
// app carries each as its official release archive (Contents/Resources/tools,
// put there by scripts/build-macos.sh, with the versions in tools.json) and
// unpacks it on first launch into
//
//     ~/Library/Application Support/Monolist/Monolist/tools/<name>
//
// which is where updates go too. Whichever is newer wins: an update outlives
// the app it was fetched by, and a newer app replaces older unpacked copies.
//
// Updates come from each project's GitHub releases, checked against the
// SHA-256 the project publishes. The latest version is read from where GitHub
// redirects the "latest" download link, which needs no API and no rate limit.
//
// FFmpeg is not here: it is Homebrew's, inside the app beside the executable,
// and it is updated with the app, since YouTube's changes do not reach it.
class ToolStore : public QObject
{
    Q_OBJECT
public:
    explicit ToolStore(QNetworkAccessManager *network, QObject *parent = nullptr);

    // ~/Library/Application Support/Monolist/Monolist/tools
    static QString directory();
    // The folders the executables are in once unpacked, for YtDlp to search.
    static QStringList executableDirectories();

    // Unpacks what the app carries wherever nothing as new is installed yet.
    // Synchronous: it only has work to do on the first launch of a new app,
    // and it has to be done before anything looks for yt-dlp.
    static void installBundled();

    bool busy() const { return m_busy; }

    // Brings the named tools (every one when empty) to their latest releases,
    // reporting each step with progress(), then finished().
    void update(const QStringList &names = {});

    // At most once a day: yt-dlp is updated without asking, because an old
    // one simply stops working; a newer Deno is announced with notice().
    void checkDaily();

Q_SIGNALS:
    void progress(const QString &message);
    void finished(bool ok, const QString &message);
    void notice(const QString &text);

private:
    void next();
    void fail(const QString &message);
    void latestVersion(const QString &name,
                       const std::function<void(const QString &version, const QString &error)> &done);
    void fetchChecksum(const QString &version);
    void fetchArchive(const QString &version, const QString &sha256);
    void install(const QString &version, const QString &archive, const QString &sha256);

    QNetworkAccessManager *m_network;
    QPointer<QNetworkReply> m_reply;
    QFile *m_download = nullptr;
    bool m_busy = false;
    bool m_announce = false;        // the daily check: say what it updated
    QStringList m_queue;            // tools still to look at
    QString m_current;              // the one being looked at
    QStringList m_updated;          // "yt-dlp 2026.08.19", for the last word
};
