#include "streamresolver.h"
#include "appdatabase.h"
#include "ytdlp.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>
#include <QTimeZone>
#include <QUrlQuery>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <memory>
#include <utility>

namespace {

// How long YouTube's answer is held for JioSaavn's, counted from when both
// were asked. A warm YouTube link answers in milliseconds, and JioSaavn's
// search in several hundred on a good day, so this is enough for it to win
// most races it can win at all, and short enough that nobody notices the
// wait when it does not. Past it YouTube plays, and JioSaavn's answer is
// kept for the next time the song comes round. A first guess, to be tuned
// against measurements.
constexpr int kSaavnGraceMs = 1200;
// JioSaavn's answers, remembered. Its links are plain CDN paths that last,
// so a match is kept a week; "not there" a day, since catalogues grow.
constexpr int kSaavnMatchDays = 7;
constexpr int kSaavnNoMatchHours = 24;
// A link mpv refused: JioSaavn left out for this song for ten minutes, the
// time a fault at their end usually takes to pass, then asked afresh.
constexpr int kSaavnRefusedMinutes = 10;
// Three failures in a row (no answer at all, not "not found") and JioSaavn
// is left alone for five minutes.
constexpr int kSaavnFailuresBeforeRest = 3;
constexpr int kSaavnRestMinutes = 5;

constexpr int kPipedTimeoutMs = 8000;
constexpr int kInvidiousTimeoutMs = 6000;
// Roughly three times a healthy resolve on the slowest machine this is known
// to run on. Past this, something is wrong and waiting longer helps nobody.
constexpr int kYtDlpTimeoutMs = 12000;

// A rung as the log names it.
const char *tierName(int tier)
{
    switch (tier) {
    case StreamResolver::TierInnerTube: return "InnerTube";
    case StreamResolver::TierYtDlp:     return "yt-dlp";
    case StreamResolver::TierMuxed:     return "its muxed stream";
    case StreamResolver::TierPiped:     return "Piped";
    case StreamResolver::TierInvidious: return "Invidious";
    default:                            return "another source";
    }
}

QNetworkRequest makeRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Monolist/0.1 (+https://github.com/)"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(kPipedTimeoutMs);
    return request;
}

} // namespace

// Both lists ship empty, and both tiers survive.
//
// The public instances were probed host by host: of the 15 documented Piped
// API hosts, 11 no longer answer at all and the two that do return an empty
// `audioStreams` list or a 500 — the extraction is broken upstream, so no
// refreshed list repairs this. Of the Invidious hosts, one still advertises an
// API and none serve /latest_version as media. What the tiers bought was a
// ~7.5s walk through hosts that cannot answer, paid at the exact moment
// something had already gone wrong.
//
// This is instance rot, not the projects ending: both are still developed. So
// the racing code stays, the `piped_instances` and `invidious_instances`
// settings rows still feed setPipedInstances()/setInvidiousInstances(), and a
// live host list revives either tier without a rebuild. With the list empty,
// startPipedRace()/startInvidiousRace() call tierExhausted() at once and the
// ladder walks past in no measurable time.
//
// The last known-good lists, for whoever repopulates them:
//   Piped      pipedapi.kavin.rocks, pipedapi.moomoo.me,
//              piped-api.garudalinux.org, api.piped.projectsegfau.lt,
//              piped.privacydev.net
//   Invidious  yewtu.be, invidious.projectsegfau.lt, iv.ggtyler.dev,
//              inv.nadeko.net, invidious.nerdvpn.de, invidious.privacydev.net,
//              yt.artemislena.eu, invidious.fdn.fr, invidious.slipfox.xyz,
//              invidious.lunar.icu, invidious.vps.sh, inv.tux.pizza,
//              invidious.io.lol
QStringList StreamResolver::defaultPipedInstances()
{
    return {};
}

QStringList StreamResolver::defaultInvidiousInstances()
{
    return {};
}

StreamResolver::StreamResolver(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
    , m_piped(defaultPipedInstances())
    , m_invidious(defaultInvidiousInstances())
{
}

StreamResolver::~StreamResolver()
{
    cancelAll();
}

void StreamResolver::setPipedInstances(const QStringList &hosts)
{
    if (!hosts.isEmpty())
        m_piped = hosts;
}

void StreamResolver::setInvidiousInstances(const QStringList &hosts)
{
    if (!hosts.isEmpty())
        m_invidious = hosts;
}

void StreamResolver::resolve(const QString &videoId, int firstTier)
{
    if (videoId.isEmpty()) {
        Q_EMIT failed(videoId, QStringLiteral("Empty video id."));
        return;
    }

    if (firstTier <= TierInnerTube) {
        // Still-valid link from earlier or from a prefetch: answer at once,
        // but queued, so the caller sees the same order of events as always.
        const auto cached = m_cache.constFind(videoId);
        if (cached != m_cache.constEnd() && cached->expires > QDateTime::currentDateTimeUtc()) {
            const CacheEntry entry = *cached;
            QMetaObject::invokeMethod(this, [this, videoId, entry]() {
                report(videoId, entry.url, entry.tier, /*fromCache=*/true);
            }, Qt::QueuedConnection);
            return;
        }
        // A prefetch already on its way: let it finish, and report this time.
        if (Job *running = m_jobs.value(videoId); running && running->prefetch) {
            running->prefetch = false;
            return;
        }
    }

    QList<int> tiers;
    for (int tier = qMax(int(TierInnerTube), firstTier); tier < TierExhausted; ++tier)
        tiers.append(tier);
    start(videoId, tiers);
}

void StreamResolver::resolveVia(const QString &videoId, const QList<int> &tiers, int homeTier)
{
    if (videoId.isEmpty()) {
        Q_EMIT failed(videoId, QStringLiteral("Empty video id."));
        return;
    }
    // This play's rescue link, when the walk starts on its rung: the play
    // goes back to it (the sound after its picture, say) rather than paying
    // for the rescue twice. Queued, as a remembered link always is.
    if (!tiers.isEmpty()) {
        const auto rescue = m_rescueLinks.constFind(videoId);
        if (rescue != m_rescueLinks.constEnd() && rescue->tier == tiers.first()
            && rescue->expires > QDateTime::currentDateTimeUtc()) {
            const CacheEntry entry = *rescue;
            qInfo("resolver: %s goes back to the link that rescued this play", qPrintable(videoId));
            QMetaObject::invokeMethod(this, [this, videoId, entry]() {
                report(videoId, entry.url, entry.tier, /*fromCache=*/true);
            }, Qt::QueuedConnection);
            return;
        }
    }
    start(videoId, tiers, homeTier);
}

void StreamResolver::start(const QString &videoId, QList<int> tiers, int homeTier)
{
    cancelJob(videoId);

    auto *job = new Job;
    job->videoId = videoId;
    job->homeTier = homeTier;
    m_jobs.insert(videoId, job);
    const int first = tiers.isEmpty() ? int(TierExhausted) : tiers.takeFirst();
    job->next = tiers;
    startTier(job, first);
}

// A link the player refused says something about where it came from. After
// InnerTube's, the muxed stream comes first: yt-dlp's sound would be fetched
// as the very client whose link was just turned down, where the muxed stream
// is asked of others. After yt-dlp's, the muxed stream is the one kind left
// to try; after the muxed stream's, yt-dlp's sound. The public instances come
// last either way. Before all of them, the caller may ask the refused tier
// once more (PlaybackController's loadFailed): a refusal is often the link's
// alone, and InnerTube's next one costs a fifth of a second where the muxed
// stream costs three.
QList<int> StreamResolver::afterRefusal(int tier)
{
    switch (tier) {
    case TierInnerTube: return { TierMuxed, TierYtDlp, TierPiped, TierInvidious };
    case TierYtDlp:     return { TierMuxed, TierPiped, TierInvidious };
    case TierMuxed:     return { TierYtDlp, TierPiped, TierInvidious };
    case TierPiped:     return { TierInvidious };
    default:            return {};
    }
}

QVariantMap StreamResolver::headersFor(const QString &videoId, const QString &url) const
{
    for (const QHash<QString, CacheEntry> *links : { &m_rescueLinks, &m_cache }) {
        const auto entry = links->constFind(videoId);
        if (entry != links->constEnd() && entry->url == url)
            return entry->headers;
    }
    return {};
}

// The picture: yt-dlp only. The public instances answer with sound, and a
// track's picture is asked for rarely enough that racing a dozen hosts for it
// would be more machinery than it is worth.
void StreamResolver::resolveVideo(const QString &videoId, int maxHeight)
{
    if (videoId.isEmpty())
        return;

    const auto cached = m_videoCache.constFind(videoId);
    if (cached != m_videoCache.constEnd() && cached->expires > QDateTime::currentDateTimeUtc()) {
        const VideoLinks links = *cached;
        QMetaObject::invokeMethod(this, [this, videoId, links]() {
            Q_EMIT videoResolved(videoId, links.video, links.audio, links.headers);
        }, Qt::QueuedConnection);
        return;
    }
    if (m_videoJobs.contains(videoId))
        return;   // already on its way
    if (!YtDlp::isAvailable()) {
        Q_EMIT videoFailed(videoId, QStringLiteral("Playing video needs yt-dlp."));
        return;
    }

    YtDlpRequest *request = YtDlp::resolveVideo(videoId, maxHeight, this);
    m_videoJobs.insert(videoId, request);

    connect(request, &YtDlpRequest::succeededJson, this,
            [this, videoId](const QJsonDocument &document) {
                if (m_videoJobs.take(videoId).isNull())
                    return;   // cancelled meanwhile
                const QJsonObject root = document.object();
                QString video = root.value(QStringLiteral("url")).toString();
                QString audio;
                QJsonObject chosen = root;
                // Two streams: yt-dlp lists them as it would merge them.
                const QJsonArray parts = root.value(QStringLiteral("requested_formats")).toArray();
                if (parts.size() >= 2) {
                    const QJsonObject first = parts.at(0).toObject();
                    const QJsonObject second = parts.at(1).toObject();
                    const bool firstIsVideo = first.value(QStringLiteral("vcodec")).toString()
                                              != QLatin1String("none");
                    chosen = firstIsVideo ? first : second;
                    video = chosen.value(QStringLiteral("url")).toString();
                    audio = (firstIsVideo ? second : first).value(QStringLiteral("url")).toString();
                }
                if (video.isEmpty()) {
                    Q_EMIT videoFailed(videoId, QStringLiteral("No picture is published for this track."));
                    return;
                }
                // The headers the link was fetched with. YouTube ties a link
                // to the client that asked for it and answers 403 to anyone
                // else — which is how a video that plays in yt-dlp refuses to
                // play here.
                const QVariantMap headers =
                    chosen.value(QStringLiteral("http_headers")).toObject().toVariantMap();
                m_videoCache.insert(videoId, { video, audio, headers, expiryOf(video) });
                Q_EMIT videoResolved(videoId, video, audio, headers);
            });

    connect(request, &YtDlpRequest::failed, this, [this, videoId](const QString &reason) {
        if (m_videoJobs.take(videoId).isNull())
            return;
        Q_EMIT videoFailed(videoId, reason);
    });
}

void StreamResolver::cancelVideo(const QString &videoId)
{
    if (QPointer<YtDlpRequest> request = m_videoJobs.take(videoId); request)
        request->cancel();
}

void StreamResolver::prefetch(const QString &videoId)
{
    if (videoId.isEmpty() || m_jobs.contains(videoId))
        return;
    const auto cached = m_cache.constFind(videoId);
    if (cached != m_cache.constEnd() && cached->expires > QDateTime::currentDateTimeUtc())
        return;

    auto *job = new Job;
    job->videoId = videoId;
    job->prefetch = true;
    job->next = { TierYtDlp, TierMuxed, TierPiped, TierInvidious };
    m_jobs.insert(videoId, job);
    startTier(job, TierInnerTube);
}

void StreamResolver::invalidate(const QString &videoId)
{
    m_cache.remove(videoId);
    m_rescueLinks.remove(videoId);
    m_videoCache.remove(videoId);
}

// googlevideo links carry their own expiry ("expire=<unix time>"). Trust it
// with a margin, and give instance links, which carry none, half an hour.
QDateTime StreamResolver::expiryOf(const QString &url)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    bool ok = false;
    const qint64 expire = QUrlQuery(QUrl(url)).queryItemValue(QStringLiteral("expire")).toLongLong(&ok);
    if (!ok || expire <= 0)
        return now.addSecs(30 * 60);
    return qMin(QDateTime::fromSecsSinceEpoch(expire, QTimeZone::UTC).addSecs(-10 * 60), now.addSecs(5 * 3600));
}

void StreamResolver::startTier(Job *job, int tier)
{
    job->tier = tier;
    ++job->generation;
    Q_EMIT tierChanged(job->videoId, tier);

    if (tier < TierExhausted) {
        const auto test = m_testAnswers.constFind(job->videoId);
        if (test != m_testAnswers.constEnd() && test->contains(tier)) {
            answerForTest(job, test->value(tier));
            return;
        }
    }

    switch (tier) {
    case TierInnerTube:  startInnerTube(job);     break;
    case TierYtDlp:      startYtDlp(job);         break;
    case TierMuxed:      startMuxed(job);         break;
    case TierPiped:      startPipedRace(job);     break;
    case TierInvidious:  startInvidiousRace(job); break;
    default: {
        const QString reason = job->errors.isEmpty()
            ? QStringLiteral("No source could resolve this track.")
            : job->errors.join(QStringLiteral(" | "));
        const QString id = job->videoId;
        const bool silent = job->prefetch;
        discard(job);
        if (!silent)
            reportFailure(id, reason);
        break;
    }
    }
}

// One request, in this process, to the endpoint YouTube's own clients use.
// Everything that can go wrong here — a refused track, a stream offered only
// behind a cipher, a network failure — is the same answer: let yt-dlp have it.
void StreamResolver::startInnerTube(Job *job)
{
    const QString videoId = job->videoId;
    const int generation = job->generation;

    m_innerTube.player(videoId, [this, videoId, generation](const QString &url, int itag,
                                                            const QString &error) {
        Job *job = m_jobs.value(videoId);
        if (!job || job->settled || job->generation != generation)
            return;
        if (url.isEmpty()) {
            tierExhausted(job, QStringLiteral("InnerTube: %1").arg(error));
            return;
        }
        qInfo("innertube: %s resolved as itag %d", qPrintable(videoId), itag);
        succeed(job, handOverInnerTube(job, url));
    });
}

QString StreamResolver::handOverInnerTube(Job *job, const QString &url)
{
    if (job->prefetch || job->videoId != m_spoil || m_spoilsLeft <= 0)
        return url;
    if (--m_spoilsLeft == 0)
        m_spoil.clear();
    // The expiry is signed into the link, so any change to it is a link the
    // CDN answers with 403 — the refusal this stands in for.
    QUrl spoiled(url);
    QUrlQuery query(spoiled);
    if (query.hasQueryItem(QStringLiteral("expire"))) {
        query.removeAllQueryItems(QStringLiteral("expire"));
        query.addQueryItem(QStringLiteral("expire"), QStringLiteral("1"));
        spoiled.setQuery(query);
    } else {
        spoiled.setHost(QStringLiteral("spoiled.invalid"));
    }
    qInfo("innertube: %s handed over spoiled, for the self-test", qPrintable(job->videoId));
    return spoiled.toString(QUrl::FullyEncoded);
}

void StreamResolver::answerForTest(Job *job, const QString &url)
{
    QString link = url;
    if (!link.isEmpty()) {
        QUrl answer(url);
        QUrlQuery query(answer);
        query.addQueryItem(QStringLiteral("g"), QString::number(++m_testAnswersGiven));
        query.addQueryItem(QStringLiteral("expire"),
                           QString::number(QDateTime::currentSecsSinceEpoch() + 6 * 3600));
        answer.setQuery(query);
        link = answer.toString(QUrl::FullyEncoded);
    }
    // Queued, as a real answer always comes.
    const QString videoId = job->videoId;
    const int generation = job->generation;
    QMetaObject::invokeMethod(this, [this, videoId, generation, link]() {
        Job *job = m_jobs.value(videoId);
        if (!job || job->settled || job->generation != generation)
            return;
        if (link.isEmpty()) {
            tierExhausted(job, QStringLiteral("self-test: this tier fails"));
            return;
        }
        qInfo("resolver: %s answered by the self-test as %s", qPrintable(videoId), tierName(job->tier));
        succeed(job, job->tier == TierInnerTube ? handOverInnerTube(job, link) : link);
    }, Qt::QueuedConnection);
}

void StreamResolver::startYtDlp(Job *job)
{
    if (!YtDlp::isAvailable()) {
        tierExhausted(job, QStringLiteral("yt-dlp not installed"));
        return;
    }

    YtDlpRequest *request = YtDlp::resolveAudio(job->videoId, this);
    job->ytdlp = request;
    const QString videoId = job->videoId;
    const int generation = job->generation;

    // Nothing else bounds this. --socket-timeout is per socket operation, and
    // yt-dlp's own retry budget lets one resolve legitimately run for minutes
    // while the app sits on "Resolving source…" with no way out. The guard is
    // the same shape as the lambdas below, so a job that has already settled
    // or moved on ignores it; tierExhausted -> abortPending kills the process.
    QTimer::singleShot(kYtDlpTimeoutMs, this, [this, videoId, generation]() {
        Job *job = m_jobs.value(videoId);
        if (job && !job->settled && job->generation == generation && job->tier == TierYtDlp)
            tierExhausted(job, QStringLiteral("yt-dlp timed out"));
    });

    connect(request, &YtDlpRequest::succeededJson, this,
            [this, videoId, generation](const QJsonDocument &document) {
                Job *job = m_jobs.value(videoId);
                if (!job || job->settled || job->generation != generation)
                    return;
                const QString url = document.object().value(QStringLiteral("url")).toString();
                if (url.isEmpty())
                    tierExhausted(job, QStringLiteral("yt-dlp returned no audio url"));
                else
                    succeed(job, url);
            });

    connect(request, &YtDlpRequest::failed, this,
            [this, videoId, generation](const QString &reason) {
                Job *job = m_jobs.value(videoId);
                if (job && !job->settled && job->generation == generation)
                    tierExhausted(job, QStringLiteral("yt-dlp: %1").arg(reason));
            });
}

// The muxed stream: itag 18 where it is offered, the best other stream with
// the sound and the picture in one file where it is not. mpv plays it as sound
// (vid=no), and the picture's bytes are the price of a stream that plays when
// the sound-only ones would not — paid for this track alone, and only then.
void StreamResolver::startMuxed(Job *job)
{
    if (!YtDlp::isAvailable()) {
        tierExhausted(job, QStringLiteral("yt-dlp not installed, so no muxed stream"));
        return;
    }

    YtDlpRequest *request = YtDlp::resolveMuxed(job->videoId, this);
    job->ytdlp = request;
    const QString videoId = job->videoId;
    const int generation = job->generation;

    // Bounded like the sound's own request, for the same reason.
    QTimer::singleShot(kYtDlpTimeoutMs, this, [this, videoId, generation]() {
        Job *job = m_jobs.value(videoId);
        if (job && !job->settled && job->generation == generation && job->tier == TierMuxed)
            tierExhausted(job, QStringLiteral("yt-dlp timed out finding the muxed stream"));
    });

    connect(request, &YtDlpRequest::succeededJson, this,
            [this, videoId, generation](const QJsonDocument &document) {
                Job *job = m_jobs.value(videoId);
                if (!job || job->settled || job->generation != generation)
                    return;
                const QJsonObject root = document.object();
                const QString url = root.value(QStringLiteral("url")).toString();
                if (url.isEmpty()) {
                    tierExhausted(job, QStringLiteral("yt-dlp returned no muxed stream"));
                    return;
                }
                // Which itag, and as which client: the log is the only place
                // that says what a failed track was rescued with.
                const QString client = QUrlQuery(QUrl(url)).queryItemValue(QStringLiteral("c"));
                qInfo("muxed: %s resolved as itag %s (%s), for this track alone",
                      qPrintable(videoId),
                      qPrintable(root.value(QStringLiteral("format_id")).toString()),
                      qPrintable(client.isEmpty() ? QStringLiteral("client not named") : client));
                // Fetched as the client that asked for it, or YouTube refuses
                // it (see resolveVideo).
                succeed(job, url, root.value(QStringLiteral("http_headers")).toObject().toVariantMap());
            });

    connect(request, &YtDlpRequest::failed, this,
            [this, videoId, generation](const QString &reason) {
                Job *job = m_jobs.value(videoId);
                if (job && !job->settled && job->generation == generation)
                    tierExhausted(job, QStringLiteral("muxed: %1").arg(reason));
            });
}

// Piped returns a JSON list of audio streams pointing straight at the YouTube
// CDN. Fine for playback; the CDN rejects byte-range fetches from other
// origins, which is why downloads prefer Invidious.
void StreamResolver::startPipedRace(Job *job)
{
    if (m_piped.isEmpty()) {
        tierExhausted(job, QStringLiteral("no Piped instances configured"));
        return;
    }

    const QString videoId = job->videoId;
    // Shared, not owned by any one lambda: aborting the losers fires their
    // finished() handlers synchronously, so a raw counter deleted by the winner
    // would be read after free.
    auto remaining = std::make_shared<int>(m_piped.size());
    const int generation = job->generation;

    for (const QString &host : std::as_const(m_piped)) {
        const QUrl url(QStringLiteral("https://%1/streams/%2").arg(host, videoId));
        QNetworkReply *reply = m_network->get(makeRequest(url));
        job->pending.append(reply);

        connect(reply, &QNetworkReply::finished, this, [this, reply, videoId, generation, remaining]() {
            reply->deleteLater();
            Job *job = m_jobs.value(videoId);
            const bool live = job && !job->settled && job->generation == generation;

            if (live && reply->error() == QNetworkReply::NoError) {
                const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
                const QJsonArray streams = root.value(QStringLiteral("audioStreams")).toArray();

                QString best;
                int bestBitrate = -1;
                for (const QJsonValue &value : streams) {
                    const QJsonObject stream = value.toObject();
                    const QString streamUrl = stream.value(QStringLiteral("url")).toString();
                    if (streamUrl.isEmpty())
                        continue;
                    const bool isMp4 = stream.value(QStringLiteral("mimeType")).toString()
                                           .contains(QLatin1String("mp4"));
                    const int bitrate = stream.value(QStringLiteral("bitrate")).toInt()
                                        + (isMp4 ? 1'000'000 : 0); // prefer m4a
                    if (bitrate > bestBitrate) {
                        bestBitrate = bitrate;
                        best = streamUrl;
                    }
                }

                if (!best.isEmpty()) {
                    // First instance to answer wins; the rest are aborted.
                    succeed(job, best);
                    return;
                }
            }

            if (--(*remaining) == 0 && live)
                tierExhausted(job, QStringLiteral("all Piped instances failed"));
        });
    }
}

// Invidious with local=true proxies the bytes through the instance itself, so
// the resulting URL is a normal HTTP resource — fetchable, seekable, and usable
// for downloads as well as playback.
void StreamResolver::startInvidiousRace(Job *job)
{
    if (m_invidious.isEmpty()) {
        tierExhausted(job, QStringLiteral("no Invidious instances configured"));
        return;
    }

    const QString videoId = job->videoId;
    auto remaining = std::make_shared<int>(m_invidious.size());
    const int generation = job->generation;

    for (const QString &host : std::as_const(m_invidious)) {
        const QUrl url(QStringLiteral("https://%1/latest_version?id=%2&itag=140&local=true")
                           .arg(host, videoId));
        QNetworkRequest request = makeRequest(url);
        request.setTransferTimeout(kInvidiousTimeoutMs);

        // HEAD is enough to learn whether the instance will serve this id,
        // and avoids pulling the file body during probing.
        QNetworkReply *reply = m_network->head(request);
        job->pending.append(reply);

        connect(reply, &QNetworkReply::finished, this,
                [this, reply, videoId, url, generation, remaining]() {
                    reply->deleteLater();
                    Job *job = m_jobs.value(videoId);
                    const bool live = job && !job->settled && job->generation == generation;

                    const int status = reply->attribute(
                        QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    const QString contentType = reply->header(
                        QNetworkRequest::ContentTypeHeader).toString();

                    // A dead or rate-limited instance answers 200 with an HTML
                    // error page. Accepting on status alone handed mpv a web
                    // page and produced "unrecognized file format", so require
                    // the response to actually claim to be media.
                    const bool isMedia = contentType.startsWith(QLatin1String("audio/"))
                                      || contentType.startsWith(QLatin1String("video/"))
                                      || contentType.startsWith(QLatin1String("application/octet-stream"));

                    if (live && reply->error() == QNetworkReply::NoError
                        && status >= 200 && status < 300 && isMedia) {
                        succeed(job, url.toString());
                        return;
                    }

                    if (--(*remaining) == 0 && live)
                        tierExhausted(job, QStringLiteral("all Invidious instances failed"));
                });
    }
}

void StreamResolver::succeed(Job *job, const QString &url, const QVariantMap &headers)
{
    if (job->settled)
        return;
    job->settled = true;

    const QString videoId = job->videoId;
    const int tier = job->tier;
    const bool silent = job->prefetch;
    // A rescue link serves the play it rescued, and no replay: see
    // resolveVia. InnerTube's own links are never one.
    const bool rescue = job->homeTier >= 0 && tier != job->homeTier && tier != TierInnerTube;
    if (rescue && !m_keepRescueLinks) {
        m_rescueLinks.insert(videoId, { url, tier, expiryOf(url), headers });
        qInfo("resolver: %s rescued by %s, for this play only", qPrintable(videoId), tierName(tier));
    } else {
        m_cache.insert(videoId, { url, tier, expiryOf(url), headers });
    }

    abortPending(job);
    discard(job);

    if (!silent)
        report(videoId, url, tier, /*fromCache=*/false);
}

void StreamResolver::tierExhausted(Job *job, const QString &reason)
{
    if (job->settled)
        return;
    job->errors.append(reason);
    abortPending(job);
    startTier(job, job->next.isEmpty() ? int(TierExhausted) : job->next.takeFirst());
}

void StreamResolver::abortPending(Job *job)
{
    // Bump first: abort() and cancel() below deliver their callbacks
    // synchronously, and those callbacks must see themselves as stale rather
    // than advance the tier a second time.
    ++job->generation;

    // Detach the lists before touching them — starting the next tier appends to
    // job->pending, which must not happen mid-iteration.
    const QList<QPointer<QNetworkReply>> replies = std::exchange(job->pending, {});
    for (const QPointer<QNetworkReply> &reply : replies) {
        if (reply && reply->isRunning())
            reply->abort();
    }

    if (QPointer<YtDlpRequest> request = std::exchange(job->ytdlp, {}); request && request->isRunning())
        request->cancel();
}

void StreamResolver::discard(Job *job)
{
    m_jobs.remove(job->videoId);
    delete job;
}

void StreamResolver::cancel(const QString &videoId)
{
    m_races.remove(videoId);
    cancelJob(videoId);
}

void StreamResolver::cancelJob(const QString &videoId)
{
    Job *job = m_jobs.value(videoId);
    if (!job)
        return;
    job->settled = true;
    abortPending(job);
    discard(job);
}

void StreamResolver::cancelAll()
{
    m_races.clear();
    const QStringList ids = m_jobs.keys();
    for (const QString &id : ids)
        cancelJob(id);
}

// ------------------------------------------------------------------ the race

void StreamResolver::resolveTrack(const Saavn::Target &track)
{
    const QString videoId = track.videoId;
    if (videoId.isEmpty()) {
        Q_EMIT failed(videoId, QStringLiteral("Empty video id."));
        return;
    }
    // Asked again for the same song: the earlier race is over, unheard.
    m_races.remove(videoId);

    const bool resting = m_saavnRestUntil.isValid() && m_saavnRestUntil > QDateTime::currentDateTimeUtc();
    if (!m_saavnEnabled || track.title.trimmed().isEmpty()) {
        resolve(videoId);
        return;
    }

    const SaavnVerdict verdict = saavnVerdict(track);
    if (verdict.kind == SaavnVerdict::Match) {
        qInfo("jiosaavn: %s plays from the match found before (%s, %d kbps)", qPrintable(videoId),
              qPrintable(verdict.saavnId), verdict.kbps);
        const QString url = handOverSaavn(videoId, verdict.url);
        // Queued, like a remembered YouTube link, so the caller sees the
        // same order of events either way.
        QMetaObject::invokeMethod(this, [this, videoId, url]() {
            Q_EMIT resolved(videoId, url, TierJioSaavn, /*fromCache=*/true);
        }, Qt::QueuedConnection);
        // YouTube's link, fetched quietly beside it: should mpv refuse
        // JioSaavn's, the way back starts warm rather than from nothing.
        prefetch(videoId);
        return;
    }
    if (verdict.kind != SaavnVerdict::Unknown || resting) {
        resolve(videoId);
        return;
    }

    // Both at once. The ladder's answer comes back through report(), which
    // finds the race and holds it there.
    Race race;
    race.clock.start();
    race.generation = ++m_raceGeneration;
    m_races.insert(videoId, race);
    startSaavnLookup(track);
    resolve(videoId);
}

void StreamResolver::prefetchTrack(const Saavn::Target &track)
{
    if (track.videoId.isEmpty())
        return;
    const bool resting = m_saavnRestUntil.isValid() && m_saavnRestUntil > QDateTime::currentDateTimeUtc();
    if (m_saavnEnabled && !track.title.trimmed().isEmpty()) {
        if (saavnVerdict(track).kind == SaavnVerdict::Unknown && !resting)
            startSaavnLookup(track);
    }
    // YouTube's link too, even where JioSaavn's is known: it is what the song
    // falls back to at once should mpv refuse JioSaavn's.
    prefetch(track.videoId);
}

void StreamResolver::report(const QString &videoId, const QString &url, int tier, bool fromCache)
{
    const auto race = m_races.find(videoId);
    if (race == m_races.end()) {
        Q_EMIT resolved(videoId, url, tier, fromCache);
        return;
    }
    race->youtubeReady = true;
    race->url = url;
    race->tier = tier;
    race->fromCache = fromCache;
    const qint64 left = kSaavnGraceMs - race->clock.elapsed();
    if (left <= 0) {
        qInfo("jiosaavn: %s has not answered; YouTube plays", qPrintable(videoId));
        youtubeWins(videoId);
        return;
    }
    const int generation = race->generation;
    QTimer::singleShot(int(left), this, [this, videoId, generation]() {
        const auto race = m_races.constFind(videoId);
        if (race == m_races.constEnd() || race->generation != generation)
            return;
        qInfo("jiosaavn: %s did not answer within %d ms; YouTube plays, and a later answer is kept",
              qPrintable(videoId), kSaavnGraceMs);
        youtubeWins(videoId);
    });
}

// The ladder found nothing. With a race on, JioSaavn is all that is left and
// is waited for — its own timeouts bound the wait — before the failure is
// told.
void StreamResolver::reportFailure(const QString &videoId, const QString &reason)
{
    const auto race = m_races.find(videoId);
    if (race == m_races.end()) {
        Q_EMIT failed(videoId, reason);
        return;
    }
    race->youtubeFailed = true;
    race->failure = reason;
}

void StreamResolver::youtubeWins(const QString &videoId)
{
    const Race race = m_races.take(videoId);
    Q_EMIT resolved(videoId, race.url, race.tier, race.fromCache);
}

void StreamResolver::startSaavnLookup(const Saavn::Target &track)
{
    if (m_saavnAsking.contains(track.videoId))
        return;   // already on its way; its answer serves this too
    m_saavnAsking.insert(track.videoId);
    const QString videoId = track.videoId;
    const QString signature = Saavn::signature(track);
    m_saavn.lookup(track, [this, videoId, signature](const JioSaavn::Result &result) {
        saavnAnswered(videoId, signature, result);
    });
}

void StreamResolver::saavnAnswered(const QString &videoId, const QString &signature,
                                   const JioSaavn::Result &result)
{
    m_saavnAsking.remove(videoId);
    const QDateTime now = QDateTime::currentDateTimeUtc();

    switch (result.kind) {
    case JioSaavn::Result::Match: {
        m_saavnFailuresInARow = 0;
        SaavnVerdict verdict;
        verdict.kind = SaavnVerdict::Match;
        verdict.url = result.url;
        verdict.kbps = result.kbps;
        verdict.saavnId = result.saavnId;
        verdict.signature = signature;
        verdict.expires = now.addDays(kSaavnMatchDays);
        rememberSaavn(videoId, verdict);
        // The host and the bitrate: the link itself stays out of the log.
        qInfo("jiosaavn: %s is \"%s\" by %s (%s, %d s) at %d kbps from %s, found in %lld ms", qPrintable(videoId),
              qPrintable(result.title), qPrintable(result.artists), qPrintable(result.saavnId), result.durationSec,
              result.kbps, qPrintable(QUrl(result.url).host()), static_cast<long long>(result.elapsedMs));
        break;
    }
    case JioSaavn::Result::NoMatch: {
        m_saavnFailuresInARow = 0;
        SaavnVerdict verdict;
        verdict.kind = SaavnVerdict::NoMatch;
        verdict.signature = signature;
        verdict.expires = now.addSecs(kSaavnNoMatchHours * 3600);
        rememberSaavn(videoId, verdict);
        qInfo("jiosaavn: %s not taken from JioSaavn: %s (%d rows, %lld ms)", qPrintable(videoId),
              qPrintable(result.reason), result.rows, static_cast<long long>(result.elapsedMs));
        // The first few reasons, so a song that should have matched can be
        // looked into from the log.
        for (qsizetype i = 0; i < result.refusals.size() && i < 3; ++i)
            qInfo("jiosaavn:   %s", qPrintable(result.refusals.at(i)));
        break;
    }
    case JioSaavn::Result::Failed:
        // No verdict: JioSaavn was not heard from, so nothing is remembered
        // about the song and it is asked again next time.
        qWarning("jiosaavn: %s could not be looked up: %s", qPrintable(videoId), qPrintable(result.reason));
        if (++m_saavnFailuresInARow >= kSaavnFailuresBeforeRest) {
            m_saavnFailuresInARow = 0;
            m_saavnRestUntil = now.addSecs(kSaavnRestMinutes * 60);
            qWarning("jiosaavn: %d lookups failed in a row; not asking for %d minutes",
                     kSaavnFailuresBeforeRest, kSaavnRestMinutes);
        }
        break;
    }

    const auto race = m_races.find(videoId);
    if (race == m_races.end())
        return;   // a prefetch, or a song the race already gave to YouTube
    if (result.kind == JioSaavn::Result::Match) {
        m_races.erase(race);
        // YouTube's leg carries on unheard, into the cache: should mpv refuse
        // JioSaavn's link, the way back is already there.
        if (Job *job = m_jobs.value(videoId))
            job->prefetch = true;
        Q_EMIT resolved(videoId, handOverSaavn(videoId, result.url), TierJioSaavn, /*fromCache=*/false);
        return;
    }
    if (race->youtubeReady) {
        youtubeWins(videoId);
        return;
    }
    if (race->youtubeFailed) {
        const QString reason = race->failure;
        m_races.erase(race);
        Q_EMIT failed(videoId, reason);
        return;
    }
    // YouTube is still on its way, and is now simply the answer.
    m_races.erase(race);
}

StreamResolver::SaavnVerdict StreamResolver::saavnVerdict(const Saavn::Target &track)
{
    const QString &videoId = track.videoId;
    const QString signature = Saavn::signature(track);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const auto known = m_saavnVerdicts.constFind(videoId);
    if (known != m_saavnVerdicts.constEnd()) {
        if (known->expires <= now) {
            m_saavnVerdicts.remove(videoId);
            return {};   // the table's copy expired with it
        }
        // Asked about under another name or length: that answer was for a
        // different question.
        if (known->kind != SaavnVerdict::Refused && known->signature != signature)
            return {};
        return *known;
    }

    purgeExpiredSaavn();
    QSqlDatabase db = AppDatabase::connection();
    if (!db.isOpen())
        return {};
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT matched, saavn_id, url, kbps, expires_at, signature, matcher FROM saavn_matches"
        " WHERE video_id = ?"));
    query.addBindValue(videoId);
    if (!query.exec() || !query.next())
        return {};
    SaavnVerdict verdict;
    verdict.expires = QDateTime::fromSecsSinceEpoch(query.value(4).toLongLong(), QTimeZone::UTC);
    verdict.signature = query.value(5).toString();
    // Past its time, judged by an older matcher, or about the song under
    // another name or length: asked afresh, and the new answer replaces it.
    if (verdict.expires <= now || query.value(6).toInt() != Saavn::matcherVersion()
        || verdict.signature != signature)
        return {};
    verdict.kind = query.value(0).toInt() == 1 ? SaavnVerdict::Match : SaavnVerdict::NoMatch;
    verdict.saavnId = query.value(1).toString();
    verdict.url = query.value(2).toString();
    verdict.kbps = query.value(3).toInt();
    // The link goes to mpv as it is: JioSaavn's CDN over HTTPS, or nothing.
    if (verdict.kind == SaavnVerdict::Match && !Saavn::isCdnLink(verdict.url))
        return {};
    m_saavnVerdicts.insert(videoId, verdict);
    return verdict;
}

void StreamResolver::purgeExpiredSaavn()
{
    if (m_saavnPurged)
        return;
    QSqlDatabase db = AppDatabase::connection();
    if (!db.isOpen())
        return;
    m_saavnPurged = true;
    QSqlQuery query(db);
    query.prepare(QStringLiteral("DELETE FROM saavn_matches WHERE expires_at <= ?"));
    query.addBindValue(QDateTime::currentSecsSinceEpoch());
    query.exec();
}

void StreamResolver::rememberSaavn(const QString &videoId, const SaavnVerdict &verdict)
{
    m_saavnVerdicts.insert(videoId, verdict);
    if (verdict.kind != SaavnVerdict::Match && verdict.kind != SaavnVerdict::NoMatch)
        return;   // a refusal is for this session only
    QSqlDatabase db = AppDatabase::connection();
    if (!db.isOpen())
        return;
    // When it was asked is not kept, only when the answer runs out, which is
    // all anything reads; Clear history empties the table (Library).
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO saavn_matches (video_id, matched, saavn_id, url, kbps, expires_at, signature, matcher)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(videoId);
    query.addBindValue(verdict.kind == SaavnVerdict::Match ? 1 : 0);
    query.addBindValue(AppDatabase::text(verdict.saavnId));
    query.addBindValue(AppDatabase::text(verdict.url));
    query.addBindValue(verdict.kbps);
    query.addBindValue(verdict.expires.toSecsSinceEpoch());
    query.addBindValue(AppDatabase::text(verdict.signature));
    query.addBindValue(Saavn::matcherVersion());
    if (!query.exec())
        qWarning("jiosaavn: could not remember the answer for %s", qPrintable(videoId));
}

void StreamResolver::refuseSaavn(const QString &videoId)
{
    SaavnVerdict verdict;
    verdict.kind = SaavnVerdict::Refused;
    verdict.expires = QDateTime::currentDateTimeUtc().addSecs(kSaavnRefusedMinutes * 60);
    m_saavnVerdicts.insert(videoId, verdict);
    QSqlDatabase db = AppDatabase::connection();
    if (db.isOpen()) {
        QSqlQuery query(db);
        query.prepare(QStringLiteral("DELETE FROM saavn_matches WHERE video_id = ?"));
        query.addBindValue(videoId);
        query.exec();
    }
}

QString StreamResolver::handOverSaavn(const QString &videoId, const QString &url)
{
    if (videoId != m_spoilSaavn)
        return url;
    m_spoilSaavn.clear();
    // A file the CDN does not have, on the host it does serve: refused the
    // way a moved or withdrawn file would be.
    QUrl spoiled(url);
    spoiled.setPath(QStringLiteral("/000/monolist-selftest-missing_320.mp4"));
    qInfo("jiosaavn: %s handed over spoiled, for the self-test", qPrintable(videoId));
    return spoiled.toString(QUrl::FullyEncoded);
}

int StreamResolver::saavnKbps(const QString &videoId) const
{
    const auto known = m_saavnVerdicts.constFind(videoId);
    return known != m_saavnVerdicts.constEnd() && known->kind == SaavnVerdict::Match ? known->kbps : 0;
}

void StreamResolver::setSaavnIndiaHeaders(bool on, bool forgetNoMatches)
{
    if (on == m_saavn.indiaHeaders())
        return;
    m_saavn.setIndiaHeaders(on);
    if (!forgetNoMatches)
        return;
    for (auto it = m_saavnVerdicts.begin(); it != m_saavnVerdicts.end();) {
        if (it->kind == SaavnVerdict::NoMatch)
            it = m_saavnVerdicts.erase(it);
        else
            ++it;
    }
    QSqlDatabase db = AppDatabase::connection();
    if (db.isOpen()) {
        QSqlQuery query(db);
        query.exec(QStringLiteral("DELETE FROM saavn_matches WHERE matched = 0"));
    }
}

