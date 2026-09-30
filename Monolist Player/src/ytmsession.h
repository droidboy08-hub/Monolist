#pragma once

#include "accountguard.h"
#include "cookieimport.h"
#include "innertube.h"

#include <QHash>

#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

class Library;
class QNetworkCookie;

// The YouTube Music account, signed in by importing a session from the
// user's own browser (CookieImport), and never any other way.
//
// What it holds is the cookie jar of that session, kept encrypted in
// SecretStore ("ytmusic.cookies") and in memory; the settings table gets the
// account's name and nothing else, because any QML can read it and `--set`
// echoes it to the log. Nothing here logs a value: cookie names and counts
// only.
//
// The jar is the session's cookies for music.youtube.com, www.youtube.com and
// s.youtube.com, and each call is given what a browser would send to its own
// host (CookieImport::header). Beside the cookies it keeps what the session
// says of itself, where that is known (CookieImport::SessionInfo): which of
// the browser's accounts it was (X-Goog-AuthUser, from a copied request), the
// account's own visitor id and its DATASYNC_ID (from a copied request, or
// learned from YouTube Music's answer to a check that confirmed it). The
// calls that carry the account send the first as X-Goog-AuthUser (0 when not
// known), the second as their visitor id, and name a brand channel, which the
// third reveals, as context.user.onBehalfOfUser; the account's own channel is
// never named. Calls without the account send none of it.
//
// A session is only believed once YouTube Music has said so. A bad cookie is
// usually answered 200 OK with the generic, signed-out feed, so the status
// code proves nothing; what does is `logged_in` in the answer's
// serviceTrackingParams (read from an authenticated FEmusic_home), with
// account/account_menu for the name. The check runs when a session is
// imported, once at launch, and every six hours while running:
//
//   logged_in=1, or a name from the menu     Active
//   401 or 403 on any call with the account   Rejected at once
//     (but a listen report's, which is only a reason to check now)
//   logged_in=0                               asked once more; twice is Rejected
//   no answer, or one that says nothing       Unreachable: kept, asked again later
//
// Rejected deletes the stored copy and stops sending the cookies; the app
// carries on signed out, and the Settings row asks for a new import. The
// account's name stays, to say whose session ended.
//
// Only a few calls ever carry the account, and only while the session is
// Active, each behind a switch of its own in Settings → Connections, all on
// by default:
//
//  - Home's feed (Catalog, InnerTube's Auth::IfSignedIn), "Use my account for
//    Home"; its new releases stay anonymous.
//  - A song YouTube will not play signed out, "Play with my account when
//    needed": when the anonymous rungs are refused with LOGIN_REQUIRED (how
//    "Sign in to confirm you're not a bot" comes), an age check or a content
//    check, StreamResolver asks yt-dlp once more with the session's cookies
//    (TierSignedIn), in a cookies.txt file written for that one lookup
//    (openCookieFile), read back for the cookies yt-dlp rotated, and deleted
//    (closeCookieFile). Never ahead of time, and at most 120 songs an hour.
//  - A listen that counts (the Scrobbler's rule: half the song or four
//    minutes), "Send my listens to YouTube history": reported to the
//    account's history as YouTube Music's own player does (listenQualified,
//    InnerTube::playbackTracking and reportPlayback), which is what its
//    recommendations learn from.
//
// Search, lyrics, radio and everything else never carry it. The cookies go
// through a static hook (InnerTube::setAccountHook), since there are several
// InnerTube objects and all of them must agree; Set-Cookie on their answers
// rotates the jar, which is saved again 30 seconds after the last change.
//
// Every one of those calls, yt-dlp's lookups included, first waits its turn
// with the session's AccountGuard: one at a time, spaced, counted by the hour
// and the day, and none at all while YouTube has asked for a rest (a 429, a
// bot check, a 403). A rest pauses the check too; the row says until when,
// and the app plays signed out meanwhile.
//
// Exposed to QML as the "Account" singleton.
class YtmSession : public QObject
{
    Q_OBJECT
    // signedOut | checking | active | unreachable | rejected
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString accountName READ accountName NOTIFY changed)
    // The account's channel handle ("@someone"), where the account menu
    // gave one; kept beside the name.
    Q_PROPERTY(QString accountHandle READ accountHandle NOTIFY changed)
    // The state in a few words, for the row's bold line: "Signed in as …",
    // "Checking…", "Could not reach YouTube Music", "Session expired", "Not
    // signed in". Plain text; empty when signed out.
    Q_PROPERTY(QString headline READ headline NOTIFY changed)
    // Where things stand, and what to do about it, in a sentence or three;
    // styled text, may hold a link.
    Q_PROPERTY(QString statusLine READ statusLine NOTIFY changed)
    // Why the last import was refused, in a sentence; empty after one that worked.
    Q_PROPERTY(QString importError READ importError NOTIFY changed)
    // The exported file just read, by name, while Monolist offers to delete
    // it; empty once the offer is answered, and for a paste.
    Q_PROPERTY(QString importedFileName READ importedFileName NOTIFY changed)
    // Whether a sign-in outlives Monolist here: false where there is no
    // secret store yet, and the jar is kept in memory only.
    Q_PROPERTY(bool remembered READ remembered CONSTANT)
    // Settings' "Use my account for Home": whether Home asks for its feed as
    // the account. On unless turned off (the setting ytmusic.use_for_home,
    // "0" for off); it says nothing while there is no session.
    Q_PROPERTY(bool useForHome READ useForHome WRITE setUseForHome NOTIFY useForHomeChanged)
    // "Play with my account when needed": whether a song YouTube will not
    // play signed out is asked for with the account (ytmusic.play_when_needed,
    // "0" for off; on by default).
    Q_PROPERTY(bool playWhenNeeded READ playWhenNeeded WRITE setPlayWhenNeeded NOTIFY playWhenNeededChanged)
    // "Send my listens to YouTube history" (ytmusic.report_listens, "0" for
    // off; on by default).
    Q_PROPERTY(bool reportListens READ reportListens WRITE setReportListens NOTIFY reportListensChanged)
    // YouTube asked Monolist to slow down, and nothing is asked with the
    // account until the rest is over (AccountGuard). `restLine` says so in a
    // sentence, with the time it ends; empty while not resting.
    Q_PROPERTY(bool resting READ resting NOTIFY changed)
    Q_PROPERTY(QString restLine READ restLine NOTIFY changed)

public:
    enum class State { SignedOut, Checking, Active, Unreachable, Rejected };

    // How long things wait. These are the real values; only a self-test
    // changes them.
    struct Timing {
        int launchCheckMs = 4000;            // after launch, once Home has asked for its feed
        int recheckMs = 20 * 1000;           // after a first logged_in=0
        int retryMinMs = 5 * 60 * 1000;      // after no answer, doubling...
        int retryMaxMs = 60 * 60 * 1000;     // ...to an hour
        int periodicMs = 6 * 60 * 60 * 1000; // while Active
        int saveDelayMs = 30 * 1000;         // after the cookies rotate
        // How much the account may be asked, and how it rests (AccountGuard).
        AccountGuard::Limits guard;
    };

    static const QByteArray kOrigin;         // "https://music.youtube.com"

    explicit YtmSession(Library *library, QObject *parent = nullptr);
    ~YtmSession() override;

    // Installs the InnerTube hook, restores a stored session and checks it
    // a few seconds later.
    void start();
    void installHook();

    QString state() const;
    State stateValue() const { return m_state; }
    QString accountName() const { return m_name; }
    QString accountHandle() const { return m_handle; }
    QString headline() const { return m_headline; }
    QString statusLine() const { return m_statusLine; }
    QString importError() const { return m_importError; }
    QString importedFileName() const;
    bool remembered() const;
    bool useForHome() const { return m_useForHome; }
    void setUseForHome(bool use);
    // Whether a call Home makes with Auth::IfSignedIn goes as the account
    // now: the setting on, and a session YouTube Music has confirmed.
    // Changes only with sessionChanged or useForHomeChanged.
    bool accountForHome() const;
    bool playWhenNeeded() const { return m_playWhenNeeded; }
    void setPlayWhenNeeded(bool play);
    bool reportListens() const { return m_reportListens; }
    void setReportListens(bool report);
    bool resting() const { return m_guard.paused(); }
    QString restLine() const;
    // The guard every call with the account goes through (and the self-test
    // reads).
    AccountGuard &guard() { return m_guard; }
    // yt-dlp's words after a lookup with the account: "confirm you're not a
    // bot", HTTP 429, "rate-limited" rest the account like any other call's.
    void ytDlpSaid(const QString &errorOutput);

    // — playback with the account (StreamResolver's TierSignedIn) —
    //
    // Whether a song may be asked for with the account now: the setting on,
    // and a session YouTube Music has confirmed. `why`, when given, says
    // what stands in the way otherwise. Changes only with sessionChanged or
    // playWhenNeededChanged.
    bool accountForPlayback(QString *why = nullptr) const;
    // The session's cookies as a cookies.txt file for one yt-dlp lookup,
    // youtube.com's only (CookieImport::toNetscape), in a folder of its own
    // under the app's local data (cookieFolder), readable by this user
    // alone where the system can say so. Returns the session it was written
    // for and sets `path`; 0, with `error` saying why, when there is none to
    // write or it could not be written. The path, the count and the names
    // are all the log gets.
    quint64 openCookieFile(QString *path, QString *error);
    // yt-dlp is done with it. With `readBack` (it exited on its own, so its
    // jar was written back whole) what it brought back is taken into the
    // session as rotation from www.youtube.com, if the session is still the
    // one it was written for; either way the file is deleted, and tried
    // again for a few seconds should something still hold it.
    void closeCookieFile(quint64 session, const QString &path, bool readBack);
    // yt-dlp said the account's cookies are no longer valid: the session is
    // checked now, at most once in ten minutes. The check is what decides.
    void doubt(const QString &why);
    // Where those files are written: "yt-dlp-cookies" under the app's local
    // (never roaming) data folder, or under MONOLIST_DATA_DIR when that is
    // set, so a self-test never touches the real one. A folder on a network
    // drive is refused (openCookieFile says so): the file is the session in
    // plain text, and a share keeps neither this user's permissions on it
    // nor it on this computer.
    static QString cookieFolder();
    // Whether `path` is on a network drive: a UNC path or a drive Windows
    // maps to a share, or a volume of a network file system (NFS, SMB, AFP,
    // WebDAV, sshfs) elsewhere.
    static bool onNetworkDrive(const QString &path);
    // Deletes what an earlier run left there (a crash, a kill mid-lookup):
    // every file but those written in the last `sparedSecs` seconds, which
    // may be another running copy's. Returns how many went. Done by start().
    static int sweepCookieFiles(int sparedSecs = 120);

public Q_SLOTS:
    // PlaybackController::listenQualified: the song is reported to the
    // account's YouTube history, while "Send my listens to YouTube history"
    // is on and a session is confirmed; otherwise nothing is sent at all.
    void listenQualified(const QVariantMap &track, qint64 startedAt, bool chosenByUser);

public:

    // A cookies.txt file (the URL a file dialog gives, or a plain path), or
    // pasted text: a Cookie header or a request copied as cURL. False, with
    // importError saying why, when it is not a signed-in session.
    Q_INVOKABLE bool importFile(const QUrl &file);
    Q_INVOKABLE bool importText(const QString &text);
    // Forgets why the last import was refused: the panel opens, or closes,
    // clean.
    Q_INVOKABLE void clearImportError();
    // Deletes Monolist's encrypted copy and forgets the account. The user's
    // exported file is theirs, and is only ever deleted when they say so.
    Q_INVOKABLE void signOut();
    Q_INVOKABLE void checkNow();
    // The answers to the offer made after a file import. Delete removes the
    // file for good: in the Recycle Bin it would still hold the session.
    Q_INVOKABLE bool deleteImportedFile();
    Q_INVOKABLE void keepImportedFile();

    // The Authorization header for a call from `origin`, as YouTube's own
    // pages compute it: for each of SAPISID (or __Secure-3PAPISID when it is
    // missing), __Secure-1PAPISID and __Secure-3PAPISID that is present,
    // "<scheme> <ts>_<lower-case hex SHA-1 of "<ts> <value> <origin>">",
    // space-separated, as yt-dlp sends them. Empty with none of the three.
    // `cookies` are the ones the call carries (CookieImport::forRequest), so
    // the values hashed are the ones sent.
    static QByteArray authorization(const QList<CookieImport::Cookie> &cookies, qint64 timestamp,
                                    const QByteArray &origin);
    // One scheme's part of it.
    static QByteArray sidHash(const QByteArray &scheme, qint64 timestamp, const QByteArray &sid,
                              const QByteArray &origin);

    // — the InnerTube hook (see InnerTube::AccountHook) —
    qint64 admit(const InnerTube::AccountRequest &request, QString *why);
    quint64 authHeaders(const InnerTube::AccountRequest &request, InnerTube::AccountHeaders *headers);
    void callFinished(const InnerTube::AccountRequest &request, const InnerTube::AccountOutcome &outcome);
    // What kind of call a request is, for the guard's counts.
    static AccountGuard::Kind kindOf(const InnerTube::AccountRequest &request);
    void reportRejected(quint64 session, int httpStatus);
    // Set-Cookie from `host` on an answer to `session`, merged as a browser
    // would: a cookie replaces the one with its name, domain and path, and
    // one for a domain `host` may not set is ignored. `from` names who
    // brought them, for the log.
    void absorbCookies(quint64 session, const QString &host, const QList<QNetworkCookie> &cookies,
                       const char *from = "YouTube Music");

    // — for the self-tests and screenshots —
    void setTiming(const Timing &timing);
    // The session the hook is handing out now; 0 when none.
    quint64 session() const { return m_jar.isEmpty() ? 0 : m_generation; }
    const QList<CookieImport::Cookie> &jar() const { return m_jar; }
    const CookieImport::SessionInfo &info() const { return m_info; }
    bool savePending() const { return m_dirty; }
    // An invented account in a state, in memory only: no cookies, so
    // nothing is ever sent with it. "active", "checking", "unreachable",
    // "rejected" (a session that ended), "notsignedin" (an import YouTube
    // Music answered as signed out), "unreadable" (a stored copy that would
    // not open) or "signedout" (just signed out), with "+file" for the
    // offer to delete an exported file.
    void showDemo(const QString &state);

Q_SIGNALS:
    void changed();
    // A short confirmation, for the toast.
    void notice(const QString &text);
    // What the account-carrying calls send changed: signed in, out, or
    // refused. Home listens (Catalog::followAccount), and asks for its feed
    // again when that changes whose feed it is.
    void sessionChanged();
    void useForHomeChanged();
    // StreamResolver listens to both, and forgets every link it fetched with
    // the account when the account may no longer be used for playback.
    void playWhenNeededChanged();
    void reportListensChanged();
    // A listen was reported to the account's history, or could not be:
    // "reported", "failed", or "skipped" (the setting off, or no session
    // confirmed). For the self-test.
    void listenReported(const QString &videoId, const QString &outcome);
    // A check came to a verdict: "active", "again", "unreachable" or "rejected".
    void checked(const QString &outcome);

private:
    struct CheckAnswers;

    // Why a session is Rejected, which decides what the row tells the user
    // to do: a session that ended is imported again; one that was never
    // signed in is copied again, after signing in.
    enum class Ended {
        Unknown,      // before this run: only the name was left
        Refused,      // 401 or 403 on a call with it
        SignedOut,    // logged_in=0, twice
        Unreadable,   // the stored copy is not a session Monolist can use
        WontOpen,     // the stored copy does not open for this user here
    };

    void setState(State state);
    void updateStatus();
    bool importResult(const CookieImport::Result &result, const QString &source);
    void check();
    void checkFinished(const CheckAnswers &answers);
    void reject(Ended why, int httpStatus = 0);
    // What the row says after a sign-out: where the session lives on.
    static QString signedOutNotice();
    void scheduleCheck(qint64 ms);
    void save();
    void watchNetwork();
    // What a confirmed check's answers say of the session, kept if it is new.
    void learn(const CheckAnswers &answers);
    // Hands the session's visitor id to InnerTube, or takes it away: called
    // whenever the jar or what is known of it changes.
    void publishVisitor();
    InnerTube *innerTube();
    // Deletes a cookies file, trying again a few times a second apart should
    // something still hold it; the next launch's sweep takes what is left.
    void deleteCookieFile(const QString &path, int attempt = 0);
    // The listen report itself (listenQualified), for one song.
    void reportListen(const QString &videoId);

    Library *m_library = nullptr;
    Timing m_timing;
    QPointer<InnerTube> m_innerTube;
    bool m_hooked = false;

    State m_state = State::SignedOut;
    QList<CookieImport::Cookie> m_jar;   // values never logged
    CookieImport::SessionInfo m_info;    // nor these
    // Moves on with every import, sign-out and refusal, so an answer to a
    // call made for an earlier session cannot refuse or rotate this one.
    quint64 m_generation = 1;
    bool m_memoryOnly = false;
    bool m_useForHome = true;
    bool m_playWhenNeeded = true;
    bool m_reportListens = true;
    // The cookies files this run has lent yt-dlp and not yet seen deleted.
    QSet<QString> m_cookieFiles;
    // What each of them was written with, so that what yt-dlp brings back is
    // compared with what it was lent, not with a jar that rotated meanwhile.
    QHash<QString, QList<CookieImport::Cookie>> m_lent;
    AccountGuard m_guard;
    // When yt-dlp last cast doubt on the session (doubt()).
    QElapsedTimer m_lastDoubt;
    QString m_name;
    QString m_handle;
    QString m_headline;
    QString m_statusLine;
    QString m_importError;
    QString m_notice;        // what the row says when signed out
    Ended m_ended = Ended::Unknown;
    int m_endedStatus = 0;   // the HTTP status that refused it
    // Imported in this run and not yet confirmed: a refusal now says the
    // copy was never signed in, where a later one says the session ended.
    bool m_fresh = false;
    // When a check that came to no verdict is made again (Unreachable).
    QDateTime m_retryAt;
    QString m_importedFile;  // full path, for the offer to delete it

    QTimer m_checkTimer;
    bool m_checking = false;
    quint64 m_checkRun = 0;
    bool m_confirmed = false;   // a check said Active during this session
    int m_zeroes = 0;           // logged_in=0 answers in a row
    int m_failures = 0;         // checks in a row that came to no verdict

    QTimer m_saveTimer;
    bool m_dirty = false;
    bool m_watchingNetwork = false;
};
