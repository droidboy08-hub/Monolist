#include "lastfm.h"

#include "apicredentials.h"
#include "secretstore.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

namespace {

// Last.fm writes numbers as numbers in some places and as strings in others
// ("code":"0" beside "accepted":1), and has changed which over the years.
int number(const QJsonValue &value)
{
    if (value.isDouble())
        return value.toInt();
    return value.toString().trimmed().toInt();
}

const QString kEndpoint = QStringLiteral("https://ws.audioscrobbler.com/2.0/");
const QString kAuthPage = QStringLiteral("https://www.last.fm/api/auth/");
// A scrobble batch is small, but Last.fm is sometimes slow to answer one; an
// answer that never comes is retried later, not waited on for ever.
constexpr int kTimeoutMs = 20000;

}

LastFmApi::LastFmApi(QObject *parent)
    : QObject(parent)
    , m_key(apiKey())
    , m_secret(sharedSecret())
{
}

void LastFmApi::setTestAccount(const QByteArray &key, const QByteArray &secret)
{
    m_key = key;
    m_secret = secret;
    m_testAccount = true;
}

void LastFmApi::setTestResponder(Responder responder)
{
    m_responder = std::move(responder);
}

// The stand-in is honoured only for an invented account, and only on this
// computer. The build's key and a real session key go to Last.fm and nowhere
// else, whatever the environment says: a variable left set after a test, or
// set by someone else, must never send them in plain HTTP to whoever holds a
// port.
QUrl LastFmApi::endpoint() const
{
    if (m_testAccount) {
        const QString custom = qEnvironmentVariable("MONOLIST_LASTFM_URL").trimmed();
        if (!custom.isEmpty()) {
            const QUrl url(custom);
            const QString host = url.host();
            const bool local = host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0
                               || QHostAddress(host).isLoopback();
            if (url.isValid() && local
                && (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https")))
                return url;
        }
    }
    return QUrl(kEndpoint);
}

QUrl LastFmApi::authPageUrl(const QString &token) const
{
    // The key is in the address the browser opens; that is what the page
    // needs, and why the address is never written to the log.
    QUrl url(kAuthPage);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("api_key"), QString::fromLatin1(m_key));
    query.addQueryItem(QStringLiteral("token"), token);
    url.setQuery(query);
    return url;
}

QNetworkAccessManager *LastFmApi::network()
{
    if (!m_network)
        m_network = new QNetworkAccessManager(this);
    return m_network;
}

void LastFmApi::call(Params params, Done done)
{
    if (m_key.isEmpty() || m_secret.isEmpty()) {
        Reply reply;
        reply.outcome = Outcome::Rejected;
        reply.message = unavailableReason();
        QTimer::singleShot(0, this, [done, reply]() { done(reply); });
        return;
    }
    params.prepend({ QStringLiteral("api_key"), QString::fromLatin1(m_key) });
    const Params sent = sign(params, m_secret);

    if (m_responder) {
        const QPair<int, QByteArray> answer = m_responder(sent);
        QTimer::singleShot(0, this, [done, answer]() {
            done(parseReply(answer.first, answer.second, answer.first == 0));
        });
        return;
    }

    QNetworkRequest request(endpoint());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/x-www-form-urlencoded"));
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Monolist/%1 (+https://github.com/droidboy08-hub/Monolist)")
                          .arg(QCoreApplication::applicationVersion()));
    request.setTransferTimeout(kTimeoutMs);
    QNetworkReply *reply = network()->post(request, formBody(sent));
    connect(reply, &QNetworkReply::finished, this, [reply, done]() {
        reply->deleteLater();
        // A 4xx is a Qt error too, with Last.fm's answer in the body; only
        // no status at all means nothing came back.
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        Reply read = parseReply(status, body, status == 0);
        if (status == 0 && read.message == QLatin1String("no answer"))
            read.message = QStringLiteral("no answer (%1)").arg(reply->errorString());
        done(read);
    });
}

void LastFmApi::getToken(Done done)
{
    call({ { QStringLiteral("method"), QStringLiteral("auth.getToken") } }, std::move(done));
}

void LastFmApi::getSession(const QString &token, Done done)
{
    call({ { QStringLiteral("method"), QStringLiteral("auth.getSession") },
           { QStringLiteral("token"), token } },
         std::move(done));
}

void LastFmApi::updateNowPlaying(const QByteArray &sessionKey, const Scrobble &item, Done done)
{
    Params params = {
        { QStringLiteral("method"), QStringLiteral("track.updateNowPlaying") },
        { QStringLiteral("artist"), item.artist },
        { QStringLiteral("track"), item.track },
    };
    if (!item.album.isEmpty())
        params.append({ QStringLiteral("album"), item.album });
    if (!item.albumArtist.isEmpty())
        params.append({ QStringLiteral("albumArtist"), item.albumArtist });
    if (item.durationS > 0)
        params.append({ QStringLiteral("duration"), QString::number(item.durationS) });
    params.append({ QStringLiteral("sk"), QString::fromUtf8(sessionKey) });
    call(params, std::move(done));
}

void LastFmApi::scrobble(const QByteArray &sessionKey, const QList<Scrobble> &items, Done done)
{
    Params params = { { QStringLiteral("method"), QStringLiteral("track.scrobble") } };
    for (int i = 0; i < items.size() && i < kMaxBatch; ++i) {
        const Scrobble &item = items.at(i);
        const auto indexed = [i](const char *name) {
            return QStringLiteral("%1[%2]").arg(QLatin1String(name)).arg(i);
        };
        params.append({ indexed("artist"), item.artist });
        params.append({ indexed("track"), item.track });
        params.append({ indexed("timestamp"), QString::number(item.timestamp) });
        if (!item.album.isEmpty())
            params.append({ indexed("album"), item.album });
        if (!item.albumArtist.isEmpty())
            params.append({ indexed("albumArtist"), item.albumArtist });
        if (item.durationS > 0)
            params.append({ indexed("duration"), QString::number(item.durationS) });
        // Only ever sent as 0: the default is 1, chosen by the listener.
        if (!item.chosenByUser)
            params.append({ indexed("chosenByUser"), QStringLiteral("0") });
    }
    params.append({ QStringLiteral("sk"), QString::fromUtf8(sessionKey) });
    call(params, std::move(done));
}

QByteArray LastFmApi::apiKey()
{
    return QByteArrayLiteral(MONOLIST_LASTFM_API_KEY);
}

QByteArray LastFmApi::sharedSecret()
{
    return QByteArrayLiteral(MONOLIST_LASTFM_SHARED_SECRET);
}

bool LastFmApi::hasKey() const
{
    return !m_key.isEmpty() && !m_secret.isEmpty();
}

bool LastFmApi::available() const
{
    return hasKey() && SecretStore::available();
}

QString LastFmApi::unavailableReason() const
{
    if (!hasKey())
        return QStringLiteral("This build has no Last.fm key.");
    if (!SecretStore::available())
        return QStringLiteral("Monolist cannot keep a Last.fm sign-in safely on this platform yet.");
    return QString();
}

QByteArray LastFmApi::signature(const Params &params, const QByteArray &secret)
{
    QList<QPair<QByteArray, QByteArray>> signedParams;
    signedParams.reserve(params.size());
    for (const auto &param : params) {
        if (param.first == QLatin1String("format") || param.first == QLatin1String("callback"))
            continue;
        signedParams.append({ param.first.toUtf8(), param.second.toUtf8() });
    }
    // QByteArray compares byte for byte, which is the order the server sorts
    // in; comparing QStrings would be the same for these ASCII names, but the
    // bytes are what is signed, so the bytes are what is sorted.
    std::stable_sort(signedParams.begin(), signedParams.end(),
                     [](const QPair<QByteArray, QByteArray> &a, const QPair<QByteArray, QByteArray> &b) {
                         return a.first < b.first;
                     });

    QCryptographicHash md5(QCryptographicHash::Md5);
    for (const auto &param : signedParams) {
        md5.addData(param.first);
        md5.addData(param.second);
    }
    md5.addData(secret);
    return md5.result().toHex();
}

LastFmApi::Params LastFmApi::sign(Params params, const QByteArray &secret)
{
    params.removeIf([](const QPair<QString, QString> &param) {
        return param.first == QLatin1String("api_sig") || param.first == QLatin1String("format");
    });
    params.append({ QStringLiteral("api_sig"), QString::fromLatin1(signature(params, secret)) });
    params.append({ QStringLiteral("format"), QStringLiteral("json") });
    return params;
}

QByteArray LastFmApi::formBody(const Params &params)
{
    QByteArray body;
    for (const auto &param : params) {
        if (!body.isEmpty())
            body += '&';
        body += QUrl::toPercentEncoding(param.first);
        body += '=';
        body += QUrl::toPercentEncoding(param.second);
    }
    return body;
}

LastFmApi::Outcome LastFmApi::outcomeForError(int error)
{
    switch (error) {
    case 9:
        return Outcome::Reauthenticate;
    case 10:
    case 13:
    case 26:
        return Outcome::Hold;
    case 8:
    case 11:
    case 16:
        // 8 is "operation failed, most likely the backend service failed,
        // please try again": Last.fm's own trouble, not the request's.
        return Outcome::Retry;
    case 29:
        return Outcome::RateLimited;
    case 14:
        return Outcome::KeepWaiting;
    case 15:
        return Outcome::RestartSignIn;
    default:
        // 6 invalid parameters among them. Sent again whole it would fail
        // again whole, so a batch is split.
        return Outcome::Rejected;
    }
}

LastFmApi::ItemOutcome LastFmApi::itemOutcome(int ignoredCode)
{
    if (ignoredCode == 0)
        return ItemOutcome::Accepted;
    if (ignoredCode == 5)
        return ItemOutcome::TryTomorrow;
    // 1-4, and any code Last.fm adds later: it answered, and said no, so
    // sending the same item again would only be told no again.
    return ItemOutcome::Dropped;
}

LastFmApi::Reply LastFmApi::parseReply(int httpStatus, const QByteArray &body, bool transportFailed)
{
    Reply reply;
    reply.httpStatus = httpStatus;

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    const bool readable = parseError.error == QJsonParseError::NoError && document.isObject();
    if (readable)
        reply.body = document.object();

    if (readable && reply.body.contains(QStringLiteral("error"))) {
        reply.error = number(reply.body.value(QStringLiteral("error")));
        reply.message = reply.body.value(QStringLiteral("message")).toString();
        reply.outcome = outcomeForError(reply.error);
        // A server that failed says so with its status, whatever number it
        // put in the body: that is a reason to wait, never to find fault
        // with what was sent.
        if (reply.outcome == Outcome::Rejected && httpStatus >= 500)
            reply.outcome = Outcome::Retry;
        return reply;
    }
    // Anything else that is not a plain success is treated as no answer: a
    // captive portal's page, a proxy's 502, a body cut off mid-way. Waiting
    // and sending again loses nothing; deciding from a page that is not
    // Last.fm's could.
    if (transportFailed || !readable || httpStatus < 200 || httpStatus > 299) {
        reply.outcome = Outcome::Retry;
        reply.message = transportFailed ? QStringLiteral("no answer")
                        : !readable     ? QStringLiteral("an answer that is not Last.fm's (HTTP %1)").arg(httpStatus)
                                        : QStringLiteral("HTTP %1").arg(httpStatus);
        return reply;
    }
    reply.outcome = Outcome::Ok;

    // track.scrobble: {"scrobbles": {"scrobble": ..., "@attr": {"accepted": 1, "ignored": 0}}},
    // where one item comes back as an object and several as an array.
    const QJsonObject scrobbles = reply.body.value(QStringLiteral("scrobbles")).toObject();
    if (!scrobbles.isEmpty()) {
        const QJsonObject counts = scrobbles.value(QStringLiteral("@attr")).toObject();
        reply.accepted = number(counts.value(QStringLiteral("accepted")));
        reply.ignored = number(counts.value(QStringLiteral("ignored")));
        const QJsonValue items = scrobbles.value(QStringLiteral("scrobble"));
        QJsonArray list;
        if (items.isArray())
            list = items.toArray();
        else if (items.isObject())
            list.append(items);
        for (const QJsonValue &item : std::as_const(list)) {
            reply.ignoredCodes << number(item.toObject()
                                             .value(QStringLiteral("ignoredMessage")).toObject()
                                             .value(QStringLiteral("code")));
        }
    }
    // track.updateNowPlaying: {"nowplaying": {..., "ignoredMessage": {"code": "0"}}}
    const QJsonObject nowPlaying = reply.body.value(QStringLiteral("nowplaying")).toObject();
    if (!nowPlaying.isEmpty()) {
        reply.ignoredCodes << number(nowPlaying.value(QStringLiteral("ignoredMessage")).toObject()
                                         .value(QStringLiteral("code")));
    }
    return reply;
}

QString LastFmApi::token(const Reply &reply)
{
    if (reply.outcome != Outcome::Ok)
        return QString();
    return reply.body.value(QStringLiteral("token")).toString();
}

bool LastFmApi::session(const Reply &reply, QString *user, QByteArray *key)
{
    if (reply.outcome != Outcome::Ok)
        return false;
    const QJsonObject session = reply.body.value(QStringLiteral("session")).toObject();
    const QString name = session.value(QStringLiteral("name")).toString();
    const QString sessionKey = session.value(QStringLiteral("key")).toString();
    if (name.isEmpty() || sessionKey.isEmpty())
        return false;
    if (user)
        *user = name;
    if (key)
        *key = sessionKey.toUtf8();
    return true;
}

QString LastFmApi::outcomeName(Outcome outcome)
{
    switch (outcome) {
    case Outcome::Ok:             return QStringLiteral("ok");
    case Outcome::Retry:          return QStringLiteral("retry");
    case Outcome::RateLimited:    return QStringLiteral("rate-limited");
    case Outcome::Reauthenticate: return QStringLiteral("reauthenticate");
    case Outcome::Hold:           return QStringLiteral("hold");
    case Outcome::KeepWaiting:    return QStringLiteral("keep-waiting");
    case Outcome::RestartSignIn:  return QStringLiteral("restart-sign-in");
    case Outcome::Rejected:       return QStringLiteral("rejected");
    }
    return QStringLiteral("unknown");
}

QString LastFmApi::itemOutcomeName(ItemOutcome outcome)
{
    switch (outcome) {
    case ItemOutcome::Accepted:    return QStringLiteral("accepted");
    case ItemOutcome::Dropped:     return QStringLiteral("dropped");
    case ItemOutcome::TryTomorrow: return QStringLiteral("try-tomorrow");
    }
    return QStringLiteral("unknown");
}
