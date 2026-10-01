#include "googlesignin.h"

#include "ytmsession.h"

// Where there is no engine to show Google's page with (not Windows, or a
// build without the WebView2 SDK): not offered, and the import guide is the
// way to sign in.
struct GoogleSignIn::Native
{
};

GoogleSignIn::GoogleSignIn(YtmSession *session, QObject *parent)
    : QObject(parent), m_native(std::make_unique<Native>()), m_session(session)
{
}

GoogleSignIn::~GoogleSignIn() = default;

void GoogleSignIn::setOwner(QWindow *window)
{
    m_owner = window;
}

bool GoogleSignIn::available() const
{
    return false;
}

bool GoogleSignIn::refused() const
{
    return false;
}

void GoogleSignIn::forgetRefusal()
{
}

void GoogleSignIn::start()
{
    fail(QStringLiteral("failed"), QString(), true);
}

void GoogleSignIn::cancel()
{
    setState(QStringLiteral("idle"));
}

void GoogleSignIn::removeLeftovers()
{
}

void GoogleSignIn::setState(const QString &state)
{
    if (state == m_state)
        return;
    m_state = state;
    Q_EMIT stateChanged();
}

void GoogleSignIn::finish(const QByteArray &cookies)
{
    if (m_session)
        m_session->importSignIn(cookies);
}

void GoogleSignIn::fail(const QString &state, const QString &text, bool fallBack)
{
    setState(state);
    if (!text.isEmpty())
        Q_EMIT notice(text);
    if (fallBack)
        Q_EMIT fallbackRequested();
}
