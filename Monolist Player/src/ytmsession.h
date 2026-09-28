#pragma once

#include "cookieimport.h"
#include "innertube.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>

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
//   logged_in=0                               asked once more; twice is Rejected
//   no answer, or one that says nothing       Unreachable: kept, asked again later
//
// Rejected deletes the stored copy and stops sending the cookies; the app
// carries on signed out, and the Settings row asks for a new import. The
// account's name stays, to say whose session ended.
//
// Only a few calls ever carry the account: InnerTube's Auth::IfSignedIn,
// while the session is Active, on the Music client. Today that is Home's
// feed (Catalog), while "Use my account for Home" is on; its new releases
// stay anonymous. Playback, search, lyrics, radio and yt-dlp never do.
// The cookies go through a static hook
// (InnerTube::setAccountHook), since there are several InnerTube objects and
// all of them must agree; Set-Cookie on their answers rotates the jar, which
// is saved again 30 seconds after the last change.
//
// Exposed to QML as the "Account" singleton.
class YtmSession : public QObject
{
    Q_OBJECT
    // signedOut | checking | active | unreachable | rejected
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString accountName READ accountName NOTIFY changed)
    // Where things stand, in a sentence; styled text, may hold a link.
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
    quint64 authHeaders(const InnerTube::AccountRequest &request, InnerTube::AccountHeaders *headers);
    void reportRejected(quint64 session, int httpStatus);
    // Set-Cookie from `host` on an answer to `session`, merged as a browser
    // would: a cookie replaces the one with its name, domain and path, and
    // one for a domain `host` may not set is ignored.
    void absorbCookies(quint64 session, const QString &host, const QList<QNetworkCookie> &cookies);

    // — for the self-tests and screenshots —
    void setTiming(const Timing &timing);
    // The session the hook is handing out now; 0 when none.
    quint64 session() const { return m_jar.isEmpty() ? 0 : m_generation; }
    const QList<CookieImport::Cookie> &jar() const { return m_jar; }
    const CookieImport::SessionInfo &info() const { return m_info; }
    bool savePending() const { return m_dirty; }
    // An invented account in a state, in memory only: no cookies, so
    // nothing is ever sent with it. "active", "checking", "unreachable" or
    // "rejected", with "+file" for the offer to delete an exported file.
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
    // A check came to a verdict: "active", "again", "unreachable" or "rejected".
    void checked(const QString &outcome);

private:
    struct CheckAnswers;

    void setState(State state);
    void updateStatus();
    bool importResult(const CookieImport::Result &result, const QString &source);
    void check();
    void checkFinished(const CheckAnswers &answers);
    void reject(const QString &reason);
    void scheduleCheck(qint64 ms);
    void save();
    void watchNetwork();
    // What a confirmed check's answers say of the session, kept if it is new.
    void learn(const CheckAnswers &answers);
    // Hands the session's visitor id to InnerTube, or takes it away: called
    // whenever the jar or what is known of it changes.
    void publishVisitor();
    InnerTube *innerTube();

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
    QString m_name;
    QString m_statusLine;
    QString m_importError;
    QString m_notice;        // what the row says when signed out
    QString m_rejectReason;
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
