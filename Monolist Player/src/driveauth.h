#pragma once

#include <QByteArray>
#include <QDeadlineTimer>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <functional>

class Library;
class QNetworkAccessManager;
class QTcpServer;
class QTcpSocket;

// The Google Drive backup's sign-in: "Sign in with Google" in the user's own
// browser, for one thing only, a private folder of Monolist's in their Drive
// (drive.appdata), and who they are (openid email) for the Settings row. Never
// a YouTube scope, and nothing of the YouTube Music sign-in's (GoogleSignIn,
// YtmSession): its own names, its own network manager, its own secret
// (backup.google) and settings (backup.google_*), never AccountGuard, never a
// youtube.com address. Signing out of one leaves the other as it is.
//
// The installed-app flow Google documents, by hand, since Qt NetworkAuth is
// not part of the toolchain: a listener on 127.0.0.1 at a port the system
// picks, Google's page opened in the browser with a PKCE challenge (S256 of a
// 64-character verifier) and a random state, and the one request that comes
// back to the listener on "/" with that state taken, within ten minutes;
// anything else there gets a 404 and is ignored. The code it brings is
// exchanged with the verifier, the scopes granted are checked (Google lets
// the user untick the Drive box), and the refresh token goes to SecretStore.
// The access token lives in memory only, and is refreshed a minute before it
// runs out, or when Drive refuses it (DriveSync asks once).
//
// Nothing here logs a token, the code, the verifier, the state, the client's
// id or secret, an id_token, the address the browser is given or the email:
// only states and counts. What Google's token endpoint sends back is never
// logged whole.
class DriveAuth : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Unavailable,  // no client in this build, or nowhere safe to keep a sign-in
        SignedOut,
        Waiting,      // Google's page is open in the browser; the listener waits
        Finishing,    // the browser came back with a code, being exchanged
        SignedIn,
        Expired,      // the sign-in ended: revoked, run out, or unreadable here
        Failed        // the last sign-in did not complete; why() says why
    };
    enum class Failure { None, Denied, NoDriveScope, TimedOut, Refused, NoAnswer, Store };

    // What asking for an access token came to.
    enum class Token {
        Ok,
        Offline,     // Google could not be reached, or answered 5xx: later
        Refused,     // Google refused this build's client: later, and say so
        Ended        // there is no sign-in any more (or never was)
    };
    using TokenDone = std::function<void(Token result, const QByteArray &accessToken)>;

    // How long things wait. These are the real values; only a self-test
    // changes them.
    struct Timing {
        int signInWaitMs = 10 * 60 * 1000;
        int refreshAheadMs = 60 * 1000;
        int timeoutMs = 30 * 1000;
        int browserSocketMs = 15 * 1000;   // one connection to the listener, at most
    };

    static const QString kSecretName;       // backup.google
    static const QString kEmailKey;         // backup.google_email
    static const QString kSettingsPrefix;   // backup.google_
    static const QString kDriveScope;       // https://www.googleapis.com/auth/drive.appdata

    explicit DriveAuth(Library *library, QObject *parent = nullptr);
    ~DriveAuth() override;

    // Where things stood at the last run: signed in (the refresh token is in
    // SecretStore), ended, or signed out.
    void start();

    // A client was built in (cmake/apicredentials.cmake).
    bool hasClient() const;
    // Signing in can work here: a client, and somewhere safe to keep it.
    bool available() const;
    // Why not, in a sentence; empty when it can.
    QString unavailableReason() const;

    State state() const { return m_state; }
    Failure failure() const { return m_failure; }
    // The Google account's email, from the id_token, for the Settings row
    // only; empty when Google did not say.
    QString email() const { return m_email; }
    // Why the sign-in ended or did not complete, in a sentence that may be
    // shown as styled text; empty otherwise.
    QString why() const { return m_why; }
    // Unavailable, but a sign-in an earlier copy made is still kept here
    // (one built with a client, before this one without): left for a copy
    // that can use it, and signOut() can still withdraw it, which needs only
    // the token, never the client.
    bool storedSignIn() const { return m_storedSignIn; }
    // While waiting: whether the browser opened, and the page it was given,
    // for a link when it did not. Never logged.
    bool browserOpened() const { return m_opened; }
    QUrl signInPage() const { return m_page; }

    // Opens Google's page in the browser and waits for it to come back.
    void signIn();
    // Stops waiting; the listener closes.
    void cancel();
    // Revokes the sign-in at Google (best effort, not waited for), deletes
    // the refresh token and the backup.google_* settings. False, with why()
    // set, when the stored sign-in could not be deleted. Works where signing
    // in cannot (storedSignIn()), and leaves it Unavailable there.
    bool signOut();

    // The access token, from memory while it has a minute left, otherwise
    // refreshed first. `done` always runs once, on a later turn of the event
    // loop; several asking during one refresh share it.
    void accessToken(TokenDone done);
    // Drive refused it (401): the next accessToken() refreshes.
    void forgetAccessToken();

    // Google's endpoints, or the stand-ins a self-test gives: honoured only
    // for a test client (setTestClient), and only on this computer, so that
    // the build's client and a real refresh token go to Google and nowhere
    // else, whatever a test or a stray call asked for.
    QUrl authorizeEndpoint() const;
    QUrl tokenEndpoint() const;
    QUrl revokeEndpoint() const;
    bool testing() const { return m_testClient; }
    // http(s) to localhost or a loopback address.
    static bool isLoopback(const QUrl &url);

    // — for the self-tests and the Settings demo —
    void setTestClient(const QByteArray &clientId, const QByteArray &clientSecret);
    void setTestEndpoints(const QUrl &authorize, const QUrl &token, const QUrl &revoke);
    // In place of opening the browser; returns whether it "opened".
    void setUrlOpener(std::function<bool(const QUrl &)> opener);
    void setTiming(const Timing &timing);
    // Shown in that state with nothing stored, read or sent, for a look at
    // the Settings row (--backup-demo).
    void showDemo(State state, const QString &email, const QString &why, bool client = true, bool store = true,
                  bool opened = true, bool stored = false);

Q_SIGNALS:
    void stateChanged();
    // A sign-in just completed: the first sync goes now.
    void signedIn();
    // The browser came back to Monolist, signed in or not: its window comes
    // forward.
    void returned();

private:
    using Answer = std::function<void(int status, const QJsonObject &body)>;

    void setState(State state);
    void fail(Failure failure, const QString &why);
    void stopListening();
    void acceptBrowser();
    void readBrowser(QTcpSocket *socket, QByteArray &buffer);
    void answerBrowser(QTcpSocket *socket, int status, const QString &title, const QString &text);
    void exchange(const QString &code);
    void exchanged(int status, const QJsonObject &body);
    void refresh();
    void refreshed(int status, const QJsonObject &body);
    void settleWaiters(Token result);
    void expire(const QString &why);
    void revoke(const QByteArray &token);
    // A form POSTed to one of Google's (or a stand-in's) OAuth endpoints;
    // `done` gets the HTTP status (0 for no answer) and the JSON answer.
    void post(const QUrl &url, const QList<QPair<QByteArray, QByteArray>> &form, Answer done);
    QByteArray clientId() const;
    QByteArray clientSecret() const;
    bool storeAvailable() const;
    QString setting(const QString &key) const;
    void setSetting(const QString &key, const QString &value);

    Library *m_library = nullptr;
    QNetworkAccessManager *m_network = nullptr;
    Timing m_timing;
    State m_state = State::SignedOut;
    // What a cancelled sign-in goes back to: signed out, or ended, with why
    // it ended (SIGN IN AGAIN, then CANCEL, still says it).
    State m_resting = State::SignedOut;
    QString m_restingWhy;
    Failure m_failure = Failure::None;
    QString m_why;
    QString m_email;
    bool m_storedSignIn = false;

    // — a sign-in under way; never logged —
    QTcpServer *m_server = nullptr;
    QPointer<QTcpSocket> m_landing;   // the browser's request, answered once the code is exchanged
    QByteArray m_verifier;
    QByteArray m_stateValue;
    QString m_redirectUri;
    QUrl m_page;
    bool m_opened = false;
    QTimer m_waitTimer;
    int m_signInGeneration = 0;
    std::function<bool(const QUrl &)> m_openUrl;

    // — the access token, in memory only —
    QByteArray m_accessToken;
    QDeadlineTimer m_accessUntil;
    bool m_refreshing = false;
    int m_refreshGeneration = 0;
    QList<TokenDone> m_waiters;

    // — the client, and the self-tests' stand-ins —
    bool m_testClient = false;
    QByteArray m_testClientId;
    QByteArray m_testClientSecret;
    QUrl m_testAuthorize;
    QUrl m_testToken;
    QUrl m_testRevoke;
    bool m_demo = false;
    bool m_demoClient = true;
    bool m_demoStore = true;
};
