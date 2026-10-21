#include "driveauth.h"

#include "appdatabase.h"
#include "library.h"
#include "secretstore.h"

// The Google client's id and secret: this file is the only one that asks
// apicredentials.h for them (see apicredentials.h.in), so it is the only
// one that could ever print them.
#define MONOLIST_WANT_GOOGLE_CLIENT
#include "apicredentials.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>

#include <memory>
#include <utility>

const QString DriveAuth::kSecretName = QStringLiteral("backup.google");
const QString DriveAuth::kEmailKey = QStringLiteral("backup.google_email");
const QString DriveAuth::kSettingsPrefix = QStringLiteral("backup.google_");
const QString DriveAuth::kDriveScope = QStringLiteral("https://www.googleapis.com/auth/drive.appdata");

namespace {

const QString kAuthorize = QStringLiteral("https://accounts.google.com/o/oauth2/v2/auth");
const QString kToken = QStringLiteral("https://oauth2.googleapis.com/token");
const QString kRevoke = QStringLiteral("https://oauth2.googleapis.com/revoke");
// Everything asked for, and nothing more: the app's own folder in Drive, and
// the email the Settings row shows. Never a YouTube scope.
const QString kScopes = QStringLiteral("openid email ") + DriveAuth::kDriveScope;

// A request line and its headers, which is all a browser sends here; more is
// not a browser coming back from Google.
constexpr int kMaxRequestBytes = 16 * 1024;
// Connections to the listener at once: a browser opens a few (the page,
// its icon, a speculative one); anything past this is not the user's.
constexpr int kMaxBrowserSockets = 16;

// RFC 7636's unreserved characters, from the system's own random source:
// the verifier proves at the token endpoint that whoever exchanges the code
// is whoever asked for it.
QByteArray randomText(int length)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    constexpr int count = int(sizeof(alphabet)) - 1;
    QByteArray text(length, Qt::Uninitialized);
    QRandomGenerator *random = QRandomGenerator::system();
    for (int i = 0; i < length; ++i)
        text[i] = alphabet[random->bounded(count)];
    return text;
}

QByteArray challengeOf(const QByteArray &verifier)
{
    return QCryptographicHash::hash(verifier, QCryptographicHash::Sha256)
        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

// Equal or not, in a time that does not depend on where they differ.
bool sameSecret(const QByteArray &a, const QByteArray &b)
{
    if (a.size() != b.size() || a.isEmpty())
        return false;
    unsigned char difference = 0;
    for (qsizetype i = 0; i < a.size(); ++i)
        difference |= static_cast<unsigned char>(a.at(i) ^ b.at(i));
    return difference == 0;
}

QByteArray formBody(const QList<QPair<QByteArray, QByteArray>> &form)
{
    QByteArray body;
    for (const auto &field : form) {
        if (!body.isEmpty())
            body += '&';
        body += QUrl::toPercentEncoding(QString::fromLatin1(field.first)) + '='
                + QUrl::toPercentEncoding(QString::fromUtf8(field.second));
    }
    return body;
}

// The email in an id_token's payload. Shown in Settings and nothing else, so
// the signature is not checked: the token came straight from Google's token
// endpoint over TLS, which OpenID Connect allows to stand in for it.
QString emailOf(const QString &idToken)
{
    const QList<QByteArray> parts = idToken.toLatin1().split('.');
    if (parts.size() < 2)
        return QString();
    const QByteArray payload = QByteArray::fromBase64(parts.at(1), QByteArray::Base64UrlEncoding);
    const QString email = QJsonDocument::fromJson(payload).object().value(QStringLiteral("email")).toString().trimmed();
    // An address, as shown; nothing that could be taken for markup or a
    // second line.
    if (email.size() > 320 || email.contains(QLatin1Char('<')) || email.contains(QLatin1Char('\n'))
        || !email.contains(QLatin1Char('@')))
        return QString();
    return email;
}

bool grantsDrive(const QString &scope)
{
    return scope.split(QLatin1Char(' '), Qt::SkipEmptyParts).contains(DriveAuth::kDriveScope);
}

QString userAgent()
{
    return QStringLiteral("Monolist/%1 (+https://github.com/droidboy08-hub/Monolist)")
        .arg(QCoreApplication::applicationVersion().isEmpty() ? QStringLiteral("0.1")
                                                              : QCoreApplication::applicationVersion());
}

// The page the browser shows once it is back: Monolist's paper and ink,
// one rule, one red mark, and nothing to load, link to or run.
QByteArray browserPage(const QString &title, const QString &text)
{
    const QString html = QStringLiteral(
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Monolist</title><style>"
        "html{background:#f3f2f2;color:#201e1d}"
        "@media (prefers-color-scheme:dark){html{background:#141312;color:#ebe9e8}}"
        "body{margin:0;font:16px/1.5 Archivo,\"Segoe UI\",Helvetica,Arial,sans-serif}"
        "main{max-width:34rem;margin:18vh auto 0;padding:0 24px}"
        ".mark{font-size:11px;font-weight:800;letter-spacing:.12em;text-transform:uppercase;margin:0 0 16px}"
        ".mark i{display:inline-block;width:10px;height:10px;background:#ec3013;margin-right:8px}"
        "h1{font-size:28px;font-weight:800;line-height:1.15;margin:0;padding-bottom:16px;"
        "border-bottom:2px solid currentColor}"
        "p{margin:16px 0 0}"
        "</style></head><body><main><p class=\"mark\"><i></i>Monolist</p><h1>%1</h1><p>%2</p></main></body></html>")
                             .arg(title.toHtmlEscaped(), text.toHtmlEscaped());
    return html.toUtf8();
}

} // namespace

DriveAuth::DriveAuth(Library *library, QObject *parent)
    : QObject(parent)
    , m_library(library)
    , m_network(new QNetworkAccessManager(this))
{
    m_waitTimer.setSingleShot(true);
    connect(&m_waitTimer, &QTimer::timeout, this, [this]() {
        if (m_state != State::Waiting)
            return;
        qInfo("backup: the Google sign-in was not finished in time; the listener is closed");
        stopListening();
        fail(Failure::TimedOut, QStringLiteral("Monolist stopped waiting for Google. Sign in again when you're ready"));
    });
}

DriveAuth::~DriveAuth()
{
    stopListening();
}

// --------------------------------------------------------------- the client

QByteArray DriveAuth::clientId() const
{
    return m_testClient ? m_testClientId : QByteArrayLiteral(MONOLIST_GOOGLE_CLIENT_ID);
}

QByteArray DriveAuth::clientSecret() const
{
    return m_testClient ? m_testClientSecret : QByteArrayLiteral(MONOLIST_GOOGLE_CLIENT_SECRET);
}

bool DriveAuth::hasClient() const
{
    if (m_demo)
        return m_demoClient;
    return !clientId().isEmpty() && !clientSecret().isEmpty();
}

bool DriveAuth::storeAvailable() const
{
    return m_demo ? m_demoStore : SecretStore::available();
}

bool DriveAuth::available() const
{
    return hasClient() && storeAvailable();
}

QString DriveAuth::unavailableReason() const
{
    if (!hasClient())
        return QStringLiteral("This copy of Monolist was built without Google sign-in");
    if (!storeAvailable())
        return QStringLiteral("Signing in to Google Drive isn't available on this computer yet");
    return QString();
}

void DriveAuth::setTestClient(const QByteArray &clientId, const QByteArray &clientSecret)
{
    m_testClientId = clientId;
    m_testClientSecret = clientSecret;
    m_testClient = true;
}

void DriveAuth::setTestEndpoints(const QUrl &authorize, const QUrl &token, const QUrl &revoke)
{
    m_testAuthorize = authorize;
    m_testToken = token;
    m_testRevoke = revoke;
}

void DriveAuth::setUrlOpener(std::function<bool(const QUrl &)> opener)
{
    m_openUrl = std::move(opener);
}

void DriveAuth::setTiming(const Timing &timing)
{
    m_timing = timing;
}

bool DriveAuth::isLoopback(const QUrl &url)
{
    if (!url.isValid() || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https")))
        return false;
    const QString host = url.host();
    return host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0 || QHostAddress(host).isLoopback();
}

QUrl DriveAuth::authorizeEndpoint() const
{
    return m_testClient && isLoopback(m_testAuthorize) ? m_testAuthorize : QUrl(kAuthorize);
}

QUrl DriveAuth::tokenEndpoint() const
{
    return m_testClient && isLoopback(m_testToken) ? m_testToken : QUrl(kToken);
}

QUrl DriveAuth::revokeEndpoint() const
{
    return m_testClient && isLoopback(m_testRevoke) ? m_testRevoke : QUrl(kRevoke);
}

// ------------------------------------------------------------------ state

QString DriveAuth::setting(const QString &key) const
{
    return m_library ? m_library->settingValue(key) : QString();
}

void DriveAuth::setSetting(const QString &key, const QString &value)
{
    if (m_library)
        m_library->setSetting(key, value);
}

void DriveAuth::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    Q_EMIT stateChanged();
}

void DriveAuth::start()
{
    if (m_demo)
        return;
    // The email is there, even empty (Google need not say), from the first
    // sign-in until signing out: whether this computer was ever signed in.
    const QString never = QStringLiteral("\n");
    const QString stored = m_library ? m_library->settingValue(kEmailKey, never) : never;
    const bool signedInBefore = stored != never;
    m_email = signedInBefore ? stored : QString();
    if (!available()) {
        // A copy built with a client may have signed in here before this
        // one, built without: that sign-in is kept for a copy that can use
        // it, but SIGN OUT must still be able to withdraw it, and the row
        // must not say there is none.
        QByteArray refreshToken;
        m_storedSignIn = SecretStore::read(kSecretName, &refreshToken) == SecretStore::Status::Ok || signedInBefore;
        qInfo("backup: Google Drive sign-in is not available here (%s)%s",
              hasClient() ? "no secret store" : "this build has no client",
              m_storedSignIn ? "; an earlier copy's sign-in is kept" : "");
        setState(State::Unavailable);
        return;
    }
    QByteArray refreshToken;
    QString error;
    switch (SecretStore::read(kSecretName, &refreshToken, &error)) {
    case SecretStore::Status::Ok:
        setState(State::SignedIn);
        break;
    case SecretStore::Status::NotFound:
        // Signed in before, and the sign-in went (Google ended it): the row
        // says so and offers SIGN IN AGAIN, rather than looking never used.
        if (!signedInBefore) {
            setState(State::SignedOut);
        } else {
            m_why = QStringLiteral("Your Google sign-in has ended. Sign in again to go on syncing");
            setState(State::Expired);
        }
        break;
    case SecretStore::Status::Corrupt:
        qWarning("backup: the stored Google sign-in does not open here (%s); it needs signing in again",
                 qPrintable(error));
        m_why = QStringLiteral("Your Google sign-in couldn't be opened on this computer. Sign in again to go on syncing");
        setState(State::Expired);
        break;
    default:
        qWarning("backup: the stored Google sign-in could not be read (%s)", qPrintable(error));
        m_why = QStringLiteral("Monolist couldn't read your Google sign-in: %1").arg(error.toHtmlEscaped());
        setState(State::Expired);
        break;
    }
    m_resting = m_state == State::SignedIn ? State::SignedOut : m_state;
    qInfo("backup: Google Drive backup: %s", m_state == State::SignedIn ? "signed in"
                                             : m_state == State::Expired ? "the sign-in has ended" : "not signed in");
}

void DriveAuth::showDemo(State state, const QString &email, const QString &why, bool client, bool store,
                         bool opened, bool stored)
{
    m_demo = true;
    m_demoClient = client;
    m_demoStore = store;
    m_storedSignIn = stored;
    m_email = email;
    m_why = why;
    m_opened = opened;
    // The browser that would not open gets a link to Google's front page,
    // never a real sign-in's address.
    m_page = opened ? QUrl() : QUrl(QStringLiteral("https://accounts.google.com/"));
    m_state = state;
    Q_EMIT stateChanged();
}

void DriveAuth::fail(Failure failure, const QString &why)
{
    m_failure = failure;
    m_why = why;
    m_resting = State::Failed;
    setState(State::Failed);
}

// ---------------------------------------------------------------- signing in

void DriveAuth::signIn()
{
    if (m_demo)
        return;
    if (m_state == State::Unavailable || !available()) {
        qInfo("backup: no Google sign-in in this build or on this computer; nothing opened");
        return;
    }
    if (m_state == State::Waiting || m_state == State::Finishing || m_state == State::SignedIn)
        return;
    m_resting = m_state;
    m_restingWhy = m_why;
    stopListening();

    // 127.0.0.1 and no other address, at a port the system picks: Google's
    // loopback redirect for installed apps, reachable from this computer
    // only.
    m_server = new QTcpServer(this);
    m_server->setMaxPendingConnections(kMaxBrowserSockets);
    if (!m_server->listen(QHostAddress::LocalHost, 0)) {
        qWarning("backup: could not listen on this computer for Google's answer: %s",
                 qPrintable(m_server->errorString()));
        stopListening();
        fail(Failure::NoAnswer, QStringLiteral("Monolist couldn't get ready for Google's answer on this computer. Try again"));
        return;
    }
    connect(m_server, &QTcpServer::newConnection, this, &DriveAuth::acceptBrowser);
    m_redirectUri = QStringLiteral("http://127.0.0.1:%1").arg(m_server->serverPort());
    m_verifier = randomText(64);
    m_stateValue = randomText(32);
    ++m_signInGeneration;

    QUrl page = authorizeEndpoint();
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("client_id"), QString::fromLatin1(clientId()));
    query.addQueryItem(QStringLiteral("redirect_uri"), m_redirectUri);
    query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    query.addQueryItem(QStringLiteral("scope"), kScopes);
    query.addQueryItem(QStringLiteral("code_challenge"), QString::fromLatin1(challengeOf(m_verifier)));
    query.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    query.addQueryItem(QStringLiteral("state"), QString::fromLatin1(m_stateValue));
    // A refresh token every time (prompt=consent with offline access), so a
    // second sign-in on the same account still brings one; and only these
    // scopes, never others the account granted another time.
    query.addQueryItem(QStringLiteral("access_type"), QStringLiteral("offline"));
    query.addQueryItem(QStringLiteral("prompt"), QStringLiteral("consent"));
    query.addQueryItem(QStringLiteral("include_granted_scopes"), QStringLiteral("false"));
    page.setQuery(query);
    m_page = page;

    m_failure = Failure::None;
    m_why.clear();
    m_waitTimer.start(m_timing.signInWaitMs);
    setState(State::Waiting);
    m_opened = m_openUrl ? m_openUrl(m_page) : QDesktopServices::openUrl(m_page);
    // Never the address: it carries the client and the challenge.
    qInfo("backup: Google's sign-in page %s; waiting for it",
          m_opened ? "is open in the browser" : "could not be opened");
    Q_EMIT stateChanged();
}

void DriveAuth::cancel()
{
    if (m_state != State::Waiting && m_state != State::Finishing)
        return;
    ++m_signInGeneration;
    stopListening();
    qInfo("backup: the Google sign-in was cancelled");
    // Back as it was: an ended sign-in still says why it needs signing in
    // again; a failed one is put away.
    const State back = m_resting == State::Failed ? State::SignedOut : m_resting;
    m_why = back == State::Expired ? m_restingWhy : QString();
    setState(back);
}

void DriveAuth::stopListening()
{
    m_waitTimer.stop();
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
    if (m_landing) {
        m_landing->abort();
        m_landing->deleteLater();
    }
    m_landing.clear();
    m_verifier.clear();
    m_stateValue.clear();
    m_redirectUri.clear();
    m_page.clear();
}

void DriveAuth::acceptBrowser()
{
    QTcpServer *server = m_server;
    if (!server)
        return;
    while (QTcpSocket *socket = server->nextPendingConnection()) {
        if (server->findChildren<QTcpSocket *>().size() > kMaxBrowserSockets) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer]() {
            *buffer += socket->readAll();
            readBrowser(socket, *buffer);
        });
        // A connection that never finishes its request is not left open.
        QTimer::singleShot(m_timing.browserSocketMs, socket, [this, socket]() {
            if (socket != m_landing)
                socket->abort();
        });
    }
}

void DriveAuth::answerBrowser(QTcpSocket *socket, int status, const QString &title, const QString &text)
{
    if (!socket)
        return;
    const QByteArray body = status == 200 ? browserPage(title, text) : QByteArrayLiteral("Not found");
    // The address the browser is on holds the code: no page it could go to
    // next is told it, and nothing is kept.
    const QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + (status == 200 ? " OK" : " Not Found")
                            + "\r\nContent-Type: " + (status == 200 ? "text/html; charset=utf-8" : "text/plain")
                            + "\r\nContent-Length: " + QByteArray::number(body.size())
                            + "\r\nCache-Control: no-store"
                              "\r\nReferrer-Policy: no-referrer"
                              "\r\nContent-Security-Policy: default-src 'none'; style-src 'unsafe-inline'"
                              "\r\nConnection: close\r\n\r\n";
    socket->write(head + body);
    socket->disconnectFromHost();
}

// Only the request Google sends the browser back with counts: a GET of "/"
// carrying this sign-in's state. Anything else is a 404 and changes nothing,
// so a page elsewhere that guesses the port can neither end the wait nor
// slip in a code of its own.
void DriveAuth::readBrowser(QTcpSocket *socket, QByteArray &buffer)
{
    if (buffer.size() > kMaxRequestBytes) {
        socket->abort();
        return;
    }
    const qsizetype end = buffer.indexOf("\r\n\r\n");
    if (end < 0)
        return;
    const QList<QByteArray> requestLine = buffer.left(buffer.indexOf("\r\n")).split(' ');
    buffer.clear();
    disconnect(socket, &QTcpSocket::readyRead, this, nullptr);
    const auto notFound = [this, socket]() { answerBrowser(socket, 404, QString(), QString()); };

    if (m_state != State::Waiting || requestLine.size() != 3 || requestLine.at(0) != "GET"
        || !requestLine.at(1).startsWith('/')) {
        notFound();
        return;
    }
    const QUrl target(QStringLiteral("http://127.0.0.1") + QString::fromLatin1(requestLine.at(1)));
    if (!target.isValid() || target.path() != QLatin1String("/")) {
        notFound();
        return;
    }
    const QUrlQuery query(target);
    const QByteArray state = query.queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded).toUtf8();
    if (!sameSecret(state, m_stateValue)) {
        qInfo("backup: a request to the sign-in listener did not carry this sign-in's state; ignored");
        notFound();
        return;
    }
    const QString error = query.queryItemValue(QStringLiteral("error"), QUrl::FullyDecoded);
    const QString code = query.queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded);
    if (!error.isEmpty()) {
        const bool denied = error == QLatin1String("access_denied");
        // Google's error codes are fixed words, never anything of the user's.
        qInfo("backup: Google's sign-in came back refused (%s)",
              qPrintable(QString(error).remove(QRegularExpression(QStringLiteral("[^a-z_]")))));
        // Kept past the listener, which closes now, until the page is sent
        // and the browser hangs up.
        socket->setParent(this);
        answerBrowser(socket, 200, denied ? QStringLiteral("Monolist isn't connected")
                                          : QStringLiteral("Monolist couldn't connect"),
                      denied ? QStringLiteral("You didn't allow Monolist its folder in your Google Drive. You can "
                                              "close this tab; TRY AGAIN in Monolist's Settings asks again.")
                             : QStringLiteral("Google didn't finish the sign-in. You can close this tab; TRY "
                                              "AGAIN in Monolist's Settings starts it again."));
        stopListening();
        fail(denied ? Failure::Denied : Failure::Refused,
             denied ? QStringLiteral("You didn't allow it, so Monolist isn't signed in to Google Drive")
                    : QStringLiteral("Google didn't finish the sign-in. Try again"));
        Q_EMIT returned();
        return;
    }
    if (code.isEmpty()) {
        notFound();
        return;
    }
    // This one is answered once the code is exchanged, so the page says
    // what really happened; the listener takes nothing more.
    m_landing = socket;
    socket->setParent(this);
    m_waitTimer.stop();
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
    setState(State::Finishing);
    exchange(code);
}

void DriveAuth::exchange(const QString &code)
{
    qInfo("backup: Google sent the browser back; finishing the sign-in");
    const int generation = m_signInGeneration;
    post(tokenEndpoint(),
         { { "grant_type", "authorization_code" },
           { "code", code.toUtf8() },
           { "code_verifier", m_verifier },
           { "client_id", clientId() },
           { "client_secret", clientSecret() },
           { "redirect_uri", m_redirectUri.toUtf8() } },
         [this, generation](int status, const QJsonObject &body) {
             if (generation != m_signInGeneration || m_state != State::Finishing) {
                 // Cancelled while Google answered: what it granted is given
                 // back rather than left unused on the account.
                 revoke(body.value(QStringLiteral("refresh_token")).toString().toUtf8());
                 return;
             }
             exchanged(status, body);
         });
}

void DriveAuth::exchanged(int status, const QJsonObject &body)
{
    // The browser's page says what happened, and the socket goes once the
    // browser hangs up; the sign-in's one-time values go with it.
    const auto finish = [this](const QString &title, const QString &text) {
        answerBrowser(m_landing, 200, title, text);
        m_landing.clear();
        m_verifier.clear();
        m_stateValue.clear();
        m_redirectUri.clear();
        m_page.clear();
        Q_EMIT returned();
    };
    const QByteArray accessToken = body.value(QStringLiteral("access_token")).toString().toUtf8();
    const QByteArray refreshToken = body.value(QStringLiteral("refresh_token")).toString().toUtf8();
    if (status != 200 || accessToken.isEmpty()) {
        const QString error = body.value(QStringLiteral("error")).toString();
        qWarning("backup: Google did not finish the sign-in (HTTP %d%s)", status,
                 error.isEmpty() ? "" : qPrintable(QStringLiteral(", ") + QString(error).remove(
                                                       QRegularExpression(QStringLiteral("[^a-z_]")))));
        finish(QStringLiteral("Monolist couldn't connect"),
               QStringLiteral("Google didn't finish the sign-in. You can close this tab; TRY AGAIN in Monolist's "
                              "Settings starts it again."));
        fail(status == 0 ? Failure::NoAnswer : Failure::Refused,
             status == 0 ? QStringLiteral("Couldn't reach Google to finish signing in. Try again")
                         : QStringLiteral("Google didn't finish the sign-in. Try again"));
        return;
    }
    // Google lets the user untick the Drive box and still go on: then there
    // is nothing to back up into, and the sign-in is given back at once.
    // Google shows that box unticked, beside the email's, which is ticked
    // for the user: the words say to tick it, not to leave it ticked, and
    // name the button the row offers next (TRY AGAIN).
    if (!grantsDrive(body.value(QStringLiteral("scope")).toString())) {
        qInfo("backup: the sign-in came back without the Drive folder's permission; it is revoked");
        revoke(refreshToken.isEmpty() ? accessToken : refreshToken);
        finish(QStringLiteral("Monolist needs its folder"),
               QStringLiteral("Monolist keeps your library in a folder of its own in your Google Drive, and the box "
                              "for it wasn't ticked. You can close this tab. In Monolist's Settings, press TRY AGAIN "
                              "and, on Google's page, tick the box for Monolist's folder before Continue."));
        fail(Failure::NoDriveScope,
             QStringLiteral("Monolist needs its folder in your Google Drive. Press TRY AGAIN and, on Google's page, "
                            "tick the box for Monolist's folder before Continue"));
        return;
    }
    if (refreshToken.isEmpty()) {
        qWarning("backup: Google's answer had no refresh token; the sign-in is given back");
        revoke(accessToken);
        finish(QStringLiteral("Monolist couldn't connect"),
               QStringLiteral("Google didn't give Monolist a sign-in it can keep. You can close this tab; TRY AGAIN "
                              "in Monolist's Settings starts it again."));
        fail(Failure::Refused, QStringLiteral("Google didn't give Monolist a sign-in it can keep. Try again"));
        return;
    }
    QString error;
    if (SecretStore::write(kSecretName, refreshToken, &error) != SecretStore::Status::Ok) {
        qWarning("backup: the Google sign-in could not be kept: %s", qPrintable(error));
        revoke(refreshToken);
        finish(QStringLiteral("Monolist couldn't connect"),
               QStringLiteral("Monolist couldn't keep the sign-in safely on this computer. You can close this tab."));
        fail(Failure::Store, QStringLiteral("Monolist couldn't keep the sign-in safely: %1").arg(error.toHtmlEscaped()));
        return;
    }
    m_email = emailOf(body.value(QStringLiteral("id_token")).toString());
    setSetting(kEmailKey, m_email);
    m_accessToken = accessToken;
    const qint64 lifeMs = qint64(body.value(QStringLiteral("expires_in")).toDouble(3600)) * 1000;
    m_accessUntil = QDeadlineTimer(qMax<qint64>(0, lifeMs - m_timing.refreshAheadMs));
    m_failure = Failure::None;
    m_why.clear();
    m_resting = State::SignedOut;
    finish(QStringLiteral("Monolist is connected to Google Drive"),
           QStringLiteral("You can close this tab and go back to Monolist."));
    qInfo("backup: signed in to Google Drive");
    setState(State::SignedIn);
    Q_EMIT signedIn();
}

// ------------------------------------------------------------------ tokens

void DriveAuth::accessToken(TokenDone done)
{
    if (m_demo || m_state != State::SignedIn) {
        QTimer::singleShot(0, this, [done]() { done(Token::Ended, QByteArray()); });
        return;
    }
    if (!m_accessToken.isEmpty() && !m_accessUntil.hasExpired()) {
        const QByteArray token = m_accessToken;
        QTimer::singleShot(0, this, [done, token]() { done(Token::Ok, token); });
        return;
    }
    m_waiters.append(std::move(done));
    if (!m_refreshing)
        refresh();
}

void DriveAuth::forgetAccessToken()
{
    m_accessToken.clear();
    m_accessUntil = QDeadlineTimer(0);
}

void DriveAuth::refresh()
{
    QByteArray refreshToken;
    QString error;
    const SecretStore::Status read = SecretStore::read(kSecretName, &refreshToken, &error);
    if (read != SecretStore::Status::Ok) {
        qWarning("backup: the stored Google sign-in could not be read (%s)",
                 qPrintable(SecretStore::statusText(read)));
        expire(QStringLiteral("Your Google sign-in couldn't be read on this computer. Sign in again to go on syncing"));
        return;
    }
    m_refreshing = true;
    const int generation = ++m_refreshGeneration;
    post(tokenEndpoint(),
         { { "grant_type", "refresh_token" },
           { "refresh_token", refreshToken },
           { "client_id", clientId() },
           { "client_secret", clientSecret() } },
         [this, generation](int status, const QJsonObject &body) {
             if (generation != m_refreshGeneration)
                 return;
             m_refreshing = false;
             refreshed(status, body);
         });
}

void DriveAuth::refreshed(int status, const QJsonObject &body)
{
    if (m_state != State::SignedIn) {
        settleWaiters(Token::Ended);
        return;
    }
    const QByteArray accessToken = body.value(QStringLiteral("access_token")).toString().toUtf8();
    if (status == 200 && !accessToken.isEmpty()) {
        // A refresh may say which scopes the token carries; one without the
        // folder's (taken back in the account's settings) can do nothing.
        const QString scope = body.value(QStringLiteral("scope")).toString();
        if (!scope.isEmpty() && !grantsDrive(scope)) {
            qInfo("backup: the Google sign-in no longer has the Drive folder's permission");
            expire(QStringLiteral("Monolist no longer has its folder in your Google Drive. Press SIGN IN AGAIN and, "
                                  "on Google's page, tick the box for Monolist's folder before Continue"));
            return;
        }
        m_accessToken = accessToken;
        const qint64 lifeMs = qint64(body.value(QStringLiteral("expires_in")).toDouble(3600)) * 1000;
        m_accessUntil = QDeadlineTimer(qMax<qint64>(0, lifeMs - m_timing.refreshAheadMs));
        qInfo("backup: the Google access token was renewed");
        settleWaiters(Token::Ok);
        return;
    }
    const QString error = body.value(QStringLiteral("error")).toString();
    if (error == QLatin1String("invalid_grant")) {
        // Revoked, run out (a test client's last a week), or the password
        // changed: there is nothing to refresh with any more.
        qInfo("backup: Google ended the sign-in (invalid_grant); the stored sign-in is deleted");
        expire(QStringLiteral("Your Google sign-in has ended. Sign in again to go on syncing"));
        return;
    }
    if (status >= 400 && status < 500 && !error.isEmpty()) {
        qWarning("backup: Google refused to renew the access token (HTTP %d, %s)", status,
                 qPrintable(QString(error).remove(QRegularExpression(QStringLiteral("[^a-z_]")))));
        settleWaiters(Token::Refused);
        return;
    }
    qInfo("backup: Google could not renew the access token now (HTTP %d)", status);
    settleWaiters(Token::Offline);
}

void DriveAuth::settleWaiters(Token result)
{
    const QList<TokenDone> waiters = std::exchange(m_waiters, {});
    const QByteArray token = result == Token::Ok ? m_accessToken : QByteArray();
    for (const TokenDone &done : waiters) {
        QTimer::singleShot(0, this, [done, result, token]() { done(result, token); });
    }
}

void DriveAuth::expire(const QString &why)
{
    forgetAccessToken();
    QString error;
    if (SecretStore::remove(kSecretName, &error) != SecretStore::Status::Ok)
        qWarning("backup: the ended Google sign-in could not be deleted: %s", qPrintable(error));
    m_failure = Failure::None;
    m_why = why;
    m_resting = State::Expired;
    setState(State::Expired);
    settleWaiters(Token::Ended);
}

// ------------------------------------------------------------- signing out

bool DriveAuth::signOut()
{
    if (m_demo)
        return true;
    ++m_signInGeneration;
    ++m_refreshGeneration;
    m_refreshing = false;
    stopListening();
    QByteArray refreshToken;
    SecretStore::read(kSecretName, &refreshToken);
    if (!refreshToken.isEmpty())
        revoke(refreshToken);
    else if (!m_accessToken.isEmpty())
        revoke(m_accessToken);
    forgetAccessToken();
    QString error;
    // Nowhere to keep a sign-in here means none was kept: nothing to delete.
    if (storeAvailable() && SecretStore::remove(kSecretName, &error) != SecretStore::Status::Ok) {
        qWarning("backup: the Google sign-in could not be deleted from this computer: %s", qPrintable(error));
        m_why = QStringLiteral("Monolist couldn't delete your Google sign-in from this computer: %1. Try again")
                    .arg(error.toHtmlEscaped());
        m_failure = Failure::Store;
        setState(State::Failed);
        settleWaiters(Token::Ended);
        return false;
    }
    // Only the backup's own: never ytmusic.*, signin.* or anything of the
    // YouTube Music sign-in's.
    QSqlQuery remove(AppDatabase::connection());
    remove.prepare(QStringLiteral("DELETE FROM settings WHERE substr(key, 1, ?) = ?"));
    remove.addBindValue(int(kSettingsPrefix.size()));
    remove.addBindValue(kSettingsPrefix);
    remove.exec();
    m_email.clear();
    m_why.clear();
    m_failure = Failure::None;
    m_resting = State::SignedOut;
    m_storedSignIn = false;
    qInfo("backup: signed out of Google Drive; the stored sign-in is deleted");
    // A copy that cannot sign in stays one that cannot, now with nothing
    // kept.
    setState(available() ? State::SignedOut : State::Unavailable);
    Q_EMIT stateChanged();
    settleWaiters(Token::Ended);
    return true;
}

// Best effort and not waited for: the sign-in is forgotten here whatever
// Google says, and the account's own settings can always withdraw it.
void DriveAuth::revoke(const QByteArray &token)
{
    if (token.isEmpty())
        return;
    post(revokeEndpoint(), { { "token", token } }, [](int status, const QJsonObject &) {
        qInfo("backup: Google %s the sign-in's withdrawal", status == 200 ? "confirmed" : "did not confirm");
    });
}

// ------------------------------------------------------------- the network

void DriveAuth::post(const QUrl &url, const QList<QPair<QByteArray, QByteArray>> &form, Answer done)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/x-www-form-urlencoded"));
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    // No cookies either way, and no redirect followed: Google's OAuth
    // endpoints answer where they were asked, and the form holds the secret.
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(m_timing.timeoutMs);
    QNetworkReply *reply = m_network->post(request, formBody(form));
    connect(reply, &QNetworkReply::finished, this, [reply, done]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QJsonParseError parseError;
        const QJsonObject body = QJsonDocument::fromJson(reply->readAll(), &parseError).object();
        done(status, body);
    });
}
