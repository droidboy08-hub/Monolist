#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <functional>
#include <memory>

#include "driveauth.h"
#include "librarybackup.h"

class Library;
class Recommender;
class QNetworkAccessManager;
class QNetworkReply;
class QWindow;

// The library's backup, as QML sees it (the "Backup" singleton): a file
// saved or brought back from Settings (Export, Import), with no sign-in, and
// the Google Drive backup that keeps the same file in the user's own Drive,
// in the folder only Monolist can open (appDataFolder), merged on every
// device that signs in. DriveAuth signs in; LibraryBackup writes and merges
// the file; this finds, reads and writes it in Drive, and decides when.
//
// One sync: find the file, read it (unless it has not changed since this
// computer last saw it), merge it into the library, and upload the library
// as it is then, unless that holds just what the file did. Two copies of the
// file (two computers that each made one at once) are both merged, the one
// kept is the same on every computer, and the other is deleted once its
// content is in. A file that cannot be read is never overwritten: syncing
// stops, says so, and waits for SYNC NOW.
//
// When: right after signing in; 15 s after a launch that is signed in; a
// minute after the last edit (a like, a playlist changed), or five minutes
// into a run of them; when Monolist's window comes to the front and the last
// sync is a few minutes old, so a computer that sat in the tray catches up
// before it is used; every half hour while Monolist runs; and SYNC NOW.
// Plays wait for the half-hourly sync (or any other that uploads): a song
// every few minutes is not worth the whole file sent each time. A sync with
// nothing to send and nothing new in Drive reads nothing here either. Never
// two at once, and never two within 30 s unless asked. No network, a 429, a
// 5xx or Drive's rate limit back off 1, 5, then 30 minutes (sooner when the
// system says the network is back). Quitting waits for nothing.
//
// A computer whose clock is wrong does not sync: every time in the file is
// compared with times it would write, so one running hours ahead would win
// over every other computer's edits, and one behind would lose its own.
// Google's answers say what time it is (their Date); more than five minutes
// apart from this computer's clock, the sync stops before anything is read
// or written, says so, and looks again every few minutes.
//
// Kept apart from the YouTube Music sign-in in every way: its own names, its
// own settings (backup.*), its own network manager with no cookies either
// way, the token sent to www.googleapis.com only, redirects followed only on
// the same host over https, never AccountGuard, never a YouTube address.
class DriveSync : public QObject
{
    Q_OBJECT
    // Whether Google sign-in can work here: a client built in, and
    // somewhere safe to keep the sign-in. Export and Import always work.
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    // Whether this copy was built with a Google client at all.
    Q_PROPERTY(bool hasClient READ hasClient NOTIFY stateChanged)
    // Why not, in a sentence; empty when it can.
    Q_PROPERTY(QString unavailableReason READ unavailableReason NOTIFY stateChanged)
    // The Settings row's state, in ServiceRow's words: unavailable, off,
    // waiting, connected, expired or error.
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool signedIn READ signedIn NOTIFY stateChanged)
    // The Google account, for "Signed in as …" (plain text, never markup);
    // "your Google account" when Google did not say.
    Q_PROPERTY(QString email READ email NOTIFY stateChanged)
    // A sync, or the end of a sign-in, under way.
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    // Where things stand, in a line for the row ("Synced 2 minutes ago",
    // "Waiting for you in the browser", "Couldn't reach Google Drive. Trying
    // again at 14:05"). Styled text: what came from outside is escaped, and
    // it holds a link only when the browser could not be opened.
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    // When the library and the Drive backup were last the same; invalid
    // before the first sync.
    Q_PROPERTY(QDateTime lastSyncedAt READ lastSyncedAt NOTIFY stateChanged)
    // What needs the user, in a sentence (the status line says the same);
    // empty when nothing does.
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    // SYNC NOW would do something: signed in, and no sync running.
    Q_PROPERTY(bool canSyncNow READ canSyncNow NOTIFY stateChanged)
    // Syncing has stopped on something only the user can end (a file held
    // unread, a Drive that is full), so the row's line is red; one it gets
    // past by itself, by waiting (no network, a busy Drive), stays grey.
    Q_PROPERTY(bool attention READ attention NOTIFY stateChanged)
    // SIGN OUT could not delete the sign-in here: trying again is another
    // SIGN OUT, never a new sign-in.
    Q_PROPERTY(bool signOutFailed READ signOutFailed NOTIFY stateChanged)
    // Unavailable here (no client in this build), yet a sign-in an earlier
    // copy made is still kept: SIGN OUT can withdraw it all the same.
    Q_PROPERTY(bool storedSignIn READ storedSignIn NOTIFY stateChanged)
    // Waiting on a browser that could not be opened: COPY LINK puts Google's
    // page on the clipboard to paste into one.
    Q_PROPERTY(bool browserFailed READ browserFailed NOTIFY stateChanged)

public:
    // What stopped the last sync, while it waits to try again.
    enum class Problem {
        None,
        Offline,     // no answer: no network, or Google unreachable
        Slowed,      // 429, or 403 with Drive's rate-limit reason
        Server,      // 5xx
        Full,        // the Drive has no room (403 storageQuotaExceeded)
        Refused,     // Google refused this build's client
        Odd,         // an answer Monolist does not follow: a redirect elsewhere, a 4xx
        Database,    // the library would not take the merge
        Unreadable,  // the file in Drive is not a library Monolist can read: held
        Newer,       // it is one a newer Monolist wrote: held
        Clock        // this computer's clock is more than five minutes from Google's
    };

    // How long things wait. These are the real values; only a self-test
    // changes them.
    struct Timing {
        int launchMs = 15 * 1000;
        int editMs = 60 * 1000;
        int editMaxMs = 5 * 60 * 1000;
        int periodMs = 30 * 60 * 1000;
        int gapMs = 30 * 1000;
        QList<int> backoffMs{ 60 * 1000, 5 * 60 * 1000, 30 * 60 * 1000 };
        int timeoutMs = 30 * 1000;
        int tickMs = 30 * 1000;   // the "Synced N minutes ago" line kept true
        // The window brought to the front syncs when the last sync started
        // longer ago than this.
        int staleMs = 2 * 60 * 1000;
        // A clock found wrong is looked at again this often, until it is
        // right (each look is one list of the files, nothing read).
        int clockRetryMs = 5 * 60 * 1000;
    };
    // How far this computer's clock may be from Google's, in seconds.
    static constexpr qint64 kClockToleranceSecs = 5 * 60;

    static const QString kFileName;   // monolist-library.json

    explicit DriveSync(Library *library, QObject *parent = nullptr);
    ~DriveSync() override;

    LibraryBackup *backup() { return &m_backup; }
    DriveAuth *auth() { return m_auth; }
    // Told of turn-downs a merge brings or takes away.
    void setRecommender(Recommender *recommender);
    // The window the Save and Open dialogs belong to, and the one brought
    // forward when the browser comes back from Google.
    void setOwner(QWindow *window);
    // Tests answer the Save or Open dialog in its place: the path chosen, or
    // empty for a dialog cancelled. `suggested` is the file name offered.
    using FilePicker = std::function<QString(bool save, const QString &suggested)>;
    void setFilePicker(FilePicker picker);

    // Where things stood at the last run, then the launch's sync, the half-
    // hourly one and the network watched. Before this, nothing is sent.
    void start();

    bool available() const { return m_auth->available(); }
    bool hasClient() const { return m_auth->hasClient(); }
    QString unavailableReason() const { return m_auth->unavailableReason().toHtmlEscaped(); }
    QString state() const;
    bool signedIn() const { return m_auth->state() == DriveAuth::State::SignedIn; }
    QString email() const;
    bool busy() const;
    QString status() const;
    QDateTime lastSyncedAt() const { return m_lastSyncedAt; }
    QString error() const;
    // In the demo the button looks as it would, and does nothing.
    bool canSyncNow() const { return signedIn() && !busy(); }
    Problem problem() const { return m_problem; }
    bool held() const { return m_problem == Problem::Unreadable || m_problem == Problem::Newer; }
    bool attention() const
    {
        return signedIn() && (held() || m_problem == Problem::Full || m_problem == Problem::Clock);
    }
    bool signOutFailed() const
    {
        return m_auth->state() == DriveAuth::State::Failed && m_auth->failure() == DriveAuth::Failure::Store;
    }
    bool storedSignIn() const
    {
        return m_auth->state() == DriveAuth::State::Unavailable && m_auth->storedSignIn();
    }
    bool browserFailed() const
    {
        return m_auth->state() == DriveAuth::State::Waiting && !m_auth->browserOpened();
    }
    // This computer's clock less Google's, in seconds, as the last answer
    // that said found it; 0 before one has.
    qint64 clockSkewSecs() const { return m_clockSkewSecs; }

    // Settings' SIGN IN WITH GOOGLE (and SIGN IN AGAIN): Google's page in
    // the browser, then the first sync.
    Q_INVOKABLE void signIn();
    // CANCEL while the browser is open.
    Q_INVOKABLE void cancelSignIn();
    // COPY LINK, when the browser could not be opened: Google's sign-in
    // page on the clipboard, to paste into one. Never logged.
    Q_INVOKABLE void copySignInLink();
    // SIGN OUT: the sign-in withdrawn and forgotten here, with the backup's
    // own settings. The file in Drive and the library here stay.
    Q_INVOKABLE void signOut();
    // SYNC NOW: at once, whatever waits; reads the file even when it seems
    // unchanged, and tries again a file that could not be read.
    Q_INVOKABLE void syncNow();
    // Settings' EXPORT…: the system's Save dialog, then the library written
    // to the file chosen, and a line for the toast.
    Q_INVOKABLE void exportLibrary();
    // Settings' IMPORT…: the system's Open dialog, then the file read,
    // checked and merged into the library, and what changed in the toast.
    Q_INVOKABLE void importLibrary();

    // A sync when the rules allow one: never two at once, nor within the gap
    // of the last, nor while backing off or held.
    void requestSync();

    // Whether the library changed here since a sync last read it.
    bool editedSinceSync() const { return m_editedSinceSync; }

    // — for the self-tests and the Settings demo —
    void setTiming(const Timing &timing);
    // Drive's stand-in, honoured only with DriveAuth's test client and only
    // on this computer, as DriveAuth's endpoints are.
    void setTestDrive(const QUrl &base);
    // https://www.googleapis.com, or the stand-in.
    QUrl driveBase() const;
    // The Settings row in one state, with an invented account and nothing
    // stored, read or sent (--backup-demo): off, waiting, nobrowser,
    // finishing, synced, syncing, offline, slowed, unreadable, newer, clock,
    // expired, denied, noscope, signedout, nostore, nobuild, nobuildsignedin.
    bool showDemo(const QString &state);

public Q_SLOTS:
    // The library changed on this computer: Library::edited, a turn-down
    // made or taken back, or a file imported. Uploaded a minute after the
    // last of a run of them.
    void noteLibraryEdited();
    // A play recorded (PlaybackController::playRecorded): kept to upload
    // with the half-hourly sync, or any other that uploads, never one of
    // its own.
    void notePlayed();

Q_SIGNALS:
    // A line for the toast: what Export or Import did, or why not, and what
    // the first sync after signing in (or SYNC NOW) brought in. `sticky`:
    // it stays until put away, as the first sync's does, which may come
    // while the window is still behind the browser.
    void notice(const QString &text, bool sticky = false);
    // noteLibraryEdited, for anything else that waits on it.
    void libraryEdited();
    void stateChanged();

private:
    struct Remote {
        QString id;
        QString modified;   // modifiedTime and size, as Drive gave them
    };
    struct Run;
    struct Answer {
        int status = 0;            // 0: no answer, or not all of one
        QByteArray body;
        QString reason;            // Drive's error reason, when it gave one
        qint64 retryAfterMs = 0;
        bool redirectRefused = false;
        bool tooLarge = false;
        QDateTime serverTime;      // its Date, in UTC; invalid without one
    };
    using Done = std::function<void(const Answer &answer)>;

    // The dialog, or the test's picker: a path, or empty when cancelled.
    QString pickFile(bool save, const QString &suggested);
    void runExport();
    void runImport();

    // — a sync —
    void startRun(bool forced);
    void find();
    void found(const Answer &answer);
    void downloadNext();
    void downloaded(const Answer &answer);
    void mergeAndUpload();
    void upload(const QByteArray &content);
    void uploaded(const Answer &answer);
    void removeExtras();
    void finishRun();
    // The run ends on `problem`; most wait and try again (as long as the
    // answer's Retry-After, when it says longer).
    void failRun(Problem problem);
    void failRun(Problem problem, const Answer &answer);
    void endRun();
    void stopRun();
    // One request to Drive with the access token, asked for again once on a
    // 401 (DriveAuth refreshes it). A sign-in that has ended ends the run.
    void send(const QByteArray &verb, const QUrl &url, const QByteArray &body, const QByteArray &type, Done done,
              bool retried = false);
    bool mayCarryToken(const QUrl &url) const;
    Problem problemOf(const Answer &answer) const;

    void authChanged();
    void signedInNow();
    void watchNetwork();
    void watchWindow();
    void forgetSyncState();
    QString setting(const QString &key) const;
    void setSetting(const QString &key, const QString &value);
    QString retryText() const;
    // "5 hours fast", "12 minutes slow": the clock's error, for the row.
    QString skewText() const;

    Library *m_library = nullptr;
    LibraryBackup m_backup;
    DriveAuth *m_auth = nullptr;
    QNetworkAccessManager *m_network = nullptr;
    QPointer<QWindow> m_owner;
    FilePicker m_picker;
    // The folder the last dialog was left in, for the next one.
    QString m_folder;
    // A dialog is open, or its file being written or read: a second press
    // while it is waits for nothing.
    bool m_fileBusy = false;
    bool m_editedSinceSync = false;
    // Plays recorded since the last upload, waiting for one.
    bool m_playedSinceSync = false;
    // The half-hourly sync is owed: it uploads the plays waiting.
    bool m_periodDue = false;
    // A sync has finished since launch: until one has, the library may hold
    // what the last run of Monolist had not yet sent.
    bool m_syncedThisLaunch = false;

    Timing m_timing;
    QUrl m_testDrive;
    bool m_started = false;
    bool m_watchingNetwork = false;
    bool m_watchingWindow = false;
    qint64 m_clockSkewSecs = 0;
    QElapsedTimer m_clock;
    QTimer m_syncTimer;     // the next sync the rules allow
    QTimer m_editTimer;     // a minute after the last edit
    QTimer m_periodTimer;   // every half hour
    QTimer m_tickTimer;     // the status line's "N minutes ago"
    qint64 m_lastStartMs = -1;
    qint64 m_notBeforeMs = 0;
    qint64 m_firstEditMs = -1;
    quint64 m_editSerial = 0;
    int m_failures = 0;
    Problem m_problem = Problem::None;
    QDateTime m_retryAt;
    QDateTime m_lastSyncedAt;
    std::unique_ptr<Run> m_run;
    quint64 m_runSerial = 0;
    QPointer<QNetworkReply> m_reply;
    // Asked for while a sync ran: another once it ends (SYNC NOW's at once).
    bool m_again = false;
    bool m_againForced = false;
    // The first sync after signing in says what it brought in, and SYNC NOW
    // what it did: both owed until a sync gets all the way through, which
    // may be a retry on its own a minute later. What the merges brought in
    // meanwhile is kept with them, since a sync that merged and then could
    // not upload has changed the library all the same, and its retry finds
    // nothing new to tell.
    bool m_firstAfterSignIn = false;
    bool m_forcedPending = false;
    LibraryBackup::Counts m_pendingCounts;
    bool m_justSignedOut = false;
    bool m_demo = false;
    bool m_demoBusy = false;
};
