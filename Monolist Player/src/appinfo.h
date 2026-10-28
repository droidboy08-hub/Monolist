#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVariantList>

#include "appupdater.h"

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class ToolStore;

// What this copy of the app is, and how it is kept current.
//
// Three separate questions live here because the user asked them as three:
// which build am I running, is there a newer one, and are the open-source
// tools it plays through up to date. The last is its own thing on purpose —
// what usually goes stale in a player like this is not the player, it is
// yt-dlp, and a version of yt-dlp from six months ago is the difference
// between a track playing and a track failing.
//
// Exposed to QML as the "About" singleton.
class AppInfo : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QString buildNumber READ buildNumber CONSTANT)
    Q_PROPERTY(QString commit READ commit CONSTANT)
    Q_PROPERTY(QString buildDate READ buildDate CONSTANT)
    Q_PROPERTY(bool modified READ modified CONSTANT)
    Q_PROPERTY(QString qtVersion READ qtVersion CONSTANT)
    // "0.1 (build 29)" — the one line to quote in a bug report.
    Q_PROPERTY(QString fullVersion READ fullVersion CONSTANT)

    // The open-source tools this plays through, each {name, version, role}.
    // Empty until refreshComponents() has answered: finding a version means
    // starting a process, which is not something to do while the app launches,
    // nor anything to wait for.
    Q_PROPERTY(QVariantList components READ components NOTIFY componentsChanged)
    Q_PROPERTY(bool componentsKnown READ componentsKnown NOTIFY componentsChanged)

    Q_PROPERTY(int updateState READ updateState NOTIFY updateChanged)
    Q_PROPERTY(QString updateMessage READ updateMessage NOTIFY updateChanged)
    // The release's page on GitHub: the last resort when the app cannot
    // update itself, or did not manage to.
    Q_PROPERTY(QString updateUrl READ updateUrl NOTIFY updateChanged)
    // What the interface asks about, named rather than compared against an
    // enum value that reads as a magic number in QML.
    Q_PROPERTY(bool checking READ checking NOTIFY updateChanged)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY updateChanged)
    // Available, and this copy can download and install it itself (UPDATE);
    // otherwise the release page is where it is (GET IT).
    Q_PROPERTY(bool canUpdateInApp READ canUpdateInApp NOTIFY updateChanged)
    Q_PROPERTY(bool downloading READ downloading NOTIFY updateChanged)
    // Downloaded and checked against its checksum; being unpacked, checked.
    Q_PROPERTY(bool preparing READ preparing NOTIFY updateChanged)
    // RESTART puts it in place.
    Q_PROPERTY(bool readyToRestart READ readyToRestart NOTIFY updateChanged)
    // Downloaded, for the user to put in place by hand.
    Q_PROPERTY(bool manualUpdate READ manualUpdate NOTIFY updateChanged)
    // The download or its checks failed: TRY AGAIN.
    Q_PROPERTY(bool updateFailed READ updateFailed NOTIFY updateChanged)
    // 0-100 while downloading; -1 when the size is not known.
    Q_PROPERTY(int updateProgress READ updateProgress NOTIFY updateChanged)

    Q_PROPERTY(int toolsState READ toolsState NOTIFY toolsChanged)
    Q_PROPERTY(QString toolsMessage READ toolsMessage NOTIFY toolsChanged)
    Q_PROPERTY(bool toolsBusy READ toolsBusy NOTIFY toolsChanged)
    Q_PROPERTY(bool canUpdateTools READ canUpdateTools CONSTANT)
    // What Update components does here, or why there is nothing to do.
    Q_PROPERTY(QString toolsDescription READ toolsDescription CONSTANT)

public:
    // Idle covers "not asked yet"; the rest are what an answer can be.
    enum State {
        Idle = 0,
        Working,        // a request or a process is out
        UpToDate,
        Available,      // something newer exists
        NotConfigured,  // no release feed has been set, so there is nothing to ask
        Failed,         // the check failed
        Downloading,    // the update for this copy is arriving
        Preparing,      // and being unpacked and checked
        Ready,          // RESTART puts it in place
        Manual,         // downloaded, but this copy cannot be replaced where it is
        DownloadFailed  // the download or its checks failed
    };
    Q_ENUM(State)

    explicit AppInfo(QObject *parent = nullptr);

    QString version() const;
    QString buildNumber() const;
    QString commit() const;
    QString buildDate() const;
    bool modified() const;
    QString qtVersion() const;
    QString fullVersion() const;

    QVariantList components() const { return m_components; }
    bool componentsKnown() const { return m_componentsKnown; }

    int updateState() const { return m_updateState; }
    QString updateMessage() const { return m_updateMessage; }
    QString updateUrl() const { return m_updateUrl; }
    bool checking() const { return m_updateState == Working; }
    bool updateAvailable() const { return m_updateState == Available; }
    bool canUpdateInApp() const { return m_updateState == Available && m_canUpdateInApp; }
    bool downloading() const { return m_updateState == Downloading; }
    bool preparing() const { return m_updateState == Preparing; }
    bool readyToRestart() const { return m_updateState == Ready; }
    bool manualUpdate() const { return m_updateState == Manual; }
    bool updateFailed() const { return m_updateState == DownloadFailed; }
    int updateProgress() const { return m_updateProgress; }

    int toolsState() const { return m_toolsState; }
    QString toolsMessage() const { return m_toolsMessage; }
    bool toolsBusy() const { return m_toolsState == Working; }
    bool canUpdateTools() const;
    QString toolsDescription() const;

    // Where a release feed lives, if one has been set. Stored as the setting
    // `update.feed`; a GitHub releases API URL is the shape expected.
    static QString updateFeed();
    static void setUpdateFeed(const QString &url);

    // Negative, zero or positive as `left` is older than, the same as or newer
    // than `right`: "1.2.10" after "1.2.9", "2026.08.19.1" after "2026.08.19".
    static int compareVersions(const QString &left, const QString &right);

    // This build as releases are tagged ("0.1.191"), and its build number.
    static QString currentRelease();
    static QString currentBuild();

    // The in-app update's own machinery, made on first use; for the test.
    AppUpdater *updater();

public Q_SLOTS:
    // Asks the versions of the bundled tools, one process each, all at once
    // and in the background; componentsChanged says when every one has
    // answered. Done when the Settings page first opens, never at start-up.
    void refreshComponents();
    // Is there a newer Monolist? Answers NotConfigured when no feed is set,
    // which is the honest answer rather than a spinner that never resolves.
    void checkForUpdate();
    // The "advanced" update: brings the bundled open-source tools up to date
    // by running the same setup script that installed them.
    void updateTools();
    // The release page, in the browser: the last resort.
    void openUpdatePage();
    // UPDATE: downloads the update for this copy, checks it and gets it
    // ready (Downloading, Preparing, then Ready or Manual); TRY AGAIN after
    // a failure. Falls back to the release page when this copy cannot.
    void downloadUpdate();
    // CANCEL: stops the download, and deletes what it left.
    void cancelUpdate();
    // RESTART: hands over to the helper, which puts the update in place once
    // Monolist has quit and starts it again; and quits. A separate press,
    // never automatic: it stops the music.
    void restartToUpdate();
    // Manual: the downloaded disk image or zip shown again.
    void showDownloadedUpdate();
    // At launch, once the window is up: what the last RESTART came to (a
    // toast), and the update's leftovers cleared away.
    void startUpdateHousekeeping();
    // --update-demo <state>: Settings › Updates in that state (available,
    // downloading, preparing, ready, manual, failed, getit), with an invented
    // release and nothing fetched, for a look or a screenshot.
    bool showUpdateDemo(const QString &state);
    // macOS: once a day, update yt-dlp quietly and announce anything else
    // newer (ToolStore). Nothing elsewhere, where the setup script does it.
    void startToolChecks();
    // For a bug report: the version line, the build, and every component.
    QString report() const;
    // The same, on the clipboard, because nobody retypes a build number
    // correctly and a wrong one sends whoever reads it the wrong way. Asked
    // before the versions are in, it copies what is known at once and swaps
    // in the whole report when they arrive, if nothing was copied since.
    void copyReport();

Q_SIGNALS:
    void componentsChanged();
    void updateChanged();
    void toolsChanged();
    // A sentence for the toast: something was updated, or could be.
    void notice(const QString &text);
    // The library was found damaged at launch (AppDatabase::recoveryNote),
    // and where its damaged copy is kept.
    void recovered(const QString &text, const QString &folder);
    // The setup script has run, so the tools on disk may not be the ones the
    // rest of the app looked for at launch.
    void toolsUpdated();
    // The update is downloaded and ready: a toast with RESTART, wherever the
    // user has gone while it downloaded.
    void updateReady(const QString &text);

private:
    void setUpdate(int state, const QString &message, const QString &url = {});
    void followUpdater();
    void setTools(int state, const QString &message);
    // Starts one tool's --version; its first line lands in m_versionAnswers
    // under `tool`, or nothing when it cannot be run or does not answer.
    void askVersion(const QString &tool, const QString &program, const QStringList &arguments);
    void publishComponents();
    static QString setupScriptPath();

    QNetworkAccessManager *m_network = nullptr;
    QPointer<QNetworkReply> m_updateReply;
    QPointer<QProcess> m_toolsProcess;
    ToolStore *m_toolStore = nullptr;   // macOS only

    QVariantList m_components;
    bool m_componentsKnown = false;
    // The version read in flight: which round it is (a newer one discards an
    // older one's answers), how many processes are still out, what they said.
    int m_versionRound = 0;
    int m_versionsPending = 0;
    QHash<QString, QString> m_versionAnswers;
    QList<QPointer<QProcess>> m_versionProcesses;
    // A report copied before the versions were in, exactly as copied; empty
    // when there is none waiting to be completed.
    QString m_partialReport;

    int m_updateState = Idle;
    QString m_updateMessage;
    QString m_updateUrl;
    int m_updateProgress = -1;
    // The release the last check found, and the feed it came from.
    UpdateRelease m_release;
    QUrl m_releaseFeed;
    bool m_canUpdateInApp = false;
    AppUpdater *m_updater = nullptr;

    int m_toolsState = Idle;
    QString m_toolsMessage;
};
