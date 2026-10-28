#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>
#include <memory>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QThread;

// One file of a release, as the feed lists it.
struct UpdateAsset {
    QString name;
    QUrl url;           // assets[].browser_download_url
    qint64 size = 0;    // as the feed says; 0 when it does not
    bool isNull() const { return name.isEmpty(); }
};

// The newest release, as GitHub's "latest release" answer describes it.
struct UpdateRelease {
    QString version;    // the tag without its "v": 0.1.191
    QUrl page;          // its page on GitHub, the last resort
    QList<UpdateAsset> assets;
    bool isValid() const { return !version.isEmpty(); }
};

// This copy of Monolist, as far as replacing it goes.
struct UpdateTarget {
    enum Kind {
        Unsupported,        // nothing to download for it: the release page it is
        MacBundle,          // Monolist.app, from one of the disk images
        WindowsSetup,       // installed by the setup program, which upgrades it
        WindowsPortable     // the zip unpacked somewhere
    };
    Kind kind = Unsupported;
    // The bundle (Mac) or the folder monolist.exe is in (Windows).
    QString path;
    // Mac: signed with the project's Developer ID, rather than ad hoc. The
    // update keeps to the same kind (the owner's choice: both are published).
    bool developerId = false;
    // Windows: installed for everyone on the PC (the uninstall key in HKLM),
    // so the setup asks for an administrator.
    bool allUsers = false;
    // Why this copy cannot be replaced where it is, in words that end the
    // sentence "…cannot be replaced where it is (because) <blocker>"; empty
    // when it can. The update is then downloaded and shown, for the user to
    // put in place by hand.
    QString blocker;
};

// The rules, pure, for the update and its test (--update-test).
namespace UpdateRules {

// GitHub's answer for a release; an invalid release when it names no tag.
UpdateRelease parseRelease(const QByteArray &json);

// The file this copy wants from a release, and the checksum published beside
// it ("<file>.sha256"). `missing` says why there is no file, when there is none.
struct Choice {
    UpdateAsset file;
    UpdateAsset checksum;
    QString missing;
};
Choice choose(const UpdateRelease &release, const UpdateTarget &target, const QString &arch);

// Where a download may come from: over HTTPS, from github.com or one of
// GitHub's download hosts under githubusercontent.com, and nowhere else; or,
// when the feed itself is on this computer (a test's stand-in), from this
// computer, over plain HTTP too.
bool allowedSource(const QUrl &url, const QUrl &feed);
bool isLoopback(const QUrl &url);

// The SHA-256 in a checksum file ("<hex>  <name>"): the first 64 hex digits on
// the line naming `name`, or on the only line there is; lower case. Empty when
// there is none.
QString sha256From(const QByteArray &text, const QString &name);

// "0.1.191" from "Monolist-0.1.191-macos-arm64.dmg", or empty.
QString versionInName(const QString &fileName);

} // namespace UpdateRules

// Windows' rules, kept apart from its registry so that they are checked on
// any computer (--update-test).
namespace WinUpdate {

// The same folder, written either way: case, slashes, a last separator.
bool sameFolder(const QString &a, const QString &b);

// The copy in `folder`: the setup's when one of the setup's uninstall keys
// (this user's, then everyone's) names that folder; a portable copy when
// the folder is a package's (its LICENSES\Monolist-MIT.txt) rather than a
// build folder (CMakeCache.txt); else one that cannot update itself.
UpdateTarget describeFolder(const QString &folder, const QString &currentUserLocation,
                            const QString &allUsersLocation);

} // namespace WinUpdate

// Downloads the newest Monolist for this copy, checks it, gets it ready, and
// hands over to a small helper that puts it in place once Monolist has quit
// and starts it again. Owned by AppInfo, which shows it in Settings › Updates.
//
//   1. The file for this copy (UpdateRules::choose) and its .sha256, both
//      taken from the release's own list and both from GitHub only
//      (UpdateRules::allowedSource), redirects included.
//   2. The file streamed to <name>.part as it arrives, hashed on the way, and
//      renamed to <name> only when it matches the published SHA-256; any
//      other end (a cancel, a quit, a cut, a mismatch) deletes the part.
//   3. Got ready: on a Mac the app copied out of the disk image and checked
//      (signature, identifier, newer build, the Developer ID for that kind);
//      on Windows the zip unpacked beside this copy, or nothing for the setup.
//   4. RESTART: the helper started, detached, and Monolist quits. It waits
//      for this process to end, swaps old for new with a way back, starts
//      Monolist again, and leaves a word for it (result.txt) on how it went.
//
// The Windows builds are not code-signed: there the download's integrity is
// HTTPS from GitHub and the SHA-256 published beside it, nothing more.
class AppUpdater : public QObject
{
    Q_OBJECT
public:
    enum Phase {
        Idle,
        Downloading,
        Preparing,      // the checksum checked; unpacking and checking it
        Ready,          // RESTART puts it in place
        Manual,         // downloaded, but this copy cannot be replaced where it is
        Failed
    };
    Q_ENUM(Phase)

    explicit AppUpdater(QNetworkAccessManager *network, QObject *parent = nullptr);
    ~AppUpdater() override;

    // Where downloads, the staged copy, the helper and its word live: a
    // folder of the scratch library under MONOLIST_DATA_DIR, else the cache.
    static QString directory();
    // This copy, as the platform sees it (platform files).
    static UpdateTarget describeThisCopy();
    // The build number Monolist is running ("191"), and its whole version.
    static QString runningBuild();
    static QString runningVersion();

    // This copy, described when first asked (it reads the registry, or
    // this app's signature), never at launch.
    UpdateTarget target();
    // For the test: another copy to update than this one.
    void setTarget(const UpdateTarget &target)
    {
        m_target = target;
        m_targetKnown = true;
    }
    void setDirectory(const QString &directory) { m_directory = directory; }
    QString workDirectory() const { return m_directory.isEmpty() ? directory() : m_directory; }

    Phase phase() const { return m_phase; }
    QString message() const { return m_message; }
    // 0-100 while downloading, -1 when the size is not known.
    int percent() const { return m_percent; }
    QString version() const { return m_release.version; }
    // The verified download, and (Mac, Windows zip) the copy made from it.
    QString downloadedFile() const { return m_file; }
    QString stagedPath() const { return m_staged; }

    // Whether this copy can be updated from inside the app for that release
    // (a file for it exists in the release), and why not when it cannot.
    bool canUpdate(const UpdateRelease &release, QString *why = nullptr);

    // Downloads, checks and gets ready the release's file for this copy.
    void start(const UpdateRelease &release, const QUrl &feed);
    // Stops whatever is under way and deletes what it left half done.
    void cancel();
    // The downloaded disk image opened, or the zip shown, for the user to put
    // in place by hand (Manual).
    void showDownloaded();

    // RESTART: starts the helper and returns true; Monolist should then quit
    // at once. False, with the reason, when it could not be started.
    bool handOver(QString *error);

    // What the helper has to know besides the paths, for the test: whose
    // end to wait for, what opens the app and registers it (Mac), how long
    // to wait, and whether to wait for the helper here rather than detach.
    struct HelperOptions {
        qint64 pid = 0;               // this process when 0
        QString opener;               // /usr/bin/open when empty
        QString lsregister;           // the system's when empty
        int timeoutSeconds = 60;
        bool wait = false;            // run it attached, and wait (tests)
        int waitMs = 120000;
    };
    void setHelperOptions(const HelperOptions &options) { m_helper = options; }

    // At launch: what the last RESTART came to (empty when there was none),
    // and leftovers cleared away: half downloads, a staged copy, the files of
    // releases no newer than this one. `ok` says whether it went well.
    QString housekeeping(bool *ok = nullptr);

    // The helper, as it is written to disk and how it is started; for the
    // test as well as handOver.
    static QString helperFileName();
    static QByteArray helperScript();

Q_SIGNALS:
    void changed();

private:
    void setPhase(Phase phase, const QString &message);
    void fail(const QString &message);
    void stopNetwork();
    void fetchChecksum();
    void checkExisting(const QString &sha256);
    void download(const QString &sha256);
    void downloaded();
    void stage();
    void ready();
    QNetworkReply *get(const QUrl &url, qint64 timeoutMs);
    bool writePending(QString *error) const;

    // The platform's part (appupdater_mac.mm, appupdater_win.cpp, and the
    // fallback in appupdater.cpp): the copy made from the download (a disk
    // image's app, a zip unpacked), what a cancel or a quit has to undo of
    // it, and the helper.
    void stagePlatform();
    void attachImage(int attempt);  // Mac
    void detachImage();             // Mac
    void verifyStaged();            // Mac
    void stopPlatform();
    bool launchHelper(QString *error);

    // Runs a program for the staging, keeping it to be stopped by cancel();
    // `done` gets the exit code (-1: did not start, or was stopped) and what
    // it printed.
    void run(const QString &program, const QStringList &arguments, int timeoutMs,
             const std::function<void(int code, const QByteArray &out, const QByteArray &err)> &done);

    QNetworkAccessManager *m_network;
    QString m_directory;
    UpdateTarget m_target;
    bool m_targetKnown = false;
    UpdateRelease m_release;
    QUrl m_feed;
    UpdateRules::Choice m_choice;
    HelperOptions m_helper;

    Phase m_phase = Idle;
    QString m_message;
    int m_percent = -1;
    // Each start() and cancel() begins a new round: what an older round's
    // replies, processes and threads say afterwards is not listened to.
    int m_round = 0;

    QPointer<QNetworkReply> m_reply;
    QFile *m_part = nullptr;
    std::unique_ptr<QCryptographicHash> m_hash;
    QString m_expectedSha;
    qint64 m_received = 0;
    QString m_file;          // the download, verified
    QString m_staged;        // the copy made from it
    QString m_stagingRoot;   // the folder the staging is done in
    QString m_mountPoint;    // Mac: the disk image while it is attached (its device)
    QPointer<QProcess> m_process;
    QPointer<QThread> m_worker;
    // RESTART has started the helper: the staged copy is its now.
    bool m_handedOver = false;
};
