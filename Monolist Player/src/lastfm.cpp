#include "lastfm.h"

#include "apicredentials.h"
#include "secretstore.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QUrl>

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

}

LastFmApi::LastFmApi(QObject *parent)
    : QObject(parent)
{
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
    return !apiKey().isEmpty() && !sharedSecret().isEmpty();
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
    case 11:
    case 16:
        return Outcome::Retry;
    case 29:
        return Outcome::RateLimited;
    case 14:
        return Outcome::KeepWaiting;
    case 15:
        return Outcome::RestartSignIn;
    default:
        // 6 invalid parameters and 8 operation failed among them. Sent
        // again whole it would fail again whole, so a batch is split.
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
