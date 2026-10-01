#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>

class QWindow;
class YtmSession;

// Signing in to YouTube Music on Google's own page, in a window of Monolist's
// own: the owner's choice (2026-10-01) over copying a session out of a
// browser by hand, which stays as the way round when this one cannot be used.
//
// On Windows the page is shown by WebView2, the Edge engine Windows carries
// (googlesignin_win.cpp); elsewhere there is none (googlesignin_stub.cpp), and
// `available` is false. The window is the engine as it comes: its own user
// agent and client hints (which name it "Microsoft Edge WebView2"), nothing
// injected into the page, no request intercepted or rewritten, no automation
// and no debugging port; InPrivate, so the session lives in memory, in a
// fresh profile folder for each sign-in, deleted afterwards. That keeps clear
// of the faked and automated browsers on Google's list of those it may block,
// but not of all of it: the same help page (support.google.com/accounts/
// answer/7675428) lists a browser "embedded in a different application",
// which this is, so Google may refuse it at any time. What keeps the account
// safe is taking that refusal as final (below). Monolist does not
// read what is typed: only, once the window has landed on YouTube Music
// signed in, the session's cookies for YouTube, which go to the account
// (YtmSession) the same way an imported session does, encrypted with the
// Windows sign-in and checked with YouTube Music before it says Signed in.
//
// If Google refuses to sign in here ("This browser or app may not be
// secure"), that is taken as the answer: the window closes, nothing is tried
// again, and the import guide is offered instead (fallbackRequested).
//
// Exposed to QML as the "SignIn" singleton.
class GoogleSignIn : public QObject
{
    Q_OBJECT
    // There is an engine to show Google's page with on this computer.
    Q_PROPERTY(bool available READ available CONSTANT)
    // idle | opening | open | reading | done | refused | failed
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    // The window is up.
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    // Google refused to sign in here within the last week: the import is the
    // way in until then (or until forgetRefusal()).
    Q_PROPERTY(bool refused READ refused NOTIFY stateChanged)

public:
    explicit GoogleSignIn(YtmSession *session, QObject *parent = nullptr);
    ~GoogleSignIn() override;

    // The app's window, which the sign-in window belongs to and opens over.
    void setOwner(QWindow *window);

    bool available() const;
    bool refused() const;
    QString state() const { return m_state; }
    bool busy() const
    {
        return m_state == QLatin1String("opening") || m_state == QLatin1String("open")
               || m_state == QLatin1String("reading");
    }

    // Opens the window on Google's sign-in page, or brings it forward.
    Q_INVOKABLE void start();
    // Closes it, nothing kept.
    Q_INVOKABLE void cancel();
    // Lets the window be tried again before the week is out: only from a
    // control the user chose.
    Q_INVOKABLE void forgetRefusal();

    // What a sign-in left behind if Monolist stopped before deleting it: the
    // profile folders of earlier windows. Run once at launch.
    static void removeLeftovers();

    // The platform's own part: the window and its engine.
    struct Native;

Q_SIGNALS:
    void stateChanged();
    // A line for the toast.
    void notice(const QString &text);
    // This way cannot sign in here: the import guide instead.
    void fallbackRequested();

private:
    void setState(const QString &state);
    // The window's session, as cookies.txt text: handed to the account.
    void finish(const QByteArray &cookies);
    void fail(const QString &state, const QString &text, bool fallBack);

    std::unique_ptr<Native> m_native;
    QPointer<YtmSession> m_session;
    QPointer<QWindow> m_owner;
    QString m_state = QStringLiteral("idle");
    // When Google refused, in seconds; 0 for not lately.
    qint64 m_refusedAt = 0;

    friend struct Native;
};
